#pragma once

#include "core/Result.hpp"
#include "encoder/EncoderTypes.hpp"
#include "capture/CaptureTypes.hpp"
#include "graphics/D3D11DeviceManager.hpp"

namespace LiteCapture::Encoder {

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr, const EncoderConfig& config) = 0;
    virtual Core::Status EncodeFrame(const Capture::CapturedFrame& frame) = 0;
    virtual Core::Status EncodeAudioFrame(const int16_t* pcm16, uint32_t frameCount, int64_t timestampNs) = 0;
    virtual Core::Status Finalize() = 0;
    virtual bool IsInitialized() const = 0;
    virtual EncoderStats GetStats() const = 0;
};

} // namespace LiteCapture::Encoder
