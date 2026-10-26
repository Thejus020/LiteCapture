#pragma once

#include "capture/ICaptureEngine.hpp"
#include <memory>

namespace LiteCapture::Capture {

class WgcCaptureEngine : public ICaptureEngine {
public:
    WgcCaptureEngine();
    ~WgcCaptureEngine() override;

    Core::Status Initialize(Graphics::D3D11DeviceManager* graphicsMgr) override;
    Core::Status StartCapture(const CaptureSource& source, FrameArrivalCallback callback, bool captureCursor = true) override;
    void StopCapture() override;
    bool IsCapturing() const override;
    CaptureStats GetStats() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace LiteCapture::Capture
