#include "logger.h"

#include <shlwapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "string_utils.h"

#pragma comment(lib, "Shlwapi.lib")

namespace Logger {

static std::mutex g_mutex;
static std::wstring g_logFilePath;
static std::atomic<Level> g_level{ Level::Info };
static std::atomic<bool> g_initialized{ false };
static HANDLE g_logFileHandle = INVALID_HANDLE_VALUE;

static bool g_consoleReady = false;
static bool g_consoleExplicit = false;
static bool g_consoleAttached = false;
static HANDLE g_stderrHandle = INVALID_HANDLE_VALUE;

static const wchar_t* LevelToString(Level level) {
    switch (level) {
    case Level::Debug:
        return L"DEBUG";
    case Level::Info:
        return L"INFO";
    case Level::Warn:
        return L"WARN";
    case Level::Error:
        return L"ERROR";
    case Level::Off:
        return L"OFF";
    default:
        return L"?";
    }
}

static std::wstring GetEnvVar(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return L"";

    std::wstring value;
    value.resize(needed);
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) return L"";

    if (!value.empty() && value.back() == 0) {
        value.pop_back();
    }
    return value;
}

static Level ParseLevel(const std::wstring& s) {
    const std::wstring v = ToLower(s);

    if (v == L"debug") return Level::Debug;
    if (v == L"info") return Level::Info;
    if (v == L"warn" || v == L"warning") return Level::Warn;
    if (v == L"error") return Level::Error;
    if (v == L"off" || v == L"none") return Level::Off;

    return Level::Info;
}

static void EnsureLogFilePathInitialized() {
    if (!g_logFilePath.empty()) return;

    wchar_t tempPath[MAX_PATH]{};
    DWORD len = GetTempPathW((DWORD)(sizeof(tempPath) / sizeof(tempPath[0])), tempPath);
    if (len == 0 || len >= (DWORD)(sizeof(tempPath) / sizeof(tempPath[0]))) {
        g_logFilePath = L"everyzip.log";
        return;
    }

    wchar_t path[MAX_PATH]{};
    if (!PathCombineW(path, tempPath, L"everyzip.log")) {
        g_logFilePath = L"everyzip.log";
        return;
    }

    g_logFilePath = path;
}

static void EnsureConsoleReady() {
    if (g_consoleReady) return;

    const std::wstring mode = ToLower(GetEnvVar(L"EVERYZIP_LOG_CONSOLE"));
    g_consoleExplicit = !mode.empty();

    if (mode == L"0" || mode == L"false" || mode == L"off" || mode == L"no") {
        g_consoleReady = false;
        return;
    }

    // First, check if we already have a valid inherited stderr handle (e.g. CLion pipe).
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        g_stderrHandle = h;
        SetConsoleOutputCP(CP_UTF8);
        g_consoleReady = true;
        return;
    }

    // No inherited handle: try to attach or allocate a console.
    bool ok = false;
    if (mode == L"alloc" || mode == L"new") {
        ok = AllocConsole() != FALSE;
    } else {
        ok = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        if (!ok && (mode == L"1" || mode == L"true" || mode == L"on" || mode == L"yes")) {
            ok = AllocConsole() != FALSE;
        }
    }
    if (ok) {
        SetConsoleOutputCP(CP_UTF8);
        g_stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
        g_consoleAttached = true;
    }
    g_consoleReady = ok;
}

static void WriteToConsoleUtf8(const char* utf8, DWORD len) {
    if (!utf8 || len == 0) return;

    if (!g_consoleReady) {
        if (g_consoleExplicit) {
            EnsureConsoleReady();
        }
        if (!g_consoleReady) return;
    }

    HANDLE h = (g_stderrHandle != INVALID_HANDLE_VALUE) ? g_stderrHandle : GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;

    DWORD written = 0;
    WriteFile(h, utf8, len, &written, nullptr);
}

static void WriteLineUtf8(Level level, const wchar_t* wideMsg, const char* utf8Msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    // 构造宽字符完整行，仅供 OutputDebugStringW 使用
    wchar_t wideLine[4096]{};
    _snwprintf_s(
        wideLine,
        _countof(wideLine),
        _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [tid:%lu] %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        LevelToString(level),
        (unsigned long)tid,
        wideMsg ? wideMsg : L""
    );
    OutputDebugStringW(wideLine);

    // 构造 UTF-8 完整行（单次转换，同时用于控制台和文件）
    // 先用栈缓冲，超出时回退到堆分配
    char stackBuf[8192];
    int headerLen = _snprintf_s(
        stackBuf, sizeof(stackBuf), _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [tid:%lu] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        // level 标签为纯 ASCII，直接窄字节写入
        [level]() -> const char* {
            switch (level) {
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
            case Level::Off:   return "OFF";
            default:           return "?";
            }
        }(),
        (unsigned long)tid
    );
    if (headerLen < 0) headerLen = 0;

    // 追加消息体到栈缓冲（utf8Msg 已是 UTF-8）
    const char* body = utf8Msg ? utf8Msg : "";
    const size_t bodyLen = strlen(body);
    const size_t tailLen = 2; // "\r\n"
    const size_t totalLen = (size_t)headerLen + bodyLen + tailLen;

    char* utf8Line = stackBuf;
    std::string heapBuf;
    if (totalLen + 1 > sizeof(stackBuf)) {
        heapBuf.resize(totalLen + 1);
        memcpy(heapBuf.data(), stackBuf, (size_t)headerLen);
        utf8Line = heapBuf.data();
    }
    memcpy(utf8Line + headerLen, body, bodyLen);
    utf8Line[headerLen + bodyLen]     = '\r';
    utf8Line[headerLen + bodyLen + 1] = '\n';
    utf8Line[headerLen + bodyLen + 2] = '\0';

    WriteToConsoleUtf8(utf8Line, (DWORD)totalLen);

    // 使用持久化文件句柄，避免每条日志都 CreateFileW/CloseHandle
    if (g_logFileHandle == INVALID_HANDLE_VALUE) {
        EnsureLogFilePathInitialized();
        g_logFileHandle = CreateFileW(
            g_logFilePath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }
    if (g_logFileHandle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFileHandle, utf8Line, (DWORD)totalLen, &written, nullptr);
    }
}

static void WriteLine(Level level, const wchar_t* message) {
    // Wide→UTF-8 单次转换，结果复用于控制台和文件输出
    char utf8Msg[4096];
    const wchar_t* src = message ? message : L"";
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, src, -1, utf8Msg, (int)sizeof(utf8Msg), nullptr, nullptr);
    if (utf8Len <= 0) utf8Msg[0] = '\0';
    WriteLineUtf8(level, message, utf8Msg);
}

void Init() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized.load()) return;

#if defined(NDEBUG)
    g_level.store(Level::Info);
#else
    g_level.store(Level::Debug);
#endif

    const std::wstring levelEnv = GetEnvVar(L"EVERYZIP_LOG_LEVEL");
    if (!levelEnv.empty()) {
        g_level.store(ParseLevel(levelEnv));
    }

    EnsureLogFilePathInitialized();
    EnsureConsoleReady();

    g_initialized.store(true);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_initialized.store(false);
    if (g_logFileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFileHandle);
        g_logFileHandle = INVALID_HANDLE_VALUE;
    }
    if (g_consoleAttached) {
        FreeConsole();
        g_consoleAttached = false;
    }
    g_consoleReady = false;
    g_stderrHandle = INVALID_HANDLE_VALUE;
}

void SetLevel(Level level) {
    g_level.store(level);
}

Level GetLevel() {
    return g_level.load();
}

bool ShouldLog(Level level) {
    const Level cur = g_level.load();
    return level >= cur && cur != Level::Off;
}

void Logf(Level level, const wchar_t* fmt, ...) {
    if (!g_initialized.load()) {
        Init();
    }

    if (!ShouldLog(level)) return;

    wchar_t msg[2048]{};

    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt ? fmt : L"", ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mutex);
    WriteLine(level, msg);
}

void Logf(Level level, const char* fmt, ...) {
    if (!g_initialized.load()) {
        Init();
    }

    if (!ShouldLog(level)) return;

    // UTF-8 格式化到栈缓冲
    char utf8Msg[2048]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(utf8Msg, _countof(utf8Msg), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);

    // OutputDebugStringW 需要宽字符，按需转换一次；控制台和文件直接用 UTF-8
    wchar_t wideMsg[2048]{};
    MultiByteToWideChar(CP_UTF8, 0, utf8Msg, -1, wideMsg, _countof(wideMsg));

    std::lock_guard<std::mutex> lk(g_mutex);
    WriteLineUtf8(level, wideMsg, utf8Msg);
}

} // namespace Logger
