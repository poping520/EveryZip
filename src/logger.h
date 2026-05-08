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
 * 使用格式化参数写入一条日志（宽字节接口）。
 * @param level 日志级别。
 * @param fmt 宽字符串格式模板。
 */
void Logf(Level level, const wchar_t* fmt, ...);
/**
 * 使用格式化参数写入一条日志（窄字节接口，格式串须为 UTF-8）。
 * @param level 日志级别。
 * @param fmt UTF-8 窄字符串格式模板。
 */
void Logf(Level level, const char* fmt, ...);

} // namespace Logger

#ifndef EVERYZIP_ENABLE_DEBUG_LOG
#if defined(NDEBUG)
#define EVERYZIP_ENABLE_DEBUG_LOG 0
#else
#define EVERYZIP_ENABLE_DEBUG_LOG 1
#endif
#endif

// 宽字节宏（W 后缀），格式串使用 L"..." 字面量
#define LOG_INFO_W(...)  ::Logger::Logf(::Logger::Level::Info,  __VA_ARGS__)
#define LOG_WARN_W(...)  ::Logger::Logf(::Logger::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR_W(...) ::Logger::Logf(::Logger::Level::Error, __VA_ARGS__)

// 窄字节宏（A 后缀），格式串使用 UTF-8 字面量
#define LOG_INFO_A(...)  ::Logger::Logf(::Logger::Level::Info,  __VA_ARGS__)
#define LOG_WARN_A(...)  ::Logger::Logf(::Logger::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR_A(...) ::Logger::Logf(::Logger::Level::Error, __VA_ARGS__)

// 默认宏（保持向后兼容，等价于宽字节版本）
#define LOG_INFO(...)  ::Logger::Logf(::Logger::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::Logger::Logf(::Logger::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::Logger::Logf(::Logger::Level::Error, __VA_ARGS__)

#if EVERYZIP_ENABLE_DEBUG_LOG
#define LOG_DEBUG_W(...) \
    do { \
        if (::Logger::ShouldLog(::Logger::Level::Debug)) { \
            ::Logger::Logf(::Logger::Level::Debug, __VA_ARGS__); \
        } \
    } while (0)
#define LOG_DEBUG_A(...) \
    do { \
        if (::Logger::ShouldLog(::Logger::Level::Debug)) { \
            ::Logger::Logf(::Logger::Level::Debug, __VA_ARGS__); \
        } \
    } while (0)
#define LOG_DEBUG(...) \
    do { \
        if (::Logger::ShouldLog(::Logger::Level::Debug)) { \
            ::Logger::Logf(::Logger::Level::Debug, __VA_ARGS__); \
        } \
    } while (0)
#else
#define LOG_DEBUG_W(...) do { } while (0)
#define LOG_DEBUG_A(...) do { } while (0)
#define LOG_DEBUG(...)   do { } while (0)
#endif
