#include "app/App.hpp"
#include "core/Logger.hpp"
#include <commctrl.h>
#include <objbase.h>
#include <filesystem>

namespace LiteCapture::App {

App::App(HINSTANCE hInstance) : m_hInstance(hInstance) {}

App::~App() {
    Shutdown();
}

bool App::Initialize(int nCmdShow, bool testCaptureMode) {
    m_testCaptureMode = testCaptureMode;

    // Initialize COM for UI dialogs and future WGC / MF calls
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        OutputDebugStringW(L"[FATAL] Failed to initialize COM library.\n");
        return false;
    }

    // Initialize Common Controls v6
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // Initialize Structured Logger
    Core::Logger::Instance().Initialize();
    LC_LOG_INFO(L"LiteCapture initializing — Phase 6 Screen & Audio Recording (WASAPI + AAC).");

    // Initialize Direct3D 11 Graphics Device Manager
    m_graphicsManager = std::make_unique<Graphics::D3D11DeviceManager>();
    auto gfxStatus = m_graphicsManager->Initialize();
    if (gfxStatus.IsError()) {
        LC_LOG_ERROR(std::format(L"Failed to initialize Direct3D 11: {}", gfxStatus.Message()));
    }

    // Initialize Recording Controller
    m_recordingController = std::make_unique<RecordingController>();
    auto recStatus = m_recordingController->Initialize(m_graphicsManager.get());
    if (recStatus.IsError()) {
        LC_LOG_ERROR(std::format(L"Failed to initialize RecordingController: {}", recStatus.Message()));
    }

    // Load user configuration
    if (!m_config.Load()) {
        LC_LOG_WARN(L"Failed to load user config, proceeding with defaults.");
    }

    // Create Main UI Window
    m_mainWindow = std::make_unique<UI::MainWindow>(m_hInstance, m_config);
    if (!m_mainWindow->Create()) {
        LC_LOG_FATAL(L"Failed to create main application window.");
        return false;
    }

    // Connect RecordingController to MainWindow
    m_mainWindow->SetRecordingController(m_recordingController.get());

    // Pass detected GPU hardware and displays to UI
    if (m_graphicsManager && m_graphicsManager->IsInitialized()) {
        const auto* adapterInfo = m_graphicsManager->GetActiveAdapterInfo();
        if (adapterInfo) {
            std::wstring gpuText = std::format(L"GPU: {}", adapterInfo->GetSummary());
            m_mainWindow->SetGpuInfo(gpuText);
            m_mainWindow->SetDisplaySources(adapterInfo->outputs);
        }
    } else {
        m_mainWindow->SetGpuInfo(L"GPU: Direct3D 11 initialization failed");
    }

    // Register global hotkey (Ctrl + Shift + R)
    if (RegisterHotKey(m_mainWindow->GetHwnd(), HOTKEY_ID_START_STOP, m_config.hotkeyModifiers, m_config.hotkeyVkCode)) {
        m_hotkeyRegistered = true;
        LC_LOG_INFO(L"Global start/stop recording hotkey (Ctrl+Shift+R) registered.");
    } else {
        LC_LOG_WARN(L"Failed to register global hotkey (Ctrl+Shift+R). Error: " + std::to_wstring(GetLastError()));
    }

    // Show Main Window (or hide in test mode)
    m_mainWindow->Show(m_testCaptureMode ? SW_HIDE : nCmdShow);
    m_initialized = true;

    LC_LOG_INFO(L"LiteCapture initialized successfully.");
    return true;
}

int App::Run() {
    if (!m_initialized) return -1;

    if (m_testCaptureMode) {
        LC_LOG_INFO(L"=== Running Automated Capture, Audio & Encoding Self-Test (Phase 6) ===");
        Capture::CaptureSource src{};
        const auto* adapter = m_graphicsManager->GetActiveAdapterInfo();
        if (adapter && !adapter->outputs.empty()) {
            src.type = Capture::CaptureSourceType::Monitor;
            src.hMonitor = adapter->outputs[0].hMonitor;
            src.name = adapter->outputs[0].deviceName;
            src.width = adapter->outputs[0].width;
            src.height = adapter->outputs[0].height;
        } else {
            src.type = Capture::CaptureSourceType::Monitor;
            src.hMonitor = MonitorFromWindow(m_mainWindow->GetHwnd(), MONITOR_DEFAULTTOPRIMARY);
            src.name = L"Primary Monitor";
        }

        auto status = m_recordingController->StartRecording(m_config, src);
        if (status.IsError()) {
            LC_LOG_ERROR(std::format(L"Automated capture start failed: {}", status.Message()));
            return 1;
        }

        std::wstring recPath = m_recordingController->GetCurrentRecordingPath();

        // Pump messages for 3 seconds while capturing and encoding frames and audio
        auto start = Core::Clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(Core::Clock::now() - start).count() < 3) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(20);
        }

        auto capStats = m_recordingController->GetStats();
        auto encStats = m_recordingController->GetEncoderStats();
        auto audStats = m_recordingController->GetAudioStats();
        LC_LOG_INFO(std::format(L"Automated Capture Test: VideoFrames={}, Dropped={}, VideoFPS={:.1f}, EncodedVideo={}, EncodedAudioFrames={}, SystemAudioActive={}, MicActive={}",
            capStats.framesCaptured, capStats.framesDropped, capStats.captureFps,
            encStats.framesEncoded, encStats.audioFramesEncoded,
            audStats.isSystemAudioActive, audStats.isMicActive));

        m_recordingController->StopRecording();

        // Verify output MP4 file exists and has non-zero size
        std::error_code ec;
        uintmax_t fileSize = std::filesystem::file_size(recPath, ec);
        if (!ec && fileSize > 0) {
            LC_LOG_INFO(std::format(L"=== Automated Capture & Encoding COMPLETED SUCCESSFULLY! Output='{}', Size={} bytes ({:.2f} MB) ===",
                recPath, fileSize, static_cast<double>(fileSize) / (1024.0 * 1024.0)));
            return 0;
        } else {
            LC_LOG_ERROR(std::format(L"Automated Capture Test FAILED: MP4 output file '{}' missing or empty! Error: {}",
                recPath, ec ? Core::Utf8ToUtf16(ec.message()) : L"File size is 0"));
            return 2;
        }
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_ID_START_STOP) {
            LC_LOG_INFO(L"Global recording hotkey triggered.");
            SendMessageW(m_mainWindow->GetHwnd(), WM_COMMAND, MAKEWPARAM(1001, BN_CLICKED), 0);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

void App::Shutdown() {
    if (!m_initialized) return;

    if (m_hotkeyRegistered && m_mainWindow && m_mainWindow->GetHwnd()) {
        UnregisterHotKey(m_mainWindow->GetHwnd(), HOTKEY_ID_START_STOP);
        m_hotkeyRegistered = false;
    }

    if (m_recordingController) {
        m_recordingController->Shutdown();
        m_recordingController.reset();
    }

    if (m_graphicsManager) {
        m_graphicsManager->Shutdown();
        m_graphicsManager.reset();
    }

    m_mainWindow.reset();

    LC_LOG_INFO(L"LiteCapture shutting down cleanly.");
    Core::Logger::Instance().Shutdown();
    CoUninitialize();
    m_initialized = false;
}

} // namespace LiteCapture::App
