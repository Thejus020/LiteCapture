#pragma once

#include "core/Common.hpp"
#include "app/Config.hpp"
#include "graphics/AdapterInfo.hpp"
#include "audio/AudioTypes.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <vector>

namespace LiteCapture::App {
class RecordingController;
}

namespace LiteCapture::UI {

// Custom message definitions
constexpr UINT WM_APP_RECORDING_STATE_CHANGED = WM_APP + 1;
constexpr UINT WM_APP_UPDATE_STATS            = WM_APP + 2;
constexpr UINT WM_APP_TRAY_NOTIFICATION       = WM_APP + 3;

class MainWindow {
public:
    MainWindow(HINSTANCE hInstance, App::AppConfig& config);
    ~MainWindow();

    bool Create();
    void Show(int nCmdShow);
    HWND GetHwnd() const { return m_hwnd; }

    void SetRecordingController(App::RecordingController* controller) { m_controller = controller; }
    void SetStatusText(const std::wstring& status);
    void SetGpuInfo(const std::wstring& gpuInfo);
    void SetDisplaySources(const std::vector<Graphics::DisplayOutputInfo>& displays);
    void UpdateRecordingStats(uint64_t durationSec, double fps, uint64_t droppedFrames, const std::wstring& encoderName);

    void ShowTrayNotification(const std::wstring& title, const std::wstring& message, DWORD infoFlags = NIIF_INFO);
    void MinimizeToTray();
    void RestoreFromTray();

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void LayoutControls();
    void UpdateFonts();
    void ApplyTheme();
    void PopulateSources();
    void PopulateProfiles();
    void PopulateAudioDevices();
    void OnBrowseOutputDir();
    void OnProfileChanged();
    void OnAudioCheckboxChanged();
    void OnSpeakerDeviceChanged();
    void OnMicDeviceChanged();
    void OnSpeakerVolumeChanged(int pos);
    void OnMicVolumeChanged(int pos);
    void AdjustSpeakerVolume(int delta);
    void AdjustMicVolume(int delta);
    void OnMinimizeToTrayChanged();
    void OnToggleRecording();

    void InitTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon(bool isRecording);
    void ShowTrayContextMenu();

    HINSTANCE m_hInstance{nullptr};
    HWND m_hwnd{nullptr};
    App::AppConfig& m_config;

    // Controls
    HWND m_lblStatusHeader{nullptr};
    HWND m_lblSource{nullptr};
    HWND m_cmbSource{nullptr};
    HWND m_lblProfile{nullptr};
    HWND m_cmbProfile{nullptr};
    HWND m_lblOutputDir{nullptr};
    HWND m_txtOutputDir{nullptr};
    HWND m_btnBrowseDir{nullptr};
    HWND m_chkSystemAudio{nullptr};
    HWND m_cmbSpeaker{nullptr};
    HWND m_lblSpeakerVol{nullptr};
    HWND m_btnSpeakerVolDown{nullptr};
    HWND m_btnSpeakerVolUp{nullptr};
    HWND m_sldSpeakerVol{nullptr};
    HWND m_chkMicrophone{nullptr};
    HWND m_cmbMic{nullptr};
    HWND m_lblMicVol{nullptr};
    HWND m_btnMicVolDown{nullptr};
    HWND m_btnMicVolUp{nullptr};
    HWND m_sldMicVol{nullptr};
    HWND m_chkMinimizeToTray{nullptr};
    HWND m_btnRecord{nullptr};
    HWND m_btnStop{nullptr};
    HWND m_lblStats{nullptr};
    HWND m_lblEncoderInfo{nullptr};
    HWND m_lblGpuInfo{nullptr};

    HFONT m_hFontRegular{nullptr};
    HFONT m_hFontBold{nullptr};
    HFONT m_hFontHeader{nullptr};
    HBRUSH m_hBgBrush{nullptr};

    UINT m_dpi{96};
    bool m_isRecording{false};
    uint64_t m_recordDurationSec{0};

    // System Tray
    NOTIFYICONDATAW m_nid{};
    bool m_trayIconCreated{false};
    HICON m_hIconApp{nullptr};
    HICON m_hIconRecording{nullptr};

    App::RecordingController* m_controller{nullptr};
    std::vector<Graphics::DisplayOutputInfo> m_displays;
    std::vector<Audio::AudioDeviceInfo> m_playbackDevices;
    std::vector<Audio::AudioDeviceInfo> m_recordingDevices;
};

} // namespace LiteCapture::UI
