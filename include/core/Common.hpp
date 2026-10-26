#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <chrono>
#include <format>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>

namespace LiteCapture::Core {

using Microsoft::WRL::ComPtr;

// Helper to convert std::string to std::wstring (UTF-8 to UTF-16)
inline std::wstring Utf8ToUtf16(std::string_view utf8Str) {
    if (utf8Str.empty()) return {};
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
    if (sizeNeeded <= 0) return {};
    std::wstring result(sizeNeeded, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), result.data(), sizeNeeded);
    return result;
}

// Helper to convert std::wstring to std::string (UTF-16 to UTF-8)
inline std::string Utf16ToUtf8(std::wstring_view utf16Str) {
    if (utf16Str.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, utf16Str.data(), static_cast<int>(utf16Str.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16Str.data(), static_cast<int>(utf16Str.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

// Convert HRESULT to descriptive error message
inline std::wstring HResultToString(HRESULT hr) {
    LPWSTR errorText = nullptr;
    DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&errorText),
        0,
        nullptr
    );

    std::wstring result;
    if (count > 0 && errorText) {
        // Strip trailing newline characters
        while (count > 0 && (errorText[count - 1] == L'\r' || errorText[count - 1] == L'\n')) {
            count--;
        }
        result = std::wstring(errorText, count);
        LocalFree(errorText);
    } else {
        result = std::format(L"HRESULT: 0x{:08X}", static_cast<uint32_t>(hr));
    }
    return result;
}

// High-resolution clock helpers
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline int64_t GetCurrentTimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

inline int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()
    ).count();
}

} // namespace LiteCapture::Core
