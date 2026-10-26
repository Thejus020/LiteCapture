#pragma once

#include "core/Common.hpp"
#include "core/Result.hpp"
#include "audio/AudioTypes.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

namespace LiteCapture::Audio {

class WasapiAudioCapture {
public:
    WasapiAudioCapture();
    ~WasapiAudioCapture();

    Core::Status Initialize(const AudioConfig& config);
    Core::Status Start(AudioFrameCallback callback);
    void Stop();
    bool IsCapturing() const { return m_isCapturing.load(); }
    AudioStats GetStats() const;
    void SetVolume(float systemVolume, float micVolume);

    // Enumerate active audio devices for user selection in the UI
    static std::vector<AudioDeviceInfo> EnumerateEndpoints(EDataFlow dataFlow);
    static std::vector<AudioDeviceInfo> EnumeratePlaybackDevices() { return EnumerateEndpoints(eRender); }
    static std::vector<AudioDeviceInfo> EnumerateRecordingDevices() { return EnumerateEndpoints(eCapture); }

private:
    struct AudioEndpoint {
        Core::ComPtr<IMMDevice> device;
        Core::ComPtr<IAudioClient> client;
        Core::ComPtr<IAudioCaptureClient> captureClient;
        HANDLE eventHandle{nullptr};
        WAVEFORMATEX* waveFormat{nullptr};
        bool isFloat{false};
        uint32_t channels{2};
        uint32_t sampleRate{48000};
        uint32_t bitsPerSample{32};
        bool isInitialized{false};
    };

    Core::Status SetupEndpoint(EDataFlow dataFlow, const std::wstring& preferredDeviceId, DWORD streamFlags, AudioEndpoint& endpoint);
    void ReleaseEndpoint(AudioEndpoint& endpoint);
    void CaptureThreadProc();

    // Helper to convert arbitrary WASAPI packet into canonical 48kHz float stereo
    void ConvertToCanonicalFloatStereo(
        const BYTE* srcData,
        uint32_t numFrames,
        const AudioEndpoint& endpoint,
        std::vector<float>& outStereoFloat,
        bool isSilent
    );

    AudioConfig m_config;
    AudioFrameCallback m_callback;
    AudioEndpoint m_systemEndpoint;
    AudioEndpoint m_micEndpoint;

    std::thread m_captureThread;
    std::atomic<bool> m_isCapturing{false};
    std::atomic<bool> m_stopRequested{false};
    HANDLE m_stopEvent{nullptr};

    std::atomic<uint64_t> m_systemFramesCaptured{0};
    std::atomic<uint64_t> m_micFramesCaptured{0};
    std::atomic<uint64_t> m_totalSamplesMixed{0};

    std::atomic<float> m_systemVolume{1.0f};
    std::atomic<float> m_micVolume{1.0f};

    mutable std::mutex m_mutex;
};

} // namespace LiteCapture::Audio
