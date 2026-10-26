#include "capture/WgcCaptureEngine.hpp"
#include "core/Logger.hpp"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <mutex>
#include <format>

namespace LiteCapture::Capture {

class WgcCaptureEngine::Impl {
public:
    Impl() = default;
    ~Impl() {
        StopCapture();
    }

    Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr) {
        if (!graphicsMgr || !graphicsMgr->IsInitialized()) {
            return Core::Status::Error(Core::StatusCode::NotInitialized, L"D3D11 Device Manager is not initialized");
        }

        m_graphicsMgr = graphicsMgr;

        // Wrap native D3D11 / DXGI device into WinRT IDirect3DDevice
        IDXGIDevice* dxgiDevice = m_graphicsMgr->GetDXGIDevice();
        if (!dxgiDevice) {
            return Core::Status::Error(Core::StatusCode::NotInitialized, L"IDXGIDevice is null");
        }

        winrt::com_ptr<::IInspectable> inspectableDevice;
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectableDevice.put());
        if (FAILED(hr)) {
            LC_LOG_HR(hr, L"Failed to create WinRT Direct3D device from DXGI device");
            return Core::Status::FromHResult(hr, L"CreateDirect3D11DeviceFromDXGIDevice");
        }

        m_winrtDevice = inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        LC_LOG_INFO(L"Windows Graphics Capture WinRT Direct3D device created.");
        return Core::Status::Ok();
    }

    Core::Status StartCapture(const CaptureSource& source, FrameArrivalCallback callback, bool captureCursor) {
        if (!m_graphicsMgr || !m_winrtDevice) {
            return Core::Status::Error(Core::StatusCode::NotInitialized, L"WGC Capture engine not initialized with Direct3D");
        }

        StopCapture();
        m_callback = std::move(callback);

        try {
            // Check if Windows Graphics Capture is supported on this machine
            if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
                LC_LOG_ERROR(L"Windows Graphics Capture is not supported on this OS or GPU.");
                return Core::Status::Error(Core::StatusCode::InternalError, L"Windows Graphics Capture unsupported");
            }

            // Create GraphicsCaptureItem via interop
            auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
            auto interop = factory.as<IGraphicsCaptureItemInterop>();

            winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
            HRESULT hr = S_OK;

            if (source.type == CaptureSourceType::Monitor && source.hMonitor) {
                hr = interop->CreateForMonitor(
                    source.hMonitor,
                    winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                    winrt::put_abi(item)
                );
            } else if (source.type == CaptureSourceType::Window && source.hWnd) {
                hr = interop->CreateForWindow(
                    source.hWnd,
                    winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                    winrt::put_abi(item)
                );
            } else {
                return Core::Status::Error(Core::StatusCode::InvalidParameter, L"Invalid capture source handle");
            }

            if (FAILED(hr) || !item) {
                LC_LOG_HR(hr, L"Failed to create GraphicsCaptureItem");
                return Core::Status::FromHResult(hr, L"CreateForMonitor/Window");
            }

            m_item = item;
            m_lastSize = m_item.Size();

            LC_LOG_INFO(std::format(L"Starting WGC capture on '{}' ({}x{})",
                source.name, m_lastSize.Width, m_lastSize.Height));

            // Create free-threaded capture frame pool (2 frames to minimize latency and memory)
            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                m_lastSize
            );

            // Hook frame arrived event
            m_frameArrivedToken = m_framePool.FrameArrived([this](auto&& sender, auto&&) {
                OnFrameArrived(sender);
            });

            // Hook item closed event (e.g. window closed or monitor unplugged)
            m_itemClosedToken = m_item.Closed([this](auto&&, auto&&) {
                LC_LOG_WARN(L"WGC Capture item closed or destroyed.");
                StopCapture();
            });

            // Create and start session
            m_session = m_framePool.CreateCaptureSession(m_item);

            // Configure cursor capture
            try {
                m_session.IsCursorCaptureEnabled(captureCursor);
            } catch (...) {
                // Ignore if not supported on Windows 10 < 2004
            }

            // Disable yellow capture border on Windows 11 (build 22000+)
            try {
                m_session.IsBorderRequired(false);
            } catch (...) {
                // Not supported on older Windows builds
            }

            m_session.StartCapture();
            m_isCapturing = true;

            m_framesCaptured = 0;
            m_framesDropped = 0;
            m_fpsCounter = 0;
            m_lastFrameTimeNs = 0;
            m_lastFpsCalcTimeNs = Core::GetCurrentTimeNs();

            LC_LOG_INFO(L"WGC capture session started successfully.");
            return Core::Status::Ok();

        } catch (const winrt::hresult_error& ex) {
            LC_LOG_HR(ex.code(), std::format(L"WinRT exception during capture start: {}", ex.message().c_str()));
            StopCapture();
            return Core::Status::FromHResult(ex.code(), ex.message().c_str());
        } catch (const std::exception& ex) {
            LC_LOG_ERROR(std::format(L"Standard exception during capture start: {}", Core::Utf8ToUtf16(ex.what())));
            StopCapture();
            return Core::Status::Error(Core::StatusCode::InternalError, Core::Utf8ToUtf16(ex.what()));
        }
    }

    void StopCapture() {
        if (!m_isCapturing.exchange(false)) {
            return;
        }

        LC_LOG_INFO(std::format(L"Stopping WGC capture. Total captured: {}, Dropped: {}",
            m_framesCaptured.load(), m_framesDropped.load()));

        if (m_session) {
            try {
                m_session.Close();
            } catch (...) {}
            m_session = nullptr;
        }

        if (m_framePool) {
            try {
                m_framePool.FrameArrived(m_frameArrivedToken);
                m_framePool.Close();
            } catch (...) {}
            m_framePool = nullptr;
        }

        if (m_item) {
            try {
                m_item.Closed(m_itemClosedToken);
            } catch (...) {}
            m_item = nullptr;
        }

        m_callback = nullptr;
        LC_LOG_INFO(L"WGC capture session stopped and resources released.");
    }

    bool IsCapturing() const { return m_isCapturing.load(); }

    CaptureStats GetStats() const {
        CaptureStats stats{};
        stats.framesCaptured = m_framesCaptured.load();
        stats.framesDropped = m_framesDropped.load();
        stats.captureFps = m_currentFps.load();
        stats.currentWidth = static_cast<uint32_t>(m_lastSize.Width);
        stats.currentHeight = static_cast<uint32_t>(m_lastSize.Height);
        return stats;
    }

private:
    void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender) {
        if (!m_isCapturing.load()) return;

        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            // Handle dynamic resolution changes (e.g. window resize or display mode switch)
            auto contentSize = frame.ContentSize();
            if (contentSize.Width != m_lastSize.Width || contentSize.Height != m_lastSize.Height) {
                LC_LOG_INFO(std::format(L"Capture source resized: {}x{} -> {}x{}",
                    m_lastSize.Width, m_lastSize.Height, contentSize.Width, contentSize.Height));
                m_lastSize = contentSize;
                m_framePool.Recreate(
                    m_winrtDevice,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    m_lastSize
                );
                return;
            }

            // Extract underlying Direct3D 11 surface texture
            auto surface = frame.Surface();
            auto interopAccess = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

            Core::ComPtr<ID3D11Texture2D> texture;
            HRESULT hr = interopAccess->GetInterface(IID_PPV_ARGS(&texture));
            if (FAILED(hr) || !texture) {
                m_framesDropped++;
                return;
            }

            int64_t nowNs = Core::GetCurrentTimeNs();
            int64_t durationNs = m_lastFrameTimeNs > 0 ? (nowNs - m_lastFrameTimeNs) : (1000000000LL / 60);
            m_lastFrameTimeNs = nowNs;

            m_framesCaptured++;
            m_fpsCounter++;

            // Update FPS calculation every second
            if (nowNs - m_lastFpsCalcTimeNs >= 1000000000LL) {
                double elapsedSec = static_cast<double>(nowNs - m_lastFpsCalcTimeNs) / 1000000000.0;
                m_currentFps = static_cast<double>(m_fpsCounter.load()) / elapsedSec;
                m_fpsCounter = 0;
                m_lastFpsCalcTimeNs = nowNs;
            }

            if (m_callback) {
                CapturedFrame capFrame{};
                capFrame.texture = texture;
                capFrame.timestampNs = nowNs;
                capFrame.durationNs = durationNs;
                capFrame.width = static_cast<uint32_t>(contentSize.Width);
                capFrame.height = static_cast<uint32_t>(contentSize.Height);
                capFrame.frameIndex = m_framesCaptured.load();

                m_callback(capFrame);
            }

        } catch (const std::exception& ex) {
            LC_LOG_ERROR(std::format(L"Exception in OnFrameArrived: {}", Core::Utf8ToUtf16(ex.what())));
            m_framesDropped++;
        } catch (...) {
            m_framesDropped++;
        }
    }

    Graphics::D3D11DeviceManager* m_graphicsMgr{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};

    winrt::event_token m_frameArrivedToken{};
    winrt::event_token m_itemClosedToken{};
    winrt::Windows::Graphics::SizeInt32 m_lastSize{0, 0};

    FrameArrivalCallback m_callback;
    std::atomic<bool> m_isCapturing{false};

    std::atomic<uint64_t> m_framesCaptured{0};
    std::atomic<uint64_t> m_framesDropped{0};
    std::atomic<uint32_t> m_fpsCounter{0};
    std::atomic<double> m_currentFps{0.0};
    int64_t m_lastFrameTimeNs{0};
    int64_t m_lastFpsCalcTimeNs{0};
};

WgcCaptureEngine::WgcCaptureEngine() : m_impl(std::make_unique<Impl>()) {}
WgcCaptureEngine::~WgcCaptureEngine() = default;

Core::Status WgcCaptureEngine::Initialize(Graphics::D3D11DeviceManager* graphicsMgr) {
    return m_impl->Initialize(graphicsMgr);
}

Core::Status WgcCaptureEngine::StartCapture(const CaptureSource& source, FrameArrivalCallback callback, bool captureCursor) {
    return m_impl->StartCapture(source, std::move(callback), captureCursor);
}

void WgcCaptureEngine::StopCapture() {
    m_impl->StopCapture();
}

bool WgcCaptureEngine::IsCapturing() const {
    return m_impl->IsCapturing();
}

CaptureStats WgcCaptureEngine::GetStats() const {
    return m_impl->GetStats();
}

} // namespace LiteCapture::Capture
