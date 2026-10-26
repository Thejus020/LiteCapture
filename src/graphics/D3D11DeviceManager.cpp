#include "graphics/D3D11DeviceManager.hpp"
#include "core/Logger.hpp"
#include <format>

namespace LiteCapture::Graphics {

D3D11DeviceManager::D3D11DeviceManager() = default;

D3D11DeviceManager::~D3D11DeviceManager() {
    Shutdown();
}

Core::Result<std::vector<AdapterInfo>> D3D11DeviceManager::EnumerateAdapters() {
    m_adapters.clear();

    Core::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create DXGIFactory1 for adapter enumeration");
        return Core::Result<std::vector<AdapterInfo>>::FromHResult(hr, L"CreateDXGIFactory1");
    }

    UINT adapterIndex = 0;
    Core::ComPtr<IDXGIAdapter1> adapter;

    while (factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (SUCCEEDED(hr)) {
            AdapterInfo info{};
            info.description = desc.Description;
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.dedicatedVideoMemoryMb = desc.DedicatedVideoMemory / (1024 * 1024);
            info.dedicatedSystemMemoryMb = desc.DedicatedSystemMemory / (1024 * 1024);
            info.sharedSystemMemoryMb = desc.SharedSystemMemory / (1024 * 1024);
            info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

            // Enumerate connected display outputs
            UINT outputIndex = 0;
            Core::ComPtr<IDXGIOutput> output;
            while (adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND) {
                DXGI_OUTPUT_DESC outDesc{};
                if (SUCCEEDED(output->GetDesc(&outDesc))) {
                    DisplayOutputInfo dispInfo{};
                    dispInfo.deviceName = outDesc.DeviceName;
                    dispInfo.isAttachedToDesktop = outDesc.AttachedToDesktop != 0;
                    dispInfo.hMonitor = outDesc.Monitor;

                    int width = outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left;
                    int height = outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top;
                    dispInfo.width = width > 0 ? static_cast<uint32_t>(width) : 0;
                    dispInfo.height = height > 0 ? static_cast<uint32_t>(height) : 0;

                    // Query display mode refresh rate
                    DXGI_MODE_DESC modeDesc{};
                    modeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    modeDesc.Width = dispInfo.width;
                    modeDesc.Height = dispInfo.height;

                    DXGI_MODE_DESC closestMode{};
                    if (SUCCEEDED(output->FindClosestMatchingMode(&modeDesc, &closestMode, nullptr))) {
                        dispInfo.refreshRateNumerator = closestMode.RefreshRate.Numerator;
                        dispInfo.refreshRateDenominator = closestMode.RefreshRate.Denominator;
                    } else {
                        dispInfo.refreshRateNumerator = 60;
                        dispInfo.refreshRateDenominator = 1;
                    }

                    info.outputs.push_back(dispInfo);
                }
                output.Reset();
                outputIndex++;
            }

            LC_LOG_INFO(std::format(L"Adapter [{}]: {} | Outputs: {} | VRAM: {}MB | Shared: {}MB",
                adapterIndex, info.description, info.outputs.size(), info.dedicatedVideoMemoryMb, info.sharedSystemMemoryMb));

            m_adapters.push_back(std::move(info));
        }

        adapter.Reset();
        adapterIndex++;
    }

    return Core::Result<std::vector<AdapterInfo>>::Success(m_adapters);
}

Core::Status D3D11DeviceManager::Initialize(int preferredAdapterIndex) {
    Shutdown();

    // 1. Enumerate adapters first
    auto enumResult = EnumerateAdapters();
    if (enumResult.IsError()) {
        return enumResult.GetStatus();
    }

    if (m_adapters.empty()) {
        LC_LOG_ERROR(L"No DXGI display adapters found on the system.");
        return Core::Status::Error(Core::StatusCode::DeviceLoss, L"No DXGI display adapters available");
    }

    // 2. Select appropriate adapter
    Core::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create DXGIFactory1");
        return Core::Status::FromHResult(hr, L"CreateDXGIFactory1");
    }

    int selectedIndex = -1;
    if (preferredAdapterIndex >= 0 && preferredAdapterIndex < static_cast<int>(m_adapters.size())) {
        selectedIndex = preferredAdapterIndex;
    } else {
        // Prefer hardware adapter with active display outputs and greatest dedicated VRAM
        for (int i = 0; i < static_cast<int>(m_adapters.size()); ++i) {
            if (!m_adapters[i].isSoftware && !m_adapters[i].outputs.empty()) {
                selectedIndex = i;
                break;
            }
        }
        if (selectedIndex < 0) {
            // Fallback to first non-software adapter
            for (int i = 0; i < static_cast<int>(m_adapters.size()); ++i) {
                if (!m_adapters[i].isSoftware) {
                    selectedIndex = i;
                    break;
                }
            }
        }
        if (selectedIndex < 0) {
            selectedIndex = 0; // Final fallback
        }
    }

    hr = factory->EnumAdapters1(static_cast<UINT>(selectedIndex), &m_activeAdapterHandle);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to obtain handle for selected adapter");
        return Core::Status::FromHResult(hr, L"EnumAdapters1");
    }
    m_activeAdapterIndex = selectedIndex;

    LC_LOG_INFO(std::format(L"Initializing D3D11 on adapter [{}]: {}",
        m_activeAdapterIndex, m_adapters[m_activeAdapterIndex].description));

    // 3. Configure D3D11 Device creation flags
    // D3D11_CREATE_DEVICE_BGRA_SUPPORT is required for Windows Graphics Capture and Direct2D interop
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    // Attempt creation with selected adapter
    hr = D3D11CreateDevice(
        m_activeAdapterHandle.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // Must be UNKNOWN when passing an explicit IDXGIAdapter
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &m_featureLevel,
        &m_context
    );

#if defined(_DEBUG)
    // If debug layer is not installed, retry without D3D11_CREATE_DEVICE_DEBUG
    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        LC_LOG_WARN(L"D3D11 debug device creation failed (Graphics Tools may not be installed). Retrying without debug flag.");
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            m_activeAdapterHandle.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_device,
            &m_featureLevel,
            &m_context
        );
    }
#endif

    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create D3D11 Device on selected adapter");
        return Core::Status::FromHResult(hr, L"D3D11CreateDevice");
    }

    // 4. Enable multithread protection for concurrent capture and encoding safety
    hr = m_context.As(&m_multithread);
    if (SUCCEEDED(hr) && m_multithread) {
        m_multithread->SetMultithreadProtected(TRUE);
        LC_LOG_INFO(L"D3D11 device multithread protection enabled.");
    } else {
        LC_LOG_WARN(L"Could not query ID3D11Multithread interface.");
    }

    // 5. Query IDXGIDevice
    hr = m_device.As(&m_dxgiDevice);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to query IDXGIDevice from D3D11Device");
        return Core::Status::FromHResult(hr, L"Query IDXGIDevice");
    }

    std::wstring featureLevelStr;
    switch (m_featureLevel) {
        case D3D_FEATURE_LEVEL_11_1: featureLevelStr = L"11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: featureLevelStr = L"11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: featureLevelStr = L"10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: featureLevelStr = L"10.0"; break;
        default: featureLevelStr = L"Unknown"; break;
    }

    LC_LOG_INFO(std::format(L"D3D11 Device initialized successfully. Feature Level: {}", featureLevelStr));
    return Core::Status::Ok();
}

void D3D11DeviceManager::Shutdown() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_multithread.Reset();
    m_dxgiDevice.Reset();
    m_context.Reset();
    m_device.Reset();
    m_activeAdapterHandle.Reset();
    m_activeAdapterIndex = -1;
    LC_LOG_INFO(L"D3D11 Device resources released cleanly.");
}

Core::Status D3D11DeviceManager::CheckDeviceRemoved() {
    if (!m_device) {
        return Core::Status::Error(Core::StatusCode::NotInitialized, L"D3D11 device not initialized");
    }

    HRESULT reason = m_device->GetDeviceRemovedReason();
    if (FAILED(reason)) {
        LC_LOG_HR(reason, L"CRITICAL: Direct3D 11 graphics device was removed or reset by GPU driver");
        return Core::Status::Error(Core::StatusCode::DeviceLoss,
            std::format(L"GPU device removed: {}", Core::HResultToString(reason)), reason);
    }

    return Core::Status::Ok();
}

Core::Result<Core::ComPtr<ID3D11Texture2D>> D3D11DeviceManager::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D11_USAGE usage,
    UINT bindFlags,
    UINT cpuAccessFlags
) {
    if (!m_device) {
        return Core::Result<Core::ComPtr<ID3D11Texture2D>>::Fail(
            Core::StatusCode::NotInitialized, L"D3D11 Device is not initialized");
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = usage;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = cpuAccessFlags;
    desc.MiscFlags = 0;

    Core::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, std::format(L"Failed to create D3D11Texture2D ({}x{}, format {})", width, height, static_cast<int>(format)));
        return Core::Result<Core::ComPtr<ID3D11Texture2D>>::FromHResult(hr, L"CreateTexture2D");
    }

    return Core::Result<Core::ComPtr<ID3D11Texture2D>>::Success(texture);
}

} // namespace LiteCapture::Graphics
