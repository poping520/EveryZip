#pragma once

#include <windows.h>

namespace Logger {

enum class Level {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Off = 4,
};

void Init();
void Shutdown();

void SetLevel(Level level);
Level GetLevel();

bool ShouldLog(Level level);

void Logf(Level level, const wchar_t* fmt, ...);

} // namespace Logger

#ifndef EVERYARCHIVE_ENABLE_DEBUG_LOG
#if defined(NDEBUG)
#define EVERYARCHIVE_ENABLE_DEBUG_LOG 0
#else
#define EVERYARCHIVE_ENABLE_DEBUG_LOG 1
#endif
#endif

#define LOG_INFO(...) ::Logger::Logf(::Logger::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) ::Logger::Logf(::Logger::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::Logger::Logf(::Logger::Level::Error, __VA_ARGS__)

#if EVERYARCHIVE_ENABLE_DEBUG_LOG
#define LOG_DEBUG(...) \
    do { \
        if (::Logger::ShouldLog(::Logger::Level::Debug)) { \
            ::Logger::Logf(::Logger::Level::Debug, __VA_ARGS__); \
        } \
    } while (0)
#else
#define LOG_DEBUG(...) do { } while (0)
#endif
