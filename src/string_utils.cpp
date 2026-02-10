#include "string_utils.h"

#include <algorithm>
#include <cwctype>

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) {
        ch = (wchar_t)towlower(ch);
    }
    return s;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out;
    out.resize((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWString(const char* s) {
    if (!s) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring w;
    w.resize((size_t)(needed - 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), needed);
    return w;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring GetEntryNameFromPath(const std::wstring& path) {
    if (path.empty()) return path;
    size_t pos = path.find_last_of(L"/\\");
    if (pos == std::wstring::npos) return path;
    if (pos + 1 >= path.size()) return L"";
    return path.substr(pos + 1);
}

// 为整数字符串添加千位分隔符（如 "1234567" → "1,234,567"）
std::wstring AddThousandsSeparator(const std::wstring& num) {
    std::wstring result;
    int count = 0;
    for (int i = (int)num.size() - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) {
            result.insert(result.begin(), L',');
        }
        result.insert(result.begin(), num[i]);
        ++count;
    }
    return result;
}

// 将字节数格式化为 KB 单位的字符串（>=1KB 取整数并添加千位分隔符，<1KB 保留两位小数）
std::wstring FormatSizeULongLong(ULONGLONG v) {
    double kb = (double)v / 1024.0;
    if (kb >= 1.0) {
        // >=1KB：取整后添加千位分隔符
        ULONGLONG kbInt = (ULONGLONG)(kb + 0.5);
        std::wstring numStr = std::to_wstring(kbInt);
        return AddThousandsSeparator(numStr) + L" KB";
    } else {
        // <1KB：保留两位小数
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.2f KB", kb);
    return buf;
    }
}

// 将 UTC FILETIME 转换为本地时间字符串
std::wstring FormatFileTimeLocal(const FILETIME& ftUtc) {
    FILETIME ftLocal{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal)) return L"";
    if (!FileTimeToSystemTime(&ftLocal, &st)) return L"";

    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u/%u/%u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

FILETIME U64ToFileTime(uint64_t v) {
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)((v >> 32) & 0xFFFFFFFFu);
    return ft;
}
