#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <share.h>

namespace wowvr
{
    namespace
    {
        CRITICAL_SECTION g_lock;
        bool g_lockReady = false;
        FILE* g_file = nullptr;
        LogLevel g_level = LogLevel::Info;
        LARGE_INTEGER g_frequency = {};
        LARGE_INTEGER g_start = {};

        const char* LevelTag(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Error: return "ERROR";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Trace: return "TRACE";
            default:              return "?????";
            }
        }

        double SecondsSinceStart()
        {
            if (g_frequency.QuadPart == 0)
            {
                return 0.0;
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart - g_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
        }
    }

    void LogInit(const wchar_t* filePath, LogLevel level)
    {
        if (!g_lockReady)
        {
            InitializeCriticalSection(&g_lock);
            g_lockReady = true;
        }

        EnterCriticalSection(&g_lock);

        g_level = level;
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_start);

        if (g_file != nullptr)
        {
            fclose(g_file);
            g_file = nullptr;
        }

        if (level != LogLevel::Off)
        {
            // Plain byte stream on purpose. Opening with ccs=UTF-8 puts the stream
            // into wide orientation, after which every narrow fprintf silently fails.
            // Wide text is converted to UTF-8 by LogWide before it gets here.
            //
            // _SH_DENYWR rather than fopen's exclusive default, so the log can be
            // tailed while the game is still running.
            g_file = _wfsopen(filePath, L"w", _SH_DENYWR);
        }

        if (g_file != nullptr)
        {
            SYSTEMTIME localTime;
            GetLocalTime(&localTime);
            fprintf(g_file,
                    "WoWVR log started %04u-%02u-%02u %02u:%02u:%02u\n"
                    "----------------------------------------------\n",
                    localTime.wYear, localTime.wMonth, localTime.wDay,
                    localTime.wHour, localTime.wMinute, localTime.wSecond);
            fflush(g_file);
        }

        LeaveCriticalSection(&g_lock);
    }

    void LogShutdown()
    {
        if (!g_lockReady)
        {
            return;
        }

        EnterCriticalSection(&g_lock);
        if (g_file != nullptr)
        {
            fflush(g_file);
            fclose(g_file);
            g_file = nullptr;
        }
        LeaveCriticalSection(&g_lock);
    }

    void LogSetLevel(LogLevel level)
    {
        g_level = level;
    }

    LogLevel LogGetLevel()
    {
        return g_level;
    }

    LogLevel LogLevelFromString(const wchar_t* name)
    {
        if (name == nullptr)
        {
            return LogLevel::Info;
        }
        if (_wcsicmp(name, L"off") == 0)   { return LogLevel::Off; }
        if (_wcsicmp(name, L"error") == 0) { return LogLevel::Error; }
        if (_wcsicmp(name, L"warn") == 0)  { return LogLevel::Warn; }
        if (_wcsicmp(name, L"info") == 0)  { return LogLevel::Info; }
        if (_wcsicmp(name, L"debug") == 0) { return LogLevel::Debug; }
        if (_wcsicmp(name, L"trace") == 0) { return LogLevel::Trace; }
        return LogLevel::Info;
    }

    void LogWrite(LogLevel level, const char* format, ...)
    {
        if (g_file == nullptr || !g_lockReady)
        {
            return;
        }

        char message[2048];
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        if (written < 0)
        {
            return;
        }

        EnterCriticalSection(&g_lock);
        if (g_file != nullptr)
        {
            fprintf(g_file, "[%9.3f] [%s] [%04lx] %s\n",
                    SecondsSinceStart(), LevelTag(level), GetCurrentThreadId(), message);
            // Flushed every line on purpose: when this DLL takes the game down,
            // the tail of the log is the only evidence we get.
            fflush(g_file);
        }
        LeaveCriticalSection(&g_lock);
    }

    const char* LogWide(const wchar_t* text)
    {
        static thread_local char buffer[1024];

        if (text == nullptr)
        {
            buffer[0] = '\0';
            return buffer;
        }

        const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1,
                                                buffer, static_cast<int>(sizeof(buffer)), nullptr, nullptr);
        if (written <= 0)
        {
            buffer[0] = '\0';
        }
        return buffer;
    }

    const char* LogSystemError(unsigned long code)
    {
        static thread_local char buffer[512];

        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);

        if (length == 0)
        {
            sprintf_s(buffer, "unknown error 0x%08lx", code);
            return buffer;
        }

        // Trim the trailing CRLF that FormatMessage appends.
        char* end = buffer + length;
        while (end > buffer && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' '))
        {
            --end;
        }
        *end = '\0';
        return buffer;
    }
}
