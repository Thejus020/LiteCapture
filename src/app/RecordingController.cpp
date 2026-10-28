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
    if (m_encoderThreadRunning.load()) {
        m_encoderThreadRunning.store(false);
        m_queueCv.notify_all();
        if (m_encoderThread.joinable()) {
            m_encoderThread.join();
        }
    }
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_texturePool.clear();
        while (!m_frameQueue.empty()) m_frameQueue.pop();
    }
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
    m_framesDroppedQueue = 0;
    m_recordStartTimeNs = Core::GetCurrentTimeNs();

    m_targetWidth = config.activeProfile.width;
    m_targetHeight = config.activeProfile.height;

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

    // 2. Pre-allocate dedicated GPU staging texture pool for decoupled capture and zero-wait backpressure
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_texturePool.clear();
        while (!m_frameQueue.empty()) m_frameQueue.pop();
        for (size_t i = 0; i < TEXTURE_POOL_SIZE; ++i) {
            auto texResult = m_graphicsMgr->CreateTexture2D(
                m_targetWidth, m_targetHeight,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                0
            );
            if (texResult.IsOk()) {
                m_texturePool.push_back(texResult.Value());
            }
        }
    }

    // 3. Initialize Media Foundation H.264 video & AAC audio encoder
    Encoder::EncoderConfig encConfig{};
    encConfig.outputPath = m_activeRecordingPath;
    encConfig.width = m_targetWidth;
    encConfig.height = m_targetHeight;
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

    // 4. Start dedicated asynchronous video encoder thread
    m_encoderThreadRunning.store(true);
    m_encoderThread = std::thread(&RecordingController::EncoderThreadProc, this);

    // 5. Start audio capture (System loopback + Microphone)
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

    // 6. Start screen capture session on an MTA worker thread
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
        if (m_encoderThreadRunning.load()) {
            m_encoderThreadRunning.store(false);
            m_queueCv.notify_all();
            if (m_encoderThread.joinable()) m_encoderThread.join();
        }
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

    // Stop and drain the asynchronous video encoding thread
    if (m_encoderThreadRunning.load()) {
        m_encoderThreadRunning.store(false);
        m_queueCv.notify_all();
        if (m_encoderThread.joinable()) {
            m_encoderThread.join();
        }
    }

    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_texturePool.clear();
        while (!m_frameQueue.empty()) m_frameQueue.pop();
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
    Capture::CaptureStats stats{};
    if (m_captureEngine) {
        stats = m_captureEngine->GetStats();
        stats.framesDropped += m_framesDroppedQueue.load();
    }
    return stats;
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
    if (!m_encoderThreadRunning.load() || !frame.texture) {
        return;
    }

    uint64_t count = ++m_framesReceived;

    Core::ComPtr<ID3D11Texture2D> stagingTexture;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_texturePool.empty()) {
            stagingTexture = m_texturePool.back();
            m_texturePool.pop_back();
        } else if (m_frameQueue.size() >= MAX_QUEUE_DEPTH) {
            // Queue is full! Backpressure policy: drop oldest frame to prevent unbounded memory growth
            stagingTexture = m_frameQueue.front().texture;
            m_frameQueue.pop();
            m_framesDroppedQueue++;
        }
    }

    if (!stagingTexture) {
        m_framesDroppedQueue++;
        return;
    }

    // Instant GPU-to-GPU copy (<0.05ms) - frees WGC's surface immediately!
    if (frame.width == m_targetWidth && frame.height == m_targetHeight) {
        m_graphicsMgr->GetContext()->CopyResource(stagingTexture.Get(), frame.texture.Get());
    } else {
        D3D11_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = (std::min)(frame.width, m_targetWidth);
        srcBox.bottom = (std::min)(frame.height, m_targetHeight);
        srcBox.back = 1;

        m_graphicsMgr->GetContext()->CopySubresourceRegion(
            stagingTexture.Get(), 0, 0, 0, 0,
            frame.texture.Get(), 0, &srcBox
        );
    }

    // Push into bounded queue for asynchronous encoding
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        QueuedFrame qf;
        qf.texture = stagingTexture;
        qf.timestampNs = frame.timestampNs;
        qf.durationNs = frame.durationNs;
        qf.width = m_targetWidth;
        qf.height = m_targetHeight;
        qf.frameIndex = frame.frameIndex;
        m_frameQueue.push(std::move(qf));
    }
    m_queueCv.notify_one();

    // Periodic log inspection every 180 frames (~3 seconds at 60fps)
    if (count % 180 == 1) {
        LC_LOG_DEBUG(std::format(L"Captured frame #{}: {}x{}, TimestampNs={}",
            count, frame.width, frame.height, frame.timestampNs));
    }
}

void RecordingController::EncoderThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    while (m_encoderThreadRunning.load()) {
        QueuedFrame qf;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return !m_frameQueue.empty() || !m_encoderThreadRunning.load();
            });

            if (m_frameQueue.empty()) {
                if (!m_encoderThreadRunning.load()) break;
                continue;
            }

            qf = std::move(m_frameQueue.front());
            m_frameQueue.pop();
        }

        if (m_encoder && m_encoder->IsInitialized()) {
            Capture::CapturedFrame capFrame{};
            capFrame.texture = qf.texture;
            capFrame.timestampNs = qf.timestampNs;
            capFrame.durationNs = qf.durationNs;
            capFrame.width = qf.width;
            capFrame.height = qf.height;
            capFrame.frameIndex = qf.frameIndex;

            m_encoder->EncodeFrame(capFrame);
        }

        // Recycle texture back into pool
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_texturePool.push_back(qf.texture);
        }
    }

    // Drain any remaining frames on shutdown so no captured frames are lost
    while (true) {
        QueuedFrame qf;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_frameQueue.empty()) break;
            qf = std::move(m_frameQueue.front());
            m_frameQueue.pop();
        }

        if (m_encoder && m_encoder->IsInitialized()) {
            Capture::CapturedFrame capFrame{};
            capFrame.texture = qf.texture;
            capFrame.timestampNs = qf.timestampNs;
            capFrame.durationNs = qf.durationNs;
            capFrame.width = qf.width;
            capFrame.height = qf.height;
            capFrame.frameIndex = qf.frameIndex;
            m_encoder->EncodeFrame(capFrame);
        }
    }

    CoUninitialize();
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
