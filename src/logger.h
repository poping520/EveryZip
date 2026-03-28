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

/** 初始化日志系统，设置日志级别、输出目标和文件句柄。 */
void Init();
/** 关闭日志系统并释放控制台、文件句柄等资源。 */
void Shutdown();

/**
 * 设置当前日志输出级别阈值。
 * @param level 新的最小输出级别。
 */
void SetLevel(Level level);
/**
 * 获取当前日志输出级别阈值。
 * @return 当前日志级别。
 */
Level GetLevel();

/**
 * 判断指定级别的日志当前是否应被输出。
 * @param level 待判断的日志级别。
 * @return 应输出返回 true，否则返回 false。
 */
bool ShouldLog(Level level);

/**
 * 使用格式化参数写入一条日志。
 * @param level 日志级别。
 * @param fmt 宽字符串格式模板。
 */
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
