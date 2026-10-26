#pragma once

#include "core/Common.hpp"
#include <dxgi.h>
#include <vector>
#include <string>

namespace LiteCapture::Graphics {

struct DisplayOutputInfo {
    std::wstring deviceName;     // e.g. \\.\DISPLAY1
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refreshRateNumerator{0};
    uint32_t refreshRateDenominator{1};
    HMONITOR hMonitor{nullptr};
    bool isAttachedToDesktop{false};

    double RefreshRate() const {
        return refreshRateDenominator > 0 ? static_cast<double>(refreshRateNumerator) / refreshRateDenominator : 0.0;
    }

    std::wstring GetDisplayName() const {
        return std::format(L"{} ({}x{} @ {:.0f}Hz)", deviceName, width, height, RefreshRate());
    }
};

struct AdapterInfo {
    std::wstring description;
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    size_t dedicatedVideoMemoryMb{0};
    size_t dedicatedSystemMemoryMb{0};
    size_t sharedSystemMemoryMb{0};
    bool isSoftware{false};
    std::vector<DisplayOutputInfo> outputs;

    std::wstring GetSummary() const {
        if (isSoftware) {
            return std::format(L"{} (Software Fallback)", description);
        }
        return std::format(L"{} (VRAM: {} MB, Shared: {} MB)",
            description, dedicatedVideoMemoryMb, sharedSystemMemoryMb);
    }
};

} // namespace LiteCapture::Graphics
