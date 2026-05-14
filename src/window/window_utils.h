#pragma once

#include "main_window.h"

inline UINT GetWindowDpiValue(HWND hWnd) {
    using FnGetDpiForWindow = UINT(WINAPI*)(HWND);
    static auto fn = (FnGetDpiForWindow)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) return fn(hWnd);
    HDC hdc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi ? dpi : 96;
}

inline int ScaleDpiValue(int px, UINT dpi) {
    return MulDiv(px, (int)dpi, 96);
}

inline std::wstring LoadStateString(MainWindowState* s, UINT id) {
    return LS(s->hInstance, id, GetEffectiveLanguageId(s->userConfig));
}
