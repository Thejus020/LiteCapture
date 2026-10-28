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

    struct AudioRingBuffer {
        std::vector<float> buffer;
        size_t capacity{0};
        size_t head{0};
        size_t tail{0};
        size_t count{0};

        void Init(size_t cap) {
            capacity = cap;
            buffer.assign(cap, 0.0f);
            head = 0;
            tail = 0;
            count = 0;
        }

        void Clear() noexcept {
            head = 0;
            tail = 0;
            count = 0;
        }

        size_t Available() const noexcept { return count; }
        size_t FreeSpace() const noexcept { return capacity - count; }

        void Push(const float* data, size_t numSamples) noexcept {
            if (!data || numSamples == 0) return;
            if (numSamples > capacity) {
                data += (numSamples - capacity);
                numSamples = capacity;
                head = 0;
                tail = 0;
                count = 0;
            } else if (numSamples > FreeSpace()) {
                size_t overflow = numSamples - FreeSpace();
                head = (head + overflow) % capacity;
                count -= overflow;
            }

            size_t part1 = (std::min)(numSamples, capacity - tail);
            memcpy(&buffer[tail], data, part1 * sizeof(float));
            size_t part2 = numSamples - part1;
            if (part2 > 0) {
                memcpy(&buffer[0], data + part1, part2 * sizeof(float));
            }
            tail = (tail + numSamples) % capacity;
            count += numSamples;
        }

        void Pop(float* dest, size_t numSamples) noexcept {
            if (!dest || numSamples == 0 || count == 0) return;
            numSamples = (std::min)(numSamples, count);

            size_t part1 = (std::min)(numSamples, capacity - head);
            memcpy(dest, &buffer[head], part1 * sizeof(float));
            size_t part2 = numSamples - part1;
            if (part2 > 0) {
                memcpy(dest + part1, &buffer[0], part2 * sizeof(float));
            }
            head = (head + numSamples) % capacity;
            count -= numSamples;
        }

        void Discard(size_t numSamples) noexcept {
            numSamples = (std::min)(numSamples, count);
            head = (head + numSamples) % capacity;
            count -= numSamples;
        }
    };

    Core::Status SetupEndpoint(EDataFlow dataFlow, const std::wstring& preferredDeviceId, DWORD streamFlags, AudioEndpoint& endpoint);
    void ReleaseEndpoint(AudioEndpoint& endpoint);
    void CaptureThreadProc();

    // Helper to convert arbitrary WASAPI packet into canonical 48kHz float stereo
    void ConvertToCanonicalFloatStereo(
        const BYTE* srcData,
        uint32_t numFrames,
        const AudioEndpoint& endpoint,
        AudioRingBuffer& outRing,
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
