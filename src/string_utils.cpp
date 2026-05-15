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

std::wstring MultiByteToWString(const std::string& s, UINT codepage, DWORD flags) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(codepage, flags, s.data(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out;
    out.resize((size_t)needed);
    if (MultiByteToWideChar(codepage, flags, s.data(), (int)s.size(), out.data(), needed) <= 0) {
        return {};
    }
    return out;
}

std::wstring Utf16UnitsToWString(const uint16_t* src, size_t lenWithNull) {
    if (!src || lenWithNull == 0) return {};
    size_t len = lenWithNull;
    if (len > 0 && src[len - 1] == 0) {
        --len;
    }
    std::wstring out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back((wchar_t)src[i]);
    }
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

std::wstring AddThousandsSeparator(const std::wstring& num) {
    const int len = (int)num.size();
    if (len <= 3) return num;
    std::wstring result;
    result.reserve(len + (len - 1) / 3);
    const int firstGroup = ((len - 1) % 3) + 1;  // 第一组的字符数（1~3）
    for (int i = 0; i < len; ++i) {
        if (i > 0 && (i - firstGroup) % 3 == 0) {
            result.push_back(L',');
        }
        result.push_back(num[i]);
    }
    return result;
}

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

uint64_t LocalTmToFileTimeValue(const std::tm& t) {
    const int year = t.tm_year + 1900;
    const int month = t.tm_mon + 1;
    if (year <= 1900 || month < 1 || month > 12 || t.tm_mday < 1 || t.tm_mday > 31) {
        return 0;
    }

    SYSTEMTIME localSt{};
    localSt.wYear = static_cast<WORD>(year);
    localSt.wMonth = static_cast<WORD>(month);
    localSt.wDay = static_cast<WORD>(t.tm_mday);
    localSt.wHour = static_cast<WORD>(t.tm_hour);
    localSt.wMinute = static_cast<WORD>(t.tm_min);
    localSt.wSecond = static_cast<WORD>(t.tm_sec);

    SYSTEMTIME utcSt{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &localSt, &utcSt)) {
        return 0;
    }

    FILETIME ft{};
    if (!SystemTimeToFileTime(&utcSt, &ft)) {
        return 0;
    }

    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return ui.QuadPart;
}

std::wstring FormatFileTimeValueLocal(uint64_t v) {
    if (v == 0) return L"";
    return FormatFileTimeLocal(U64ToFileTime(v));
}

std::wstring FormatFileTimeLocal(const FILETIME& ftUtc) {
    FILETIME ftLocal{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal)) return L"";
    if (!FileTimeToSystemTime(&ftLocal, &st)) return L"";

    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u/%02u/%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

FILETIME U64ToFileTime(uint64_t v) {
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)((v >> 32) & 0xFFFFFFFFu);
    return ft;
}
