#include "app/Config.hpp"
#include "core/Logger.hpp"
#include <shlobj.h>
#include <filesystem>

namespace LiteCapture::App {

RecordingProfile AppConfig::GetPresetProfile(ProfilePreset preset) {
    RecordingProfile p;
    switch (preset) {
        case ProfilePreset::Compact:
            p.name = L"Profile A - Compact / Small File Size (720p 30fps ~1.2Mbps, ~9MB/min)";
            p.width = 1280;
            p.height = 720;
            p.fps = 30;
            p.bitrateKbps = 1200;
            break;
        case ProfilePreset::Balanced:
            p.name = L"Profile B - Balanced (720p 30fps ~2.2Mbps, ~16MB/min)";
            p.width = 1280;
            p.height = 720;
            p.fps = 30;
            p.bitrateKbps = 2200;
            break;
        case ProfilePreset::SmoothGameplay:
            p.name = L"Profile C - Smooth Motion (720p 60fps ~4Mbps, ~30MB/min)";
            p.width = 1280;
            p.height = 720;
            p.fps = 60;
            p.bitrateKbps = 4000;
            break;
        case ProfilePreset::HigherDetail:
            p.name = L"Profile D - High Detail (768p 30fps ~5Mbps, ~37MB/min)";
            p.width = 1366;
            p.height = 768;
            p.fps = 30;
            p.bitrateKbps = 5000;
            break;
        case ProfilePreset::Custom:
            p.name = L"Custom Profile";
            p.width = 1280;
            p.height = 720;
            p.fps = 30;
            p.bitrateKbps = 1500;
            break;
    }
    return p;
}

std::wstring AppConfig::GetDefaultOutputDirectory() {
    PWSTR videoPath = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videoPath))) {
        result = (std::filesystem::path(videoPath) / L"LiteCapture").wstring();
        CoTaskMemFree(videoPath);
    } else {
        result = (std::filesystem::current_path() / L"Recordings").wstring();
    }
    return result;
}

std::wstring AppConfig::GetConfigFilePath() {
    PWSTR localAppPath = nullptr;
    std::filesystem::path configDir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppPath))) {
        configDir = std::filesystem::path(localAppPath) / L"LiteCapture";
        CoTaskMemFree(localAppPath);
    } else {
        configDir = std::filesystem::current_path();
    }
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    return (configDir / L"config.ini").wstring();
}

bool AppConfig::Load() {
    std::wstring configFile = GetConfigFilePath();
    if (!std::filesystem::exists(configFile)) {
        LC_LOG_INFO(L"Config file does not exist, initializing defaults.");
        outputDirectory = GetDefaultOutputDirectory();
        activeProfile = GetPresetProfile(ProfilePreset::Compact);
        configVersion = 2;
        Save();
        return true;
    }

    configVersion = GetPrivateProfileIntW(L"General", L"Version", 1, configFile.c_str());

    // Upgrade older v1 configuration to v2 with optimized compact bitrates
    if (configVersion < 2) {
        outputDirectory = GetDefaultOutputDirectory();
        wchar_t outDirBuffer[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"Storage", L"OutputDirectory", outputDirectory.c_str(), outDirBuffer, MAX_PATH, configFile.c_str());
        outputDirectory = outDirBuffer;

        configVersion = 2;
        profilePreset = ProfilePreset::Compact;
        activeProfile = GetPresetProfile(profilePreset);
        Save();
        LC_LOG_INFO(L"Upgraded configuration to version 2 with compact video recording presets.");
        return true;
    }

    int presetVal = GetPrivateProfileIntW(L"Video", L"Preset", 0, configFile.c_str());
    if (presetVal < 0 || presetVal > static_cast<int>(ProfilePreset::HigherDetail)) {
        presetVal = 0;
    }
    profilePreset = static_cast<ProfilePreset>(presetVal);
    activeProfile = GetPresetProfile(profilePreset);

    activeProfile.width = GetPrivateProfileIntW(L"Video", L"Width", activeProfile.width, configFile.c_str());
    activeProfile.height = GetPrivateProfileIntW(L"Video", L"Height", activeProfile.height, configFile.c_str());
    activeProfile.fps = GetPrivateProfileIntW(L"Video", L"Fps", activeProfile.fps, configFile.c_str());
    activeProfile.bitrateKbps = GetPrivateProfileIntW(L"Video", L"BitrateKbps", activeProfile.bitrateKbps, configFile.c_str());

    wchar_t outDirBuffer[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"Storage", L"OutputDirectory", GetDefaultOutputDirectory().c_str(), outDirBuffer, MAX_PATH, configFile.c_str());
    outputDirectory = outDirBuffer;

    captureSourceIndex = GetPrivateProfileIntW(L"Capture", L"SourceIndex", 0, configFile.c_str());
    recordCursor = GetPrivateProfileIntW(L"Capture", L"RecordCursor", 1, configFile.c_str()) != 0;

    captureSystemAudio = GetPrivateProfileIntW(L"Audio", L"CaptureSystemAudio", 1, configFile.c_str()) != 0;
    captureMicrophone = GetPrivateProfileIntW(L"Audio", L"CaptureMicrophone", 0, configFile.c_str()) != 0;

    wchar_t sysAudioDevBuf[256] = {0};
    GetPrivateProfileStringW(L"Audio", L"SystemAudioDeviceId", L"", sysAudioDevBuf, 256, configFile.c_str());
    systemAudioDeviceId = sysAudioDevBuf;

    wchar_t micDevBuf[256] = {0};
    GetPrivateProfileStringW(L"Audio", L"MicDeviceId", L"", micDevBuf, 256, configFile.c_str());
    micDeviceId = micDevBuf;

    int sysVolInt = GetPrivateProfileIntW(L"Audio", L"SystemVolume", 100, configFile.c_str());
    systemVolume = std::clamp(static_cast<float>(sysVolInt) / 100.0f, 0.0f, 2.0f);

    int micVolInt = GetPrivateProfileIntW(L"Audio", L"MicVolume", 100, configFile.c_str());
    micVolume = std::clamp(static_cast<float>(micVolInt) / 100.0f, 0.0f, 2.0f);

    hotkeyModifiers = GetPrivateProfileIntW(L"Hotkeys", L"Modifiers", MOD_CONTROL | MOD_SHIFT, configFile.c_str());
    hotkeyVkCode = GetPrivateProfileIntW(L"Hotkeys", L"VkCode", 0x52, configFile.c_str()); // 'R'

    minimizeToTrayOnClose = GetPrivateProfileIntW(L"UI", L"MinimizeToTray", 0, configFile.c_str()) != 0;

    // Ensure output directory exists
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);

    LC_LOG_INFO(std::format(L"Config loaded: Profile={} ({}x{} @ {}fps, {}kbps), OutputDir={}, SysVol={:.0f}%, MicVol={:.0f}%",
        activeProfile.name, activeProfile.width, activeProfile.height, activeProfile.fps, activeProfile.bitrateKbps, outputDirectory,
        systemVolume * 100.0f, micVolume * 100.0f));

    return true;
}

bool AppConfig::Save() const {
    std::wstring configFile = GetConfigFilePath();

    auto writeInt = [&](const wchar_t* sec, const wchar_t* key, int val) {
        WritePrivateProfileStringW(sec, key, std::to_wstring(val).c_str(), configFile.c_str());
    };

    writeInt(L"General", L"Version", configVersion);
    writeInt(L"Video", L"Preset", static_cast<int>(profilePreset));
    writeInt(L"Video", L"Width", activeProfile.width);
    writeInt(L"Video", L"Height", activeProfile.height);
    writeInt(L"Video", L"Fps", activeProfile.fps);
    writeInt(L"Video", L"BitrateKbps", activeProfile.bitrateKbps);

    WritePrivateProfileStringW(L"Storage", L"OutputDirectory", outputDirectory.c_str(), configFile.c_str());
    writeInt(L"Capture", L"SourceIndex", captureSourceIndex);
    writeInt(L"Capture", L"RecordCursor", recordCursor ? 1 : 0);

    writeInt(L"Audio", L"CaptureSystemAudio", captureSystemAudio ? 1 : 0);
    writeInt(L"Audio", L"CaptureMicrophone", captureMicrophone ? 1 : 0);
    WritePrivateProfileStringW(L"Audio", L"SystemAudioDeviceId", systemAudioDeviceId.c_str(), configFile.c_str());
    WritePrivateProfileStringW(L"Audio", L"MicDeviceId", micDeviceId.c_str(), configFile.c_str());
    writeInt(L"Audio", L"SystemVolume", static_cast<int>(std::round(systemVolume * 100.0f)));
    writeInt(L"Audio", L"MicVolume", static_cast<int>(std::round(micVolume * 100.0f)));

    writeInt(L"Hotkeys", L"Modifiers", hotkeyModifiers);
    writeInt(L"Hotkeys", L"VkCode", hotkeyVkCode);

    writeInt(L"UI", L"MinimizeToTray", minimizeToTrayOnClose ? 1 : 0);

    LC_LOG_INFO(L"Configuration saved successfully.");
    return true;
}

} // namespace LiteCapture::App
