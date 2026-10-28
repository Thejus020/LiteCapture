#pragma once

#include "core/Common.hpp"
#include <string>
#include <cstdint>

namespace LiteCapture::App {

enum class ProfilePreset {
    Compact = 0,        // 1280x720, 30 FPS, 1200 kbps (~9 MB/min - Smallest file size)
    LowOverhead = 0,    // Backward compatibility alias
    Balanced,           // 1280x720, 30 FPS, 2200 kbps (~16 MB/min - Standard / Recommended)
    SmoothGameplay,     // 1280x720, 60 FPS, 4000 kbps (~30 MB/min - Smooth 60fps)
    HigherDetail,       // 1366x768, 30 FPS, 5000 kbps (~37 MB/min - High Detail)
    Custom              // User defined
};

struct RecordingProfile {
    std::wstring name;
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t fps{30};
    uint32_t bitrateKbps{1200}; // in kbps (e.g., 1200 kbps = 1.2 Mbps for compact recordings)

    uint32_t BitrateBps() const { return bitrateKbps * 1000; }
};

struct AppConfig {
    uint32_t configVersion{2};

    // Video Profile
    ProfilePreset profilePreset{ProfilePreset::Compact};
    RecordingProfile activeProfile;

    // Output Directory
    std::wstring outputDirectory;

    // Capture Source
    uint32_t captureSourceIndex{0}; // 0 = Primary display, etc.
    bool recordCursor{true};

    // Audio Settings
    bool captureSystemAudio{true};
    bool captureMicrophone{false};
    std::wstring systemAudioDeviceId; // Empty = default playback device
    std::wstring micDeviceId;         // Empty = default microphone recording device
    float systemVolume{1.0f};         // 0.0f to 2.0f (0% to 200%, default 100%)
    float micVolume{1.0f};            // 0.0f to 2.0f (0% to 200%, default 100%)

    // Hotkeys
    UINT hotkeyModifiers{MOD_CONTROL | MOD_SHIFT};
    UINT hotkeyVkCode{0x52}; // 'R' key (Ctrl+Shift+R)

    // UI preferences
    bool minimizeToTrayOnClose{false};

    static RecordingProfile GetPresetProfile(ProfilePreset preset);
    static std::wstring GetDefaultOutputDirectory();
    static std::wstring GetConfigFilePath();

    bool Load();
    bool Save() const;
};

} // namespace LiteCapture::App
