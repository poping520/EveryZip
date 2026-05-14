#include "license_text.h"

#include "../resource.h"

#include <string>

namespace {

void AppendSection(std::wstring& out, const wchar_t* title) {
    out += L"\r\n\r\n";
    out += L"============================================================\r\n";
    out += title;
    out += L"\r\n";
    out += L"============================================================\r\n\r\n";
}

std::wstring Utf8ToWide(const char* text, int size) {
    if (!text || size <= 0) return L"";
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, text, size, nullptr, 0);
    if (wideSize <= 0) {
        wideSize = MultiByteToWideChar(CP_ACP, 0, text, size, nullptr, 0);
        if (wideSize <= 0) return L"";
        std::wstring result(wideSize, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text, size, result.data(), wideSize);
        return result;
    }
    std::wstring result(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, size, result.data(), wideSize);
    return result;
}

std::wstring LoadTextResource(HINSTANCE hInstance, int resourceId) {
    HRSRC hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return L"";

    HGLOBAL hData = LoadResource(hInstance, hRes);
    if (!hData) return L"";

    const DWORD size = SizeofResource(hInstance, hRes);
    const char* bytes = static_cast<const char*>(LockResource(hData));
    return Utf8ToWide(bytes, static_cast<int>(size));
}

void AppendResource(std::wstring& out, HINSTANCE hInstance, int resourceId, const wchar_t* title) {
    AppendSection(out, title);
    out += LoadTextResource(hInstance, resourceId);
}

}

std::wstring BuildLicenseText(HINSTANCE hInstance) {
    std::wstring text =
        L"EveryZip\r\n\r\n"
        L"Copyright (c) 2026 poping520\r\n"
        L"EveryZip is open source software licensed under the MIT License.\r\n\r\n"
        L"Third-party components:\r\n\r\n"
        L"1. SQLite\r\n"
        L"   SQLite source code is in the public domain.\r\n"
        L"   Used by EveryZip for local database storage.\r\n\r\n"
        L"2. zlib\r\n"
        L"   Copyright (C) 1995-2022 Jean-loup Gailly and Mark Adler.\r\n"
        L"   Licensed under the zlib License.\r\n\r\n"
        L"3. MiniZip\r\n"
        L"   Copyright (c) 1998-2010 Gilles Vollant and contributors.\r\n"
        L"   Licensed under the same terms as zlib.\r\n\r\n"
        L"4. 7-Zip C SDK\r\n"
        L"   Copyright (C) 1999-2026 Igor Pavlov.\r\n"
        L"   Licensed mainly under GNU LGPL version 2.1 or later.\r\n"
        L"   Some files in the 7-Zip source tree may use BSD or public-domain terms,\r\n"
        L"   as stated in the 7-Zip license file.\r\n\r\n"
        L"5. UnRAR\r\n"
        L"   Copyright (c) Alexander Roshal.\r\n"
        L"   UnRAR source code may be used to handle RAR archives, but must not be used\r\n"
        L"   to develop a RAR/WinRAR compatible archiver or re-create the RAR compression\r\n"
        L"   algorithm.\r\n\r\n"
        L"Full license texts are included below.";

    AppendResource(text, hInstance, IDR_LICENSE_PROJECT, L"EveryZip MIT License");

    AppendSection(text, L"SQLite Public Domain Notice");
    text +=
        L"SQLite source code is in the public domain.\r\n\r\n"
        L"From sqlite3.h:\r\n"
        L"The author disclaims copyright to this source code. In place of\r\n"
        L"a legal notice, here is a blessing:\r\n\r\n"
        L"   May you do good and not evil.\r\n"
        L"   May you find forgiveness for yourself and forgive others.\r\n"
        L"   May you share freely, never taking more than you give.\r\n";

    AppendResource(text, hInstance, IDR_LICENSE_ZLIB, L"zlib License");
    AppendResource(text, hInstance, IDR_LICENSE_MINIZIP, L"MiniZip Notice and License");
    AppendResource(text, hInstance, IDR_LICENSE_7ZIP, L"7-Zip License");
    AppendResource(text, hInstance, IDR_LICENSE_7ZIP_LGPL, L"GNU Lesser General Public License 2.1");
    AppendResource(text, hInstance, IDR_LICENSE_UNRAR, L"UnRAR License");

    return text;
}
