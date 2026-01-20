#include "logger.h"

#include <shlwapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#pragma comment(lib, "Shlwapi.lib")

namespace Logger {

static std::mutex g_mutex;
static std::wstring g_logFilePath;
static std::atomic<Level> g_level{ Level::Info };
static std::atomic<bool> g_initialized{ false };

static bool g_consoleReady = false;
static bool g_consoleExplicit = false;

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

static std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) {
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
    }
    return s;
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
        g_logFilePath = L"EveryArchive.log";
        return;
    }

    wchar_t path[MAX_PATH]{};
    if (!PathCombineW(path, tempPath, L"EveryArchive.log")) {
        g_logFilePath = L"EveryArchive.log";
        return;
    }

    g_logFilePath = path;
}

static void EnsureConsoleReady() {
    if (g_consoleReady) return;

    const std::wstring mode = ToLower(GetEnvVar(L"EVERYARCHIVE_LOG_CONSOLE"));
    g_consoleExplicit = !mode.empty();

    bool ok = false;
    if (mode == L"0" || mode == L"false" || mode == L"off" || mode == L"no") {
        ok = false;
    } else if (mode == L"alloc" || mode == L"new") {
        ok = AllocConsole() != FALSE;
    } else {
        ok = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        if (!ok && (mode == L"1" || mode == L"true" || mode == L"on" || mode == L"yes")) {
            ok = AllocConsole() != FALSE;
        }
    }

    g_consoleReady = ok;
}

static void WriteToConsole(const wchar_t* line) {
    if (!line) return;

    if (!g_consoleReady) {
        if (g_consoleExplicit) {
            EnsureConsoleReady();
        }
        if (!g_consoleReady) return;
    }

    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;

    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, line, (DWORD)wcslen(line), &written, nullptr);
    } else {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    }
}

static void WriteLine(Level level, const wchar_t* message) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    DWORD tid = GetCurrentThreadId();

    wchar_t line[4096]{};
    _snwprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [tid:%lu] %s\r\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        LevelToString(level),
        (unsigned long)tid,
        message ? message : L""
    );

    OutputDebugStringW(line);
    WriteToConsole(line);

    EnsureLogFilePathInitialized();
    HANDLE h = CreateFileW(
        g_logFilePath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD cb = (DWORD)(wcslen(line) * sizeof(wchar_t));
        DWORD written = 0;
        WriteFile(h, line, cb, &written, nullptr);
        CloseHandle(h);
    }
}

void Init() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized.load()) return;

#if defined(NDEBUG)
    g_level.store(Level::Info);
#else
    g_level.store(Level::Debug);
#endif

    const std::wstring levelEnv = GetEnvVar(L"EVERYARCHIVE_LOG_LEVEL");
    if (!levelEnv.empty()) {
        g_level.store(ParseLevel(levelEnv));
    }

    EnsureLogFilePathInitialized();
    EnsureConsoleReady();

    g_initialized.store(true);
}

void Shutdown() {
    g_initialized.store(false);
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

} // namespace Logger
