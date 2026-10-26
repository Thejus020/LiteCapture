#include "app/RecordingController.hpp"
#include "core/Logger.hpp"
#include <filesystem>
#include <format>
#include <future>
#include <objbase.h>

namespace LiteCapture::App {

RecordingController::RecordingController()
    : m_captureEngine(std::make_unique<Capture::WgcCaptureEngine>()),
      m_encoder(std::make_unique<Encoder::MfVideoEncoder>()),
      m_audioCapture(std::make_unique<Audio::WasapiAudioCapture>()) {}

RecordingController::~RecordingController() {
    Shutdown();
}

Core::Status RecordingController::Initialize(Graphics::D3D11DeviceManager* graphicsMgr) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!graphicsMgr || !graphicsMgr->IsInitialized()) {
        SetState(RecordingState::Error, L"Graphics device not initialized");
        return Core::Status::Error(Core::StatusCode::NotInitialized, L"Graphics manager not initialized");
    }

    m_graphicsMgr = graphicsMgr;
    auto status = m_captureEngine->Initialize(m_graphicsMgr);
    if (status.IsError()) {
        SetState(RecordingState::Error, status.Message());
        return status;
    }

    SetState(RecordingState::Idle, L"Ready to record");
    LC_LOG_INFO(L"RecordingController initialized successfully.");
    return Core::Status::Ok();
}

void RecordingController::Shutdown() {
    StopRecording();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_audioCapture) {
        m_audioCapture->Stop();
    }
    if (m_captureEngine) {
        m_captureEngine->StopCapture();
    }
    if (m_encoder) {
        m_encoder->Finalize();
    }
    m_graphicsMgr = nullptr;
    m_state = RecordingState::Idle;
    LC_LOG_INFO(L"RecordingController shut down cleanly.");
}

Core::Status RecordingController::StartRecording(const AppConfig& config, const Capture::CaptureSource& source) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Validate state transition
    if (m_state != RecordingState::Idle) {
        LC_LOG_WARN(std::format(L"Cannot start recording from state: {}", StateToString(m_state)));
        return Core::Status::Error(Core::StatusCode::InvalidParameter, L"Recording is already in progress or initializing");
    }

    SetState(RecordingState::Initializing, L"Starting capture and encoder...");

    m_framesReceived = 0;
    m_audioFramesReceived = 0;
    m_recordStartTimeNs = Core::GetCurrentTimeNs();

    // 1. Generate timestamped MP4 recording file path
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tmNow{};
    localtime_s(&tmNow, &timeT);

    wchar_t filename[128];
    swprintf_s(filename, L"recording_%04d%02d%02d_%02d%02d%02d.mp4",
        tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
        tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

    std::filesystem::path outDir = config.outputDirectory.empty() ?
        AppConfig::GetDefaultOutputDirectory() : config.outputDirectory;
    m_activeRecordingPath = (outDir / filename).wstring();

    bool audioEnabled = config.captureSystemAudio || config.captureMicrophone;

    // 2. Initialize Media Foundation H.264 video & AAC audio encoder
    Encoder::EncoderConfig encConfig{};
    encConfig.outputPath = m_activeRecordingPath;
    encConfig.width = config.activeProfile.width;
    encConfig.height = config.activeProfile.height;
    encConfig.fps = config.activeProfile.fps;
    encConfig.bitrateBps = config.activeProfile.BitrateBps();
    encConfig.preferHardware = true;
    encConfig.enableAudio = audioEnabled;
    encConfig.audioSampleRate = 48000;
    encConfig.audioChannels = 2;
    encConfig.audioBitrateBps = 128000;

    auto encStatus = m_encoder->Initialize(m_graphicsMgr, encConfig);
    if (encStatus.IsError()) {
        LC_LOG_ERROR(std::format(L"Encoder initialization failed: {}", encStatus.Message()));
        SetState(RecordingState::Error, encStatus.Message());
        return encStatus;
    }

    // 3. Start audio capture (System loopback + Microphone)
    if (audioEnabled && m_audioCapture) {
        Audio::AudioConfig audioCfg{};
        audioCfg.captureSystemAudio = config.captureSystemAudio;
        audioCfg.captureMicrophone = config.captureMicrophone;
        audioCfg.systemDeviceId = config.systemAudioDeviceId;
        audioCfg.micDeviceId = config.micDeviceId;
        audioCfg.sampleRate = 48000;
        audioCfg.channels = 2;
        audioCfg.bitrateBps = 128000;
        audioCfg.systemVolume = config.systemVolume;
        audioCfg.micVolume = config.micVolume;

        auto audioInitStatus = m_audioCapture->Initialize(audioCfg);
        if (audioInitStatus.IsError()) {
            LC_LOG_WARN(std::format(L"Audio capture initialization failed: {}. Continuing without audio.", audioInitStatus.Message()));
        } else {
            m_audioCapture->Start([this](const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs) {
                OnAudioFrame(pcm16, frameCount, timestampNs);
            });
        }
    }

    // 4. Start screen capture session on an MTA worker thread
    auto future = std::async(std::launch::async, [this, config, source]() -> Core::Status {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto captureCallback = [this](const Capture::CapturedFrame& frame) {
            OnCapturedFrame(frame);
        };
        return m_captureEngine->StartCapture(source, captureCallback, config.recordCursor);
    });

    auto status = future.get();
    if (status.IsError()) {
        if (m_audioCapture) m_audioCapture->Stop();
        m_encoder->Finalize();
        SetState(RecordingState::Error, status.Message());
        return status;
    }

    SetState(RecordingState::Recording, L"Recording in progress");
    LC_LOG_INFO(std::format(L"Recording active: Target='{}' -> Output='{}' ({}x{} @ {}fps, {} kbps, Audio={})",
        source.name, m_activeRecordingPath, encConfig.width, encConfig.height, encConfig.fps,
        encConfig.bitrateBps / 1000, audioEnabled ? L"Yes" : L"No"));

    return Core::Status::Ok();
}

Core::Status RecordingController::StopRecording() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Validate state transition
    if (m_state != RecordingState::Recording && m_state != RecordingState::Initializing) {
        return Core::Status::Ok();
    }

    SetState(RecordingState::Stopping, L"Stopping capture session...");

    if (m_audioCapture) {
        m_audioCapture->Stop();
    }

    if (m_captureEngine) {
        m_captureEngine->StopCapture();
    }

    SetState(RecordingState::Finalizing, L"Finalizing MP4 video file...");

    // Finalize H.264 encoder and flush MP4 container
    if (m_encoder) {
        m_encoder->Finalize();
    }

    SetState(RecordingState::Idle, L"Ready to record");

    uint64_t totalDuration = (Core::GetCurrentTimeNs() - m_recordStartTimeNs) / 1000000000LL;
    LC_LOG_INFO(std::format(L"Recording finished: File='{}', Frames={}, Duration={}s",
        m_activeRecordingPath, m_framesReceived.load(), totalDuration));

    return Core::Status::Ok();
}

RecordingState RecordingController::GetState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

Capture::CaptureStats RecordingController::GetStats() const {
    if (m_captureEngine) {
        return m_captureEngine->GetStats();
    }
    return {};
}

Encoder::EncoderStats RecordingController::GetEncoderStats() const {
    if (m_encoder) {
        return m_encoder->GetStats();
    }
    return {};
}

Audio::AudioStats RecordingController::GetAudioStats() const {
    if (m_audioCapture) {
        return m_audioCapture->GetStats();
    }
    return {};
}

uint64_t RecordingController::GetDurationSec() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == RecordingState::Recording && m_recordStartTimeNs > 0) {
        return static_cast<uint64_t>((Core::GetCurrentTimeNs() - m_recordStartTimeNs) / 1000000000LL);
    }
    return 0;
}

std::wstring RecordingController::GetCurrentRecordingPath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeRecordingPath;
}

void RecordingController::SetAudioVolume(float systemVolume, float micVolume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_audioCapture) {
        m_audioCapture->SetVolume(systemVolume, micVolume);
    }
}

void RecordingController::SetState(RecordingState newState, const std::wstring& message) {
    m_state = newState;
    LC_LOG_INFO(std::format(L"Recording state -> {} ({})", StateToString(m_state), message));

    if (m_stateCallback) {
        m_stateCallback(m_state, message);
    }
}

void RecordingController::OnCapturedFrame(const Capture::CapturedFrame& frame) {
    uint64_t count = ++m_framesReceived;

    // Feed captured Direct3D 11 texture into the H.264 video encoder
    if (m_encoder && m_encoder->IsInitialized()) {
        auto encStatus = m_encoder->EncodeFrame(frame);
        if (encStatus.IsError()) {
            LC_LOG_WARN(std::format(L"Failed to encode frame #{}: {}", count, encStatus.Message()));
        }
    }

    // Periodic log inspection every 180 frames (~3 seconds at 60fps)
    if (count % 180 == 1) {
        LC_LOG_DEBUG(std::format(L"Encoded frame #{}: {}x{}, D3D11Texture={:p}, TimestampNs={}",
            count, frame.width, frame.height, static_cast<void*>(frame.texture.Get()), frame.timestampNs));
    }
}

void RecordingController::OnAudioFrame(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs) {
    m_audioFramesReceived += frameCount;
    if (m_encoder && m_encoder->IsInitialized()) {
        auto status = m_encoder->EncodeAudioFrame(pcm16, frameCount, timestampNs);
        if (status.IsError()) {
            LC_LOG_WARN(std::format(L"Failed to encode audio frame: {}", status.Message()));
        }
    }
}

} // namespace LiteCapture::App
