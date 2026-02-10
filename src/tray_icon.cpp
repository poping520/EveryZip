#include "tray_icon.h"

#include <string>

#include "resource.h"

// 从 STRINGTABLE 资源加载本地化字符串
static std::wstring LS(HINSTANCE hInstance, UINT id) {
    const wchar_t* p = nullptr;
    int len = LoadStringW(hInstance, id, (LPWSTR)&p, 0);
    return (len > 0 && p) ? std::wstring(p, len) : std::wstring();
}

static NOTIFYICONDATAW g_nid{};

void AddTrayIcon(HWND hWnd, UINT uID, UINT uCallbackMessage) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = uID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = uCallbackMessage;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"EveryArchive");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd, HINSTANCE hInstance) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, LS(hInstance, IDS_TRAY_SHOW).c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, LS(hInstance, IDS_TRAY_EXIT).c_str());

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}
