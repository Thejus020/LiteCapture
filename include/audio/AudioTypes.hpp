#pragma once

#include "core/Common.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace LiteCapture::Audio {

struct AudioDeviceInfo {
    std::wstring id;       // Endpoint device ID or empty string for default
    std::wstring name;     // Friendly user-visible name (e.g. "Speakers (Realtek Audio)")
    bool isDefault{false};
};

struct AudioConfig {
    bool captureSystemAudio{true};
    bool captureMicrophone{false};
    std::wstring systemDeviceId;   // Empty = default system playback device
    std::wstring micDeviceId;      // Empty = default microphone recording device
    uint32_t sampleRate{48000};    // 48 kHz standard for modern video & gaming
    uint32_t channels{2};          // Stereo
    uint32_t bitrateBps{128000};   // 128 kbps AAC
    float systemVolume{1.0f};
    float micVolume{1.0f};

    bool IsAudioEnabled() const {
        return captureSystemAudio || captureMicrophone;
    }
};

// Callback returning interleaved 16-bit PCM stereo samples at 48000 Hz
using AudioFrameCallback = std::function<void(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs)>;

struct AudioStats {
    uint64_t systemFramesCaptured{0};
    uint64_t micFramesCaptured{0};
    uint64_t totalSamplesMixed{0};
    bool isSystemAudioActive{false};
    bool isMicActive{false};
};

} // namespace LiteCapture::Audio
