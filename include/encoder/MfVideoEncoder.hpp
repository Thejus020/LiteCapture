#pragma once

#include "encoder/IVideoEncoder.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <atomic>
#include <mutex>

namespace LiteCapture::Encoder {

class MfVideoEncoder : public IVideoEncoder {
public:
    MfVideoEncoder();
    ~MfVideoEncoder() override;

    Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr, const EncoderConfig& config) override;
    Core::Status EncodeFrame(const Capture::CapturedFrame& frame) override;
    Core::Status EncodeAudioFrame(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs) override;
    Core::Status Finalize() override;
    bool IsInitialized() const override { return m_initialized; }
    EncoderStats GetStats() const override;

private:
    Core::Status SetupSinkWriter();
    Core::Status CreateMediaTypes();
    Core::Status CreateAudioMediaTypes();

    Graphics::D3D11DeviceManager* m_graphicsMgr{nullptr};
    EncoderConfig m_config;

    Core::ComPtr<IMFSinkWriter> m_sinkWriter;
    Core::ComPtr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
    UINT m_dxgiResetToken{0};
    DWORD m_streamIndex{0};
    DWORD m_audioStreamIndex{0};
    bool m_hasAudioStream{false};

    // GPU staging / color conversion texture if needed
    Core::ComPtr<ID3D11Texture2D> m_stagingTexture;
    uint32_t m_stagingWidth{0};
    uint32_t m_stagingHeight{0};

    std::atomic<bool> m_initialized{false};
    std::atomic<uint64_t> m_framesEncoded{0};
    std::atomic<uint64_t> m_audioFramesEncoded{0};
    std::atomic<double> m_currentFps{0.0};
    std::wstring m_encoderName{L"Windows Media Foundation H.264"};
    bool m_isHardware{false};

    int64_t m_firstFrameTimestampNs{0};
    int64_t m_lastFrameTimestampNs{0};
    LONGLONG m_lastSampleTimeHns{-1};
    int64_t m_lastFpsCalcTimeNs{0};
    uint32_t m_fpsFrameCounter{0};

    DWORD m_cachedBufferLength{0};

    int64_t m_firstAudioTimestampNs{0};
    LONGLONG m_lastAudioSampleTimeHns{-1};
    LONGLONG m_audioSamplesWritten{0};

    mutable std::mutex m_videoMutex;
    mutable std::mutex m_audioMutex;
    mutable std::mutex m_lifecycleMutex;
};

} // namespace LiteCapture::Encoder
