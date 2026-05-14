#include "about_window.h"

#include <commctrl.h>

#include "../resource.h"
#include "window_utils.h"

namespace {

constexpr wchar_t kAboutClass[] = L"EveryZipAboutWindow";
constexpr int IDC_ABOUT_LINK_CTRL = 3040;

struct AboutWindowState {
    HWND owner = nullptr;
    MainWindowState* mainState = nullptr;
    HFONT hFont = nullptr;
    HFONT hTitleFont = nullptr;
    HICON hIcon = nullptr;
};

std::wstring GetAboutProjectUrl() {
    return L"https://github.com/poping520/EveryZip";
}

LRESULT CALLBACK AboutWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* aws = (AboutWindowState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        aws = (AboutWindowState*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)aws);

        MainWindowState* s = aws ? aws->mainState : nullptr;
        const UINT dpi = GetWindowDpiValue(hWnd);
        const int margin = ScaleDpiValue(20, dpi);
        const int iconSize = ScaleDpiValue(48, dpi);
        const int contentTop = margin;
        const int contentLeft = margin + iconSize + ScaleDpiValue(16, dpi);
        const int contentWidth = ScaleDpiValue(236, dpi);
        const int lineH = ScaleDpiValue(22, dpi);
        const int titleH = ScaleDpiValue(30, dpi);
        const int buttonW = ScaleDpiValue(88, dpi);
        const int buttonH = ScaleDpiValue(28, dpi);
        const int buttonY = ScaleDpiValue(166, dpi);

        HWND hIcon = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_ICON,
            margin, contentTop + ScaleDpiValue(2, dpi), iconSize, iconSize,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);

        HWND hTitle = CreateWindowExW(0, L"STATIC",
            s ? LoadStateString(s, IDS_ABOUT_TEXT).c_str() : L"EveryZip",
            WS_CHILD | WS_VISIBLE,
            contentLeft, contentTop, contentWidth, titleH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);

        const std::wstring versionText = s ? LoadStateString(s, IDS_ABOUT_VERSION) : L"";
        HWND hVersion = CreateWindowExW(0, L"STATIC",
            versionText.c_str(),
            WS_CHILD | WS_VISIBLE,
            contentLeft, contentTop + titleH, contentWidth, lineH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);

        const std::wstring summaryText = s ? LoadStateString(s, IDS_ABOUT_SUMMARY) : L"";
        HWND hSummary = CreateWindowExW(0, L"STATIC",
            summaryText.c_str(),
            WS_CHILD | WS_VISIBLE,
            margin, contentTop + iconSize + ScaleDpiValue(16, dpi), ScaleDpiValue(300, dpi), lineH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);

        std::wstring linkText = L"<a href=\"";
        linkText += GetAboutProjectUrl();
        linkText += L"\">";
        linkText += GetAboutProjectUrl();
        linkText += L"</a>";

        HWND hLink = CreateWindowExW(0, L"SysLink", linkText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            margin, contentTop + iconSize + ScaleDpiValue(44, dpi), ScaleDpiValue(300, dpi), lineH,
            hWnd, (HMENU)(INT_PTR)IDC_ABOUT_LINK_CTRL, s ? s->hInstance : nullptr, nullptr);

        HWND hOk = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_OK).c_str() : L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            (ScaleDpiValue(336, dpi) - buttonW) / 2, buttonY, buttonW, buttonH,
            hWnd, (HMENU)(INT_PTR)IDOK, s ? s->hInstance : nullptr, nullptr);

        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            aws->hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            LOGFONTW titleLf = ncm.lfMessageFont;
            titleLf.lfWeight = FW_SEMIBOLD;
            titleLf.lfHeight = -ScaleDpiValue(18, dpi);
            aws->hTitleFont = CreateFontIndirectW(&titleLf);
        }

        if (aws) {
            aws->hIcon = (HICON)LoadImageW(
                s ? s->hInstance : GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_APP_ICON),
                IMAGE_ICON,
                iconSize,
                iconSize,
                LR_DEFAULTCOLOR);
        }

        if (hIcon && aws && aws->hIcon) {
            SendMessageW(hIcon, STM_SETIMAGE, IMAGE_ICON, (LPARAM)aws->hIcon);
        }

        if (aws && aws->hFont) {
            HWND controls[] = { hVersion, hSummary, hLink, hOk };
            for (HWND hCtrl : controls) {
                if (hCtrl) SendMessageW(hCtrl, WM_SETFONT, (WPARAM)aws->hFont, TRUE);
            }
        }
        if (hTitle && aws && aws->hTitleFont) {
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)aws->hTitleFont, TRUE);
        }
        if (hOk) SetFocus(hOk);
        return 0;
    }

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (wParam == IDC_ABOUT_LINK_CTRL) {
            auto* hdr = (NMHDR*)lParam;
            if (hdr && (hdr->code == NM_CLICK || hdr->code == NM_RETURN) && aws && aws->mainState) {
                const std::wstring url = GetAboutProjectUrl();
                ShellExecuteW(hWnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (aws) {
            if (aws->hFont) DeleteObject(aws->hFont);
            if (aws->hTitleFont) DeleteObject(aws->hTitleFont);
            if (aws->hIcon) DestroyIcon(aws->hIcon);
            aws->hFont = nullptr;
            aws->hTitleFont = nullptr;
            aws->hIcon = nullptr;
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterAboutClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kAboutClass;
    RegisterClassExW(&wc);
    registered = true;
}

}

void ShowAboutPanel(HWND hOwner, MainWindowState* s) {
    if (!s) return;

    RegisterAboutClass(s->hInstance);

    const UINT dpi = GetWindowDpiValue(hOwner);
    const int width = ScaleDpiValue(336, dpi);
    const int height = ScaleDpiValue(242, dpi);

    RECT ownerRc{};
    GetWindowRect(hOwner, &ownerRc);
    int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
    int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;

    AboutWindowState aws{};
    aws.owner = hOwner;
    aws.mainState = s;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        kAboutClass,
        LoadStateString(s, IDS_ABOUT_TITLE).c_str(),
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, width, height,
        hOwner,
        nullptr,
        s->hInstance,
        &aws);

    if (!hDlg) return;

    EnableWindow(hOwner, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg{};
    while (IsWindow(hDlg)) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) {
            if (ret == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            HWND target = msg.hwnd;
            if (target == hDlg || IsChild(hDlg, target)) {
                DestroyWindow(hDlg);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND target = msg.hwnd;
            if ((target == hDlg || IsChild(hDlg, target)) &&
                target != GetDlgItem(hDlg, IDC_ABOUT_LINK_CTRL)) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hOwner, TRUE);
    SetForegroundWindow(hOwner);
}
