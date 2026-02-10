#pragma once

#include <windows.h>
#include <shlwapi.h>

#include <cwctype>
#include <string>

std::wstring ToLower(std::wstring s);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWString(const char* s);
std::wstring GetExeDir();
std::wstring GetEntryNameFromPath(const std::wstring& path);
std::wstring AddThousandsSeparator(const std::wstring& num);
std::wstring FormatSizeULongLong(ULONGLONG v);
std::wstring FormatFileTimeLocal(const FILETIME& ftUtc);
FILETIME U64ToFileTime(uint64_t v);
