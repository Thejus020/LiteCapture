#pragma once

#include "core/Common.hpp"
#include "core/Result.hpp"
#include "app/Config.hpp"
#include "graphics/D3D11DeviceManager.hpp"
#include "capture/CaptureTypes.hpp"
#include "capture/WgcCaptureEngine.hpp"
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>

#include "encoder/MfVideoEncoder.hpp"
#include "audio/WasapiAudioCapture.hpp"

namespace LiteCapture::App {

enum class RecordingState {
    Idle = 0,
    Initializing,
    Recording,
    Stopping,
    Finalizing,
    Error
};

inline std::wstring_view StateToString(RecordingState state) {
    switch (state) {
        case RecordingState::Idle:         return L"Idle";
        case RecordingState::Initializing: return L"Initializing";
        case RecordingState::Recording:    return L"Recording";
        case RecordingState::Stopping:     return L"Stopping";
        case RecordingState::Finalizing:   return L"Finalizing";
        case RecordingState::Error:        return L"Error";
        default:                           return L"Unknown";
    }
}

using StateChangeCallback = std::function<void(RecordingState newState, const std::wstring& message)>;

class RecordingController {
public:
    RecordingController();
    ~RecordingController();

    Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr);
    void Shutdown();

    Core::Status StartRecording(const AppConfig& config, const Capture::CaptureSource& source);
    Core::Status StopRecording();

    RecordingState GetState() const;
    Capture::CaptureStats GetStats() const;
    Encoder::EncoderStats GetEncoderStats() const;
    Audio::AudioStats GetAudioStats() const;
    uint64_t GetDurationSec() const;
    std::wstring GetCurrentRecordingPath() const;
    void SetAudioVolume(float systemVolume, float micVolume);

    void SetStateChangeCallback(StateChangeCallback callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stateCallback = std::move(callback);
    }

private:
    struct QueuedFrame {
        Core::ComPtr<ID3D11Texture2D> texture;
        int64_t timestampNs{0};
        int64_t durationNs{0};
        uint32_t width{0};
        uint32_t height{0};
        uint64_t frameIndex{0};
    };

    void SetState(RecordingState newState, const std::wstring& message = {});
    void OnCapturedFrame(const Capture::CapturedFrame& frame);
    void OnAudioFrame(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs);
    void EncoderThreadProc();

    static constexpr size_t MAX_QUEUE_DEPTH = 3;
    static constexpr size_t TEXTURE_POOL_SIZE = 4;

    mutable std::mutex m_mutex;
    RecordingState m_state{RecordingState::Idle};
    StateChangeCallback m_stateCallback;

    Graphics::D3D11DeviceManager* m_graphicsMgr{nullptr};
    std::unique_ptr<Capture::ICaptureEngine> m_captureEngine;
    std::unique_ptr<Encoder::IVideoEncoder> m_encoder;
    std::unique_ptr<Audio::WasapiAudioCapture> m_audioCapture;

    // Decoupled asynchronous encoding pipeline
    std::vector<Core::ComPtr<ID3D11Texture2D>> m_texturePool;
    std::queue<QueuedFrame> m_frameQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::thread m_encoderThread;
    std::atomic<bool> m_encoderThreadRunning{false};
    std::atomic<uint64_t> m_framesDroppedQueue{0};

    uint32_t m_targetWidth{1280};
    uint32_t m_targetHeight{720};

    int64_t m_recordStartTimeNs{0};
    std::atomic<uint64_t> m_framesReceived{0};
    std::atomic<uint64_t> m_audioFramesReceived{0};
    std::wstring m_activeRecordingPath;
};

} // namespace LiteCapture::App
