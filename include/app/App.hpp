#pragma once

#include "core/Common.hpp"
#include "app/Config.hpp"
#include "ui/MainWindow.hpp"
#include "graphics/D3D11DeviceManager.hpp"
#include "app/RecordingController.hpp"
#include <memory>

namespace LiteCapture::App {

constexpr int HOTKEY_ID_START_STOP = 9001;

class App {
public:
    App(HINSTANCE hInstance);
    ~App();

    bool Initialize(int nCmdShow, bool testCaptureMode = false);
    int Run();
    void Shutdown();

    AppConfig& GetConfig() { return m_config; }
    UI::MainWindow* GetMainWindow() { return m_mainWindow.get(); }
    Graphics::D3D11DeviceManager* GetGraphicsManager() { return m_graphicsManager.get(); }
    RecordingController* GetRecordingController() { return m_recordingController.get(); }

private:
    HINSTANCE m_hInstance{nullptr};
    AppConfig m_config;
    std::unique_ptr<UI::MainWindow> m_mainWindow;
    std::unique_ptr<Graphics::D3D11DeviceManager> m_graphicsManager;
    std::unique_ptr<RecordingController> m_recordingController;
    bool m_initialized{false};
    bool m_hotkeyRegistered{false};
    bool m_testCaptureMode{false};
};

} // namespace LiteCapture::App
