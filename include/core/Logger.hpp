#pragma once

#include "Common.hpp"
#include <fstream>
#include <mutex>

namespace LiteCapture::Core {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

class Logger {
public:
    static Logger& Instance();

    void Initialize(const std::wstring& logDir = {});
    void Shutdown();

    void Log(LogLevel level, std::wstring_view message, const char* file = nullptr, int line = 0);

    void SetMinLevel(LogLevel level) { m_minLevel = level; }
    LogLevel GetMinLevel() const { return m_minLevel; }
    const std::wstring& GetLogFilePath() const { return m_logFilePath; }

private:
    Logger() = default;
    ~Logger() { Shutdown(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::wstring FormatTimestamp() const;
    std::wstring_view LevelToString(LogLevel level) const;

    std::recursive_mutex m_mutex;
    std::ofstream m_fileStream;
    std::wstring m_logFilePath;
    LogLevel m_minLevel{LogLevel::Debug};
    bool m_initialized{false};
};

// Convenience logging functions
inline void LogMessage(LogLevel level, const std::wstring& msg, const char* file = nullptr, int line = 0) {
    Logger::Instance().Log(level, msg, file, line);
}

inline void LogHResult(LogLevel level, HRESULT hr, const std::wstring& context, const char* file = nullptr, int line = 0) {
    std::wstring fullMsg = std::format(L"{}: {} (0x{:08X})", context, HResultToString(hr), static_cast<uint32_t>(hr));
    Logger::Instance().Log(level, fullMsg, file, line);
}

} // namespace LiteCapture::Core

#define LC_LOG_TRACE(msg)   ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Trace,   (msg), __FILE__, __LINE__)
#define LC_LOG_DEBUG(msg)   ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Debug,   (msg), __FILE__, __LINE__)
#define LC_LOG_INFO(msg)    ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Info,    (msg), __FILE__, __LINE__)
#define LC_LOG_WARN(msg)    ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Warning, (msg), __FILE__, __LINE__)
#define LC_LOG_ERROR(msg)   ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Error,   (msg), __FILE__, __LINE__)
#define LC_LOG_FATAL(msg)   ::LiteCapture::Core::LogMessage(::LiteCapture::Core::LogLevel::Fatal,   (msg), __FILE__, __LINE__)
#define LC_LOG_HR(hr, ctx)  ::LiteCapture::Core::LogHResult(::LiteCapture::Core::LogLevel::Error,  (hr), (ctx), __FILE__, __LINE__)
