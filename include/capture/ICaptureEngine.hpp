#pragma once

#include "core/Result.hpp"
#include "capture/CaptureTypes.hpp"
#include "graphics/D3D11DeviceManager.hpp"

namespace LiteCapture::Capture {

class ICaptureEngine {
public:
    virtual ~ICaptureEngine() = default;

    virtual Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr) = 0;
    virtual Core::Status StartCapture(const CaptureSource& source, FrameArrivalCallback callback, bool captureCursor = true) = 0;
    virtual void StopCapture() = 0;
    virtual bool IsCapturing() const = 0;
    virtual CaptureStats GetStats() const = 0;
};

} // namespace LiteCapture::Capture
