#pragma once

namespace wowvr
{
    enum class LogLevel
    {
        Off = 0,
        Error = 1,
        Warn = 2,
        Info = 3,
        Debug = 4,
        Trace = 5
    };

    void LogInit(const wchar_t* filePath, LogLevel level);
    void LogShutdown();

    void LogSetLevel(LogLevel level);
    LogLevel LogGetLevel();

    // Parses "off"/"error"/"warn"/"info"/"debug"/"trace"; unknown values give Info.
    LogLevel LogLevelFromString(const wchar_t* name);

    void LogWrite(LogLevel level, const char* format, ...);

    // Formats the message for a Win32 HRESULT or GetLastError() code.
    const char* LogSystemError(unsigned long code);

    // UTF-8 view of a wide string, for logging with %s. The buffer is per-thread
    // and only valid until the next call on the same thread, so use one per
    // logging statement.
    const char* LogWide(const wchar_t* text);
}

// The level test happens before argument evaluation so that Trace-level logging
// in the draw path costs nothing when it is switched off.
#define WOWVR_LOG(level_, ...)                                        \
    do                                                                \
    {                                                                 \
        if (::wowvr::LogGetLevel() >= (level_))                       \
        {                                                             \
            ::wowvr::LogWrite((level_), __VA_ARGS__);                 \
        }                                                             \
    } while (0)

#define WOWVR_ERROR(...) WOWVR_LOG(::wowvr::LogLevel::Error, __VA_ARGS__)
#define WOWVR_WARN(...)  WOWVR_LOG(::wowvr::LogLevel::Warn,  __VA_ARGS__)
#define WOWVR_INFO(...)  WOWVR_LOG(::wowvr::LogLevel::Info,  __VA_ARGS__)
#define WOWVR_DEBUG(...) WOWVR_LOG(::wowvr::LogLevel::Debug, __VA_ARGS__)
#define WOWVR_TRACE(...) WOWVR_LOG(::wowvr::LogLevel::Trace, __VA_ARGS__)
