#include "encoder/MfVideoEncoder.hpp"
#include "core/Logger.hpp"
#include <icodecapi.h>
#include <codecapi.h>
#include <filesystem>
#include <format>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace LiteCapture::Encoder {

MfVideoEncoder::MfVideoEncoder() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to initialize Media Foundation (MFStartup)");
    }
}

MfVideoEncoder::~MfVideoEncoder() {
    Finalize();
    MFShutdown();
}

Core::Status MfVideoEncoder::Initialize(Graphics::D3D11DeviceManager* graphicsMgr, const EncoderConfig& config) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    if (!graphicsMgr || !graphicsMgr->IsInitialized()) {
        return Core::Status::Error(Core::StatusCode::NotInitialized, L"D3D11 graphics manager not initialized");
    }

    if (config.outputPath.empty() || config.width == 0 || config.height == 0 || config.fps == 0) {
        return Core::Status::Error(Core::StatusCode::InvalidParameter, L"Invalid encoder configuration parameters");
    }

    m_graphicsMgr = graphicsMgr;
    m_config = config;
    m_cachedBufferLength = m_config.width * m_config.height * 4;

    // Ensure output directory exists
    std::filesystem::path outDir = std::filesystem::path(m_config.outputPath).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    auto status = SetupSinkWriter();
    if (status.IsError()) {
        return status;
    }

    m_initialized = true;
    m_framesEncoded = 0;
    m_audioFramesEncoded = 0;
    m_firstFrameTimestampNs = 0;
    m_lastFrameTimestampNs = 0;
    m_lastSampleTimeHns = -1;
    m_firstAudioTimestampNs = 0;
    m_lastAudioSampleTimeHns = -1;
    m_audioSamplesWritten = 0;
    m_lastFpsCalcTimeNs = Core::GetCurrentTimeNs();
    m_fpsFrameCounter = 0;

    LC_LOG_INFO(std::format(L"MfVideoEncoder initialized: Output='{}' ({}x{} @ {}fps, {} kbps, Audio={})",
        m_config.outputPath, m_config.width, m_config.height, m_config.fps, m_config.bitrateBps / 1000,
        m_config.enableAudio ? L"Enabled (AAC 128kbps 48kHz)" : L"Disabled"));

    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::SetupSinkWriter() {
    // 1. Create IMFDXGIDeviceManager to bind D3D11 device for hardware GPU acceleration
    HRESULT hr = MFCreateDXGIDeviceManager(&m_dxgiResetToken, &m_dxgiDeviceManager);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create IMFDXGIDeviceManager");
        return Core::Status::FromHResult(hr, L"MFCreateDXGIDeviceManager");
    }

    hr = m_dxgiDeviceManager->ResetDevice(m_graphicsMgr->GetDevice(), m_dxgiResetToken);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to bind D3D11 device to IMFDXGIDeviceManager");
        return Core::Status::FromHResult(hr, L"IMFDXGIDeviceManager::ResetDevice");
    }

    // 2. Configure SinkWriter attributes
    Core::ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 6);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"MFCreateAttributes");
    }

    // Enable hardware transforms and low latency
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, m_config.preferHardware ? TRUE : FALSE);
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, m_dxgiDeviceManager.Get());

    // 3. Create Sink Writer for MP4 output
    hr = MFCreateSinkWriterFromURL(m_config.outputPath.c_str(), nullptr, attributes.Get(), &m_sinkWriter);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, std::format(L"Failed to create IMFSinkWriter for '{}'", m_config.outputPath));
        return Core::Status::FromHResult(hr, L"MFCreateSinkWriterFromURL");
    }

    // 4. Setup output and input video media types
    auto typeStatus = CreateMediaTypes();
    if (typeStatus.IsError()) {
        return typeStatus;
    }

    // 5. Setup AAC audio stream (if audio is enabled)
    m_hasAudioStream = false;
    if (m_config.enableAudio) {
        auto audioStatus = CreateAudioMediaTypes();
        if (audioStatus.IsError()) {
            LC_LOG_WARN(std::format(L"Failed to configure audio stream: {}. Proceeding with video only.", audioStatus.Message()));
        }
    }

    // 6. Begin writing MP4 container
    hr = m_sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to start SinkWriter (BeginWriting)");
        return Core::Status::FromHResult(hr, L"BeginWriting");
    }

    m_isHardware = m_config.preferHardware;
    m_encoderName = m_isHardware ? L"Media Foundation H.264 (Hardware MFT)" : L"Media Foundation H.264 (Software)";
    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::CreateMediaTypes() {
    // 1. Output Media Type (H.264 / AVC in MP4)
    Core::ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"MFCreateMediaType Output");

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, m_config.bitrateBps);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, m_config.fps, 1);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, m_config.width, m_config.height);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outputType->SetUINT32(MF_MT_MPEG2_PROFILE, 100 /* eAVEncH264VProfile_High */);
    outputType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, m_config.fps * 2);

    hr = m_sinkWriter->AddStream(outputType.Get(), &m_streamIndex);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to add H.264 output stream to SinkWriter");
        return Core::Status::FromHResult(hr, L"AddStream H.264");
    }

    // 2. Input Media Type (D3D11 BGRA / ARGB32)
    Core::ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"MFCreateMediaType Input");

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_config.width, m_config.height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, m_config.fps, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_sinkWriter->SetInputMediaType(m_streamIndex, inputType.Get(), nullptr);
    if (FAILED(hr)) {
        // Fallback to RGB32 if ARGB32 is not accepted by the MFT
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = m_sinkWriter->SetInputMediaType(m_streamIndex, inputType.Get(), nullptr);
        if (FAILED(hr)) {
            LC_LOG_HR(hr, L"Failed to set input media type on SinkWriter");
            return Core::Status::FromHResult(hr, L"SetInputMediaType");
        }
    }

    // 3. Configure advanced encoder rate control via ICodecAPI:
    // Set rate control mode to Unconstrained VBR so that static desktop scenes (with little or no movement)
    // consume minimal bits rather than padding out to constant bitrate.
    // Also enable CABAC entropy coding for 10-15% higher compression efficiency.
    Core::ComPtr<ICodecAPI> codecApi;
    hr = m_sinkWriter->GetServiceForStream(m_streamIndex, GUID_NULL, IID_PPV_ARGS(&codecApi));
    if (SUCCEEDED(hr) && codecApi) {
        VARIANT var;
        VariantInit(&var);

        // Rate control: Unconstrained VBR (eAVEncCommonRateControlMode_UnconstrainedVBR = 2)
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
        hr = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
        if (SUCCEEDED(hr)) {
            LC_LOG_INFO(L"H.264 encoder rate control set to Unconstrained VBR for optimized file sizes.");
        }

        // Quality level (70 = optimal visual quality / small file size trade-off)
        var.vt = VT_UI4;
        var.ulVal = 70;
        codecApi->SetValue(&CODECAPI_AVEncCommonQuality, &var);

        // CABAC entropy encoding (more efficient than CAVLC)
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVEncH264CABACEnable, &var);

        // GOP size (Keyframe spacing = 2 seconds)
        var.vt = VT_UI4;
        var.ulVal = m_config.fps * 2;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        VariantClear(&var);
    }

    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::CreateAudioMediaTypes() {
    // 1. Output Media Type (AAC Audio in MP4 container)
    Core::ComPtr<IMFMediaType> audioOutputType;
    HRESULT hr = MFCreateMediaType(&audioOutputType);
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"MFCreateMediaType Audio Output");

    audioOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    audioOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_config.audioSampleRate);
    audioOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_config.audioChannels);
    audioOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_config.audioBitrateBps / 8);
    audioOutputType->SetUINT32(MF_MT_AVG_BITRATE, m_config.audioBitrateBps);

    hr = m_sinkWriter->AddStream(audioOutputType.Get(), &m_audioStreamIndex);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to add AAC audio stream to SinkWriter");
        return Core::Status::FromHResult(hr, L"AddStream AAC");
    }

    // 2. Input Media Type (Uncompressed 16-bit PCM at 48kHz Stereo)
    Core::ComPtr<IMFMediaType> audioInputType;
    hr = MFCreateMediaType(&audioInputType);
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"MFCreateMediaType Audio Input");

    uint32_t blockAlign = m_config.audioChannels * (16 / 8); // 4 bytes for 16-bit stereo
    uint32_t avgBytesPerSec = m_config.audioSampleRate * blockAlign; // 48000 * 4 = 192000

    audioInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    audioInputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_config.audioSampleRate);
    audioInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_config.audioChannels);
    audioInputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
    audioInputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec);

    hr = m_sinkWriter->SetInputMediaType(m_audioStreamIndex, audioInputType.Get(), nullptr);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to set input media type on AAC audio stream");
        return Core::Status::FromHResult(hr, L"SetInputMediaType Audio");
    }

    m_hasAudioStream = true;
    LC_LOG_INFO(std::format(L"AAC audio stream configured: {} Hz, {} channels, {} kbps",
        m_config.audioSampleRate, m_config.audioChannels, m_config.audioBitrateBps / 1000));
    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::EncodeAudioFrame(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs) {
    (void)timestampNs;
    if (!m_initialized || !m_sinkWriter || !m_hasAudioStream) {
        return Core::Status::Ok();
    }

    if (!pcm16 || frameCount == 0) {
        return Core::Status::Ok();
    }

    std::lock_guard<std::mutex> lock(m_audioMutex);

    // Synchronize audio start: wait until the first video frame arrives before encoding audio.
    // This guarantees both video and audio start at the exact same physical moment at PTS = 0.
    if (m_firstFrameTimestampNs == 0) {
        return Core::Status::Ok();
    }

    // Audio sample duration in 100ns units
    LONGLONG durationHns = (static_cast<LONGLONG>(frameCount) * 10000000LL) / m_config.audioSampleRate;

    // Continuous, drift-free timestamp calculated from total samples written
    LONGLONG sampleTimeHns = (m_audioSamplesWritten * 10000000LL) / m_config.audioSampleRate;
    m_audioSamplesWritten += frameCount;

    DWORD byteLength = frameCount * m_config.audioChannels * sizeof(int16_t);

    Core::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(byteLength, &mediaBuffer);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create audio memory buffer");
        return Core::Status::FromHResult(hr, L"MFCreateMemoryBuffer Audio");
    }

    BYTE* pDest = nullptr;
    hr = mediaBuffer->Lock(&pDest, nullptr, nullptr);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"mediaBuffer->Lock Audio");
    }
    memcpy(pDest, pcm16, byteLength);
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(byteLength);

    Core::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"MFCreateSample Audio");
    }

    hr = sample->AddBuffer(mediaBuffer.Get());
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"sample->AddBuffer Audio");

    sample->SetSampleTime(sampleTimeHns);
    sample->SetSampleDuration(durationHns);

    hr = m_sinkWriter->WriteSample(m_audioStreamIndex, sample.Get());
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"SinkWriter failed to write audio sample");
        return Core::Status::FromHResult(hr, L"WriteSample Audio");
    }

    m_audioFramesEncoded += frameCount;
    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::EncodeFrame(const Capture::CapturedFrame& frame) {
    if (!m_initialized || !m_sinkWriter) {
        return Core::Status::Error(Core::StatusCode::NotInitialized, L"Encoder is not initialized");
    }

    if (!frame.texture) {
        return Core::Status::Error(Core::StatusCode::InvalidParameter, L"Frame texture is null");
    }

    std::lock_guard<std::mutex> lock(m_videoMutex);

    if (m_firstFrameTimestampNs == 0) {
        m_firstFrameTimestampNs = frame.timestampNs;
    }

    // Convert nanoseconds to 100-nanosecond units (HNS) for Media Foundation
    LONGLONG sampleDurationHns = static_cast<LONGLONG>(frame.durationNs / 100);
    if (sampleDurationHns <= 0) {
        sampleDurationHns = 10000000LL / (m_config.fps > 0 ? m_config.fps : 60);
    }

    LONGLONG sampleTimeHns = static_cast<LONGLONG>((frame.timestampNs - m_firstFrameTimestampNs) / 100);
    if (sampleTimeHns <= m_lastSampleTimeHns && m_framesEncoded > 0) {
        sampleTimeHns = m_lastSampleTimeHns + sampleDurationHns;
    }
    m_lastSampleTimeHns = sampleTimeHns;

    // Prepare D3D11 texture matching encoder dimensions
    ID3D11Texture2D* sourceTexture = frame.texture.Get();

    // Check if texture dimensions match configured encoder output
    if (frame.width != m_config.width || frame.height != m_config.height) {
        if (!m_stagingTexture || m_stagingWidth != m_config.width || m_stagingHeight != m_config.height) {
            auto texResult = m_graphicsMgr->CreateTexture2D(
                m_config.width, m_config.height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                0
            );
            if (texResult.IsError()) {
                return texResult.GetStatus();
            }
            m_stagingTexture = texResult.Value();
            m_stagingWidth = m_config.width;
            m_stagingHeight = m_config.height;
        }

        // Copy source subresource region to staging texture
        D3D11_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = (std::min)(frame.width, m_config.width);
        srcBox.bottom = (std::min)(frame.height, m_config.height);
        srcBox.back = 1;

        m_graphicsMgr->GetContext()->CopySubresourceRegion(
            m_stagingTexture.Get(), 0, 0, 0, 0,
            frame.texture.Get(), 0, &srcBox
        );
        sourceTexture = m_stagingTexture.Get();
    }

    // Create zero-copy DXGI surface media buffer directly referencing GPU texture
    Core::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), sourceTexture, 0, FALSE, &mediaBuffer);
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"Failed to create DXGI surface buffer for video frame");
        return Core::Status::FromHResult(hr, L"MFCreateDXGISurfaceBuffer");
    }

    // Set valid buffer length (cached to avoid per-frame COM QueryInterface overhead)
    mediaBuffer->SetCurrentLength(m_cachedBufferLength);

    // Create Media Foundation sample
    Core::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        return Core::Status::FromHResult(hr, L"MFCreateSample");
    }

    hr = sample->AddBuffer(mediaBuffer.Get());
    if (FAILED(hr)) return Core::Status::FromHResult(hr, L"sample->AddBuffer");

    sample->SetSampleTime(sampleTimeHns);
    sample->SetSampleDuration(sampleDurationHns);

    // Feed sample into H.264 encoder stream
    hr = m_sinkWriter->WriteSample(m_streamIndex, sample.Get());
    if (FAILED(hr)) {
        LC_LOG_HR(hr, L"SinkWriter failed to write video sample");
        return Core::Status::FromHResult(hr, L"WriteSample");
    }

    m_framesEncoded++;
    m_fpsFrameCounter++;
    m_lastFrameTimestampNs = frame.timestampNs;

    // Calculate real-time encoding FPS
    int64_t nowNs = Core::GetCurrentTimeNs();
    if (nowNs - m_lastFpsCalcTimeNs >= 1000000000LL) {
        double elapsedSec = static_cast<double>(nowNs - m_lastFpsCalcTimeNs) / 1000000000.0;
        m_currentFps = static_cast<double>(m_fpsFrameCounter) / elapsedSec;
        m_fpsFrameCounter = 0;
        m_lastFpsCalcTimeNs = nowNs;
    }

    return Core::Status::Ok();
}

Core::Status MfVideoEncoder::Finalize() {
    std::scoped_lock lock(m_lifecycleMutex, m_videoMutex, m_audioMutex);

    if (!m_initialized) {
        return Core::Status::Ok();
    }

    LC_LOG_INFO(std::format(L"Finalizing MP4 recording '{}' (Total frames: {})...",
        m_config.outputPath, m_framesEncoded.load()));

    if (m_sinkWriter) {
        HRESULT hr = m_sinkWriter->Finalize();
        if (FAILED(hr)) {
            LC_LOG_HR(hr, L"Failed to finalize IMFSinkWriter");
        } else {
            LC_LOG_INFO(L"IMFSinkWriter finalized successfully. MP4 file header completed.");
        }
        m_sinkWriter.Reset();
    }

    m_stagingTexture.Reset();
    m_dxgiDeviceManager.Reset();
    m_initialized = false;

    // Log final output file size
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(m_config.outputPath, ec);
    if (!ec) {
        double sizeMb = static_cast<double>(size) / (1024.0 * 1024.0);
        LC_LOG_INFO(std::format(L"Recorded file size: {:.2f} MB ({})", sizeMb, m_config.outputPath));
    }

    return Core::Status::Ok();
}

EncoderStats MfVideoEncoder::GetStats() const {
    EncoderStats stats{};
    stats.framesEncoded = m_framesEncoded.load();
    stats.audioFramesEncoded = m_audioFramesEncoded.load();
    stats.actualFps = m_currentFps.load();
    stats.encoderName = m_encoderName;
    stats.isHardwareAccelerated = m_isHardware;

    std::error_code ec;
    stats.fileSizeBytes = std::filesystem::file_size(m_config.outputPath, ec);
    if (ec) stats.fileSizeBytes = 0;

    return stats;
}

} // namespace LiteCapture::Encoder
