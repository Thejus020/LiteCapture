#pragma once

#include "core/Common.hpp"
#include <d3d11.h>
#include <functional>
#include <string>

namespace LiteCapture::Capture {

enum class CaptureSourceType {
    Monitor,
    Window
};

struct CaptureSource {
    CaptureSourceType type{CaptureSourceType::Monitor};
    HMONITOR hMonitor{nullptr};
    HWND hWnd{nullptr};
    std::wstring name;
    uint32_t width{0};
    uint32_t height{0};
};

struct CapturedFrame {
    Core::ComPtr<ID3D11Texture2D> texture;
    int64_t timestampNs{0};
    int64_t durationNs{0};
    uint32_t width{0};
    uint32_t height{0};
    uint64_t frameIndex{0};
};

struct CaptureStats {
    uint64_t framesCaptured{0};
    uint64_t framesDropped{0};
    double captureFps{0.0};
    uint32_t currentWidth{0};
    uint32_t currentHeight{0};
};

using FrameArrivalCallback = std::function<void(const CapturedFrame&)>;

} // namespace LiteCapture::Capture
