#pragma once

#include "core/Common.hpp"
#include <string>
#include <cstdint>

namespace LiteCapture::Encoder {

struct EncoderConfig {
    std::wstring outputPath;
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t fps{30};
    uint32_t bitrateBps{4000000}; // e.g. 4 Mbps
    bool preferHardware{true};

    // Audio encoding settings (AAC in MP4 container)
    bool enableAudio{true};
    uint32_t audioSampleRate{48000};
    uint32_t audioChannels{2};
    uint32_t audioBitrateBps{128000}; // 128 kbps
};

struct EncoderStats {
    uint64_t framesEncoded{0};
    uint64_t audioFramesEncoded{0};
    double actualFps{0.0};
    std::wstring encoderName{L"Unknown"};
    bool isHardwareAccelerated{false};
    uint64_t fileSizeBytes{0};
};

} // namespace LiteCapture::Encoder
