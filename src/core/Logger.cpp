#include "core/Logger.hpp"
#include <shlobj.h>
#include <filesystem>
#include <iostream>

namespace LiteCapture::Core {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(const std::wstring& logDir) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return;

    std::filesystem::path dirPath;
    if (!logDir.empty()) {
        dirPath = logDir;
    } else {
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
            dirPath = std::filesystem::path(localAppData) / L"LiteCapture" / L"Logs";
            CoTaskMemFree(localAppData);
        } else {
            dirPath = std::filesystem::current_path() / L"Logs";
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);

    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tmNow{};
    localtime_s(&tmNow, &timeT);

    wchar_t filename[128];
    swprintf_s(filename, L"litecapture_%04d%02d%02d_%02d%02d%02d.log",
        tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
        tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

    m_logFilePath = (dirPath / filename).wstring();
    m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
    m_initialized = true;

    // Write initial log header
    Log(LogLevel::Info, L"=== LiteCapture Log Started ===");
    Log(LogLevel::Info, std::format(L"Log file: {}", m_logFilePath));
}

void Logger::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return;

    Log(LogLevel::Info, L"=== LiteCapture Log Closed ===");
    if (m_fileStream.is_open()) {
        m_fileStream.flush();
        m_fileStream.close();
    }
    m_initialized = false;
}

std::wstring Logger::FormatTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tmNow{};
    localtime_s(&tmNow, &timeT);

    wchar_t buffer[64];
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
        tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
        static_cast<int>(ms.count()));
    return buffer;
}

std::wstring_view Logger::LevelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Trace:   return L"TRACE";
        case LogLevel::Debug:   return L"DEBUG";
        case LogLevel::Info:    return L"INFO ";
        case LogLevel::Warning: return L"WARN ";
        case LogLevel::Error:   return L"ERROR";
        case LogLevel::Fatal:   return L"FATAL";
        default:                return L"UNKN ";
    }
}

void Logger::Log(LogLevel level, std::wstring_view message, const char* file, int line) {
    if (level < m_minLevel) return;

    DWORD threadId = GetCurrentThreadId();
    std::wstring timestamp = FormatTimestamp();
    std::wstring_view levelStr = LevelToString(level);

    std::wstring logLine;
    if (file && line > 0) {
        std::string filenameOnly = std::filesystem::path(file).filename().string();
        std::wstring wFilename = Utf8ToUtf16(filenameOnly);
        logLine = std::format(L"[{}] [{}] [TID:{:04X}] [{}:{}] {}\n",
            timestamp, levelStr, threadId, wFilename, line, message);
    } else {
        logLine = std::format(L"[{}] [{}] [TID:{:04X}] {}\n",
            timestamp, levelStr, threadId, message);
    }

    // Output to debug console (Visual Studio Debugger)
    OutputDebugStringW(logLine.c_str());

    // Thread-safe output to log file
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
        std::string utf8Line = Utf16ToUtf8(logLine);
        m_fileStream << utf8Line;
        m_fileStream.flush();
    }
}

} // namespace LiteCapture::Core
