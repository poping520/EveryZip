#include "update_prompt_window.h"

#include <algorithm>

#include "../app_version.h"
#include "../logger.h"
#include "../resource.h"
#include "window_utils.h"

namespace {

constexpr wchar_t kUpdatePromptClass[] = L"EveryZipUpdatePromptWindow";
constexpr int IDC_UPDATE_ICON = 4100;
constexpr int IDC_UPDATE_FOUND = 4101;
constexpr int IDC_UPDATE_RELEASE_TITLE = 4102;
constexpr int IDC_UPDATE_CURRENT_LABEL = 4103;
constexpr int IDC_UPDATE_CURRENT_VALUE = 4104;
constexpr int IDC_UPDATE_LATEST_LABEL = 4105;
constexpr int IDC_UPDATE_LATEST_VALUE = 4106;
constexpr int IDC_UPDATE_PUBLISHED_LABEL = 4107;
constexpr int IDC_UPDATE_PUBLISHED_VALUE = 4108;
constexpr int IDC_UPDATE_NOTES_LABEL = 4109;
constexpr int IDC_UPDATE_NOTES = 4110;
constexpr int IDC_UPDATE_NOW = 4111;
constexpr int IDC_UPDATE_DISABLE_REMINDER = 4112;

struct UpdatePromptState {
    HWND owner = nullptr;
    MainWindowState* mainState = nullptr;
    const EveryZip::ReleaseInfo* release = nullptr;
    UpdatePromptChoice choice = UpdatePromptChoice::Cancel;
    HFONT hFont = nullptr;
    HFONT hBoldFont = nullptr;
    HFONT hTitleFont = nullptr;
    HBRUSH hWindowBrush = nullptr;
    HBRUSH hNotesBrush = nullptr;
    bool done = false;
};

const wchar_t* UpdatePromptChoiceName(UpdatePromptChoice choice) {
    switch (choice) {
    case UpdatePromptChoice::UpdateNow:
        return L"UpdateNow";
    case UpdatePromptChoice::DisableReminder:
        return L"DisableReminder";
    case UpdatePromptChoice::Cancel:
    default:
        return L"Cancel";
    }
}

std::wstring NormalizeReleaseBody(const std::wstring& body) {
    std::wstring text = body;
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
        text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    if (text.empty()) return L"-";

    std::wstring normalized;
    normalized.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            normalized += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (text[i] == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(text[i]);
        }
    }
    return normalized;
}

bool ParseGitHubUtcTime(const std::wstring& text, SYSTEMTIME* utc) {
    if (!utc || text.size() < 20 || text[4] != L'-' || text[7] != L'-' ||
        text[10] != L'T' || text[13] != L':' || text[16] != L':' || text[19] != L'Z') {
        return false;
    }

    SYSTEMTIME parsed{};
    parsed.wYear = (WORD)_wtoi(text.substr(0, 4).c_str());
    parsed.wMonth = (WORD)_wtoi(text.substr(5, 2).c_str());
    parsed.wDay = (WORD)_wtoi(text.substr(8, 2).c_str());
    parsed.wHour = (WORD)_wtoi(text.substr(11, 2).c_str());
    parsed.wMinute = (WORD)_wtoi(text.substr(14, 2).c_str());
    parsed.wSecond = (WORD)_wtoi(text.substr(17, 2).c_str());
    if (parsed.wYear == 0 || parsed.wMonth < 1 || parsed.wMonth > 12 ||
        parsed.wDay < 1 || parsed.wDay > 31 || parsed.wHour > 23 ||
        parsed.wMinute > 59 || parsed.wSecond > 59) {
        return false;
    }
    *utc = parsed;
    return true;
}

std::wstring FormatPublishedAtLocal(const std::wstring& publishedAt) {
    if (publishedAt.empty()) return L"-";

    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!ParseGitHubUtcTime(publishedAt, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return publishedAt;
    }

    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
        local.wYear, local.wMonth, local.wDay,
        local.wHour, local.wMinute, local.wSecond);
    return buf;
}

void FinishPrompt(HWND hWnd, UpdatePromptState* state, UpdatePromptChoice choice) {
    LOG_INFO(L"Update prompt finished: choice=%s", UpdatePromptChoiceName(choice));
    if (state) {
        state->choice = choice;
        state->done = true;
    }
    DestroyWindow(hWnd);
}

void SetControlFont(HWND hWnd, int id, HFONT font) {
    HWND hCtrl = GetDlgItem(hWnd, id);
    if (hCtrl && font) {
        SendMessageW(hCtrl, WM_SETFONT, (WPARAM)font, TRUE);
    }
}

HWND CreateLabel(HWND parent, MainWindowState* s, int id, const std::wstring& text,
    int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, (HMENU)(INT_PTR)id,
        s ? s->hInstance : nullptr, nullptr);
}

LRESULT CALLBACK UpdatePromptWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = (UpdatePromptState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        state = (UpdatePromptState*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)state);

        MainWindowState* s = state ? state->mainState : nullptr;
        const auto* release = state ? state->release : nullptr;
        if (release) {
            LOG_INFO(L"Create update prompt controls: tag=%s version=%s asset=%s url=%s",
                release->tagName.c_str(),
                EveryZip::AppVersionToString(release->version).c_str(),
                release->assetName.c_str(),
                release->htmlUrl.c_str());
        } else {
            LOG_WARN(L"Create update prompt controls without release info");
        }
        const UINT dpi = GetWindowDpiValue(hWnd);
        const int margin = ScaleDpiValue(18, dpi);
        const int iconSize = ScaleDpiValue(32, dpi);
        const int gap = ScaleDpiValue(10, dpi);
        const int lineH = ScaleDpiValue(22, dpi);
        const int labelW = ScaleDpiValue(86, dpi);
        const int valueX = margin + labelW;
        const int valueW = ScaleDpiValue(280, dpi);
        const int contentW = ScaleDpiValue(420, dpi);
        const int buttonW = ScaleDpiValue(122, dpi);
        const int buttonH = ScaleDpiValue(30, dpi);
        const int buttonGap = ScaleDpiValue(12, dpi);
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            state->hFont = CreateFontIndirectW(&ncm.lfMessageFont);

            LOGFONTW boldLf = ncm.lfMessageFont;
            boldLf.lfWeight = FW_SEMIBOLD;
            state->hBoldFont = CreateFontIndirectW(&boldLf);

            LOGFONTW titleLf = ncm.lfMessageFont;
            titleLf.lfWeight = FW_SEMIBOLD;
            titleLf.lfHeight = -ScaleDpiValue(18, dpi);
            state->hTitleFont = CreateFontIndirectW(&titleLf);
        }
        state->hWindowBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        state->hNotesBrush = CreateSolidBrush(RGB(245, 245, 245));

        HICON hInfoIcon = LoadIconW(nullptr, IDI_INFORMATION);
        HWND hIcon = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_ICON,
            margin, margin, iconSize, iconSize, hWnd, (HMENU)(INT_PTR)IDC_UPDATE_ICON,
            s ? s->hInstance : nullptr, nullptr);
        if (hIcon && hInfoIcon) {
            SendMessageW(hIcon, STM_SETICON, (WPARAM)hInfoIcon, 0);
        }

        const std::wstring releaseName = release && !release->name.empty()
            ? release->name
            : (release ? release->tagName : L"");
        const std::wstring titleText = releaseName;
        const std::wstring currentVersion = EveryZip::AppVersionWString();
        const std::wstring latestVersion = release ? EveryZip::AppVersionToString(release->version) : L"";
        const std::wstring publishedAt = FormatPublishedAtLocal(release ? release->publishedAt : L"");
        const std::wstring notes = NormalizeReleaseBody(release ? release->body : L"");

        CreateLabel(hWnd, s, IDC_UPDATE_FOUND, LoadStateString(s, IDS_UPDATE_FOUND_SHORT),
            margin + iconSize + gap, margin + ScaleDpiValue(5, dpi),
            contentW - iconSize - gap, lineH);
        CreateLabel(hWnd, s, IDC_UPDATE_RELEASE_TITLE, titleText,
            margin, margin + iconSize + ScaleDpiValue(16, dpi), contentW, ScaleDpiValue(28, dpi));

        int infoY = margin + iconSize + ScaleDpiValue(52, dpi);
        CreateLabel(hWnd, s, IDC_UPDATE_CURRENT_LABEL, LoadStateString(s, IDS_UPDATE_CURRENT_VERSION) + L":",
            margin, infoY, labelW, lineH);
        CreateLabel(hWnd, s, IDC_UPDATE_CURRENT_VALUE, currentVersion, valueX, infoY, valueW, lineH);
        infoY += lineH;
        CreateLabel(hWnd, s, IDC_UPDATE_LATEST_LABEL, LoadStateString(s, IDS_UPDATE_LATEST_VERSION) + L":",
            margin, infoY, labelW, lineH);
        CreateLabel(hWnd, s, IDC_UPDATE_LATEST_VALUE, latestVersion, valueX, infoY, valueW, lineH);
        infoY += lineH;
        CreateLabel(hWnd, s, IDC_UPDATE_PUBLISHED_LABEL, LoadStateString(s, IDS_UPDATE_PUBLISHED_AT) + L":",
            margin, infoY, labelW, lineH);
        CreateLabel(hWnd, s, IDC_UPDATE_PUBLISHED_VALUE, publishedAt, valueX, infoY, valueW, lineH);

        const int notesLabelY = infoY + lineH + ScaleDpiValue(12, dpi);
        CreateLabel(hWnd, s, IDC_UPDATE_NOTES_LABEL, LoadStateString(s, IDS_UPDATE_NOTES),
            margin, notesLabelY, contentW, lineH);

        const int notesY = notesLabelY + lineH + ScaleDpiValue(4, dpi);
        const int notesH = ScaleDpiValue(96, dpi);
        HWND hNotes = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            notes.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            margin,
            notesY,
            contentW,
            notesH,
            hWnd,
            (HMENU)(INT_PTR)IDC_UPDATE_NOTES,
            s ? s->hInstance : nullptr,
            nullptr);
        if (hNotes) {
            SendMessageW(hNotes, EM_SETSEL, 0, 0);
        } else {
            LOG_WARN(L"Create update prompt notes edit failed (err=%lu)", GetLastError());
        }

        const int buttonY = notesY + notesH + ScaleDpiValue(18, dpi);
        const int buttonsW = buttonW * 3 + buttonGap * 2;
        int buttonX = margin + contentW - buttonsW;
        CreateWindowExW(0, L"BUTTON", LoadStateString(s, IDS_UPDATE_NOW).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            buttonX, buttonY, buttonW, buttonH, hWnd,
            (HMENU)(INT_PTR)IDC_UPDATE_NOW, s ? s->hInstance : nullptr, nullptr);
        buttonX += buttonW + buttonGap;
        CreateWindowExW(0, L"BUTTON", LoadStateString(s, IDS_UPDATE_DISABLE_REMINDER).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            buttonX, buttonY, buttonW, buttonH, hWnd,
            (HMENU)(INT_PTR)IDC_UPDATE_DISABLE_REMINDER, s ? s->hInstance : nullptr, nullptr);
        buttonX += buttonW + buttonGap;
        CreateWindowExW(0, L"BUTTON", LoadStateString(s, IDS_SETTINGS_CANCEL).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            buttonX, buttonY, buttonW, buttonH, hWnd,
            (HMENU)(INT_PTR)IDCANCEL, s ? s->hInstance : nullptr, nullptr);

        int fontIds[] = {
            IDC_UPDATE_FOUND, IDC_UPDATE_CURRENT_LABEL, IDC_UPDATE_CURRENT_VALUE,
            IDC_UPDATE_LATEST_LABEL, IDC_UPDATE_PUBLISHED_LABEL,
            IDC_UPDATE_PUBLISHED_VALUE, IDC_UPDATE_NOTES_LABEL, IDC_UPDATE_NOTES,
            IDC_UPDATE_NOW, IDC_UPDATE_DISABLE_REMINDER, IDCANCEL
        };
        for (int id : fontIds) SetControlFont(hWnd, id, state->hFont);
        SetControlFont(hWnd, IDC_UPDATE_RELEASE_TITLE, state->hTitleFont);
        SetControlFont(hWnd, IDC_UPDATE_LATEST_VALUE, state->hBoldFont);

        SetFocus(GetDlgItem(hWnd, IDC_UPDATE_NOW));
        LOG_INFO(L"Update prompt controls created");
        return 0;
    }

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_UPDATE_NOW:
            FinishPrompt(hWnd, state, UpdatePromptChoice::UpdateNow);
            return 0;
        case IDC_UPDATE_DISABLE_REMINDER:
            FinishPrompt(hWnd, state, UpdatePromptChoice::DisableReminder);
            return 0;
        case IDCANCEL:
            FinishPrompt(hWnd, state, UpdatePromptChoice::Cancel);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        if (hCtrl == GetDlgItem(hWnd, IDC_UPDATE_NOTES)) {
            SetBkColor(hdc, RGB(245, 245, 245));
            return (LRESULT)(state ? state->hNotesBrush : GetSysColorBrush(COLOR_BTNFACE));
        }
        return (LRESULT)(state ? state->hWindowBrush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(245, 245, 245));
        return (LRESULT)(state ? state->hNotesBrush : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, state ? state->hWindowBrush : GetSysColorBrush(COLOR_WINDOW));

        const UINT dpi = GetWindowDpiValue(hWnd);
        RECT bottomRc = rc;
        bottomRc.top = rc.bottom - ScaleDpiValue(58, dpi);
        FillRect((HDC)wParam, &bottomRc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_CLOSE:
        FinishPrompt(hWnd, state, UpdatePromptChoice::Cancel);
        return 0;
    case WM_NCDESTROY:
        if (state) {
            if (state->hFont) { DeleteObject(state->hFont); state->hFont = nullptr; }
            if (state->hBoldFont) { DeleteObject(state->hBoldFont); state->hBoldFont = nullptr; }
            if (state->hTitleFont) { DeleteObject(state->hTitleFont); state->hTitleFont = nullptr; }
            if (state->hWindowBrush) { DeleteObject(state->hWindowBrush); state->hWindowBrush = nullptr; }
            if (state->hNotesBrush) { DeleteObject(state->hNotesBrush); state->hNotesBrush = nullptr; }
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterUpdatePromptClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = UpdatePromptWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kUpdatePromptClass;
    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_WARN(L"Register update prompt window class failed (err=%lu)", err);
            return;
        }
    }
    registered = true;
}

} // namespace

UpdatePromptChoice ShowUpdatePromptWindow(HWND owner, MainWindowState* state,
    const EveryZip::ReleaseInfo& release) {
    if (!state) {
        LOG_WARN(L"Show update prompt skipped: state is null");
        return UpdatePromptChoice::Cancel;
    }

    LOG_INFO(L"Show update prompt: current=%s latest=%s tag=%s asset=%s",
        EveryZip::AppVersionWString().c_str(),
        EveryZip::AppVersionToString(release.version).c_str(),
        release.tagName.c_str(),
        release.assetName.c_str());

    RegisterUpdatePromptClass(state->hInstance);

    const UINT dpi = owner ? GetWindowDpiValue(owner) : 96;
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    RECT windowRc{ 0, 0, ScaleDpiValue(456, dpi), ScaleDpiValue(372, dpi) };
    AdjustWindowRectEx(&windowRc, style, FALSE, exStyle);
    const int width = windowRc.right - windowRc.left;
    const int height = windowRc.bottom - windowRc.top;
    RECT ownerRc{};
    if (owner) GetWindowRect(owner, &ownerRc);
    int x = owner ? ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2 : CW_USEDEFAULT;
    int y = owner ? ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2 : CW_USEDEFAULT;

    UpdatePromptState promptState{};
    promptState.owner = owner;
    promptState.mainState = state;
    promptState.release = &release;

    HWND hDlg = CreateWindowExW(
        exStyle,
        kUpdatePromptClass,
        LoadStateString(state, IDS_UPDATE_TITLE).c_str(),
        style,
        x,
        y,
        width,
        height,
        owner,
        nullptr,
        state->hInstance,
        &promptState);
    if (!hDlg) {
        LOG_WARN(L"Create update prompt window failed (err=%lu)", GetLastError());
        return UpdatePromptChoice::Cancel;
    }
    LOG_INFO(L"Update prompt window created: hwnd=0x%p", hDlg);

    BOOL ownerWasEnabled = FALSE;
    if (owner) {
        ownerWasEnabled = IsWindowEnabled(owner);
        EnableWindow(owner, FALSE);
        LOG_INFO(L"Update prompt disabled owner window: owner=0x%p wasEnabled=%d",
            owner, ownerWasEnabled);
    }

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    LOG_INFO(L"Update prompt modal loop started");

    MSG msg{};
    while (!promptState.done && IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner && ownerWasEnabled) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        LOG_INFO(L"Update prompt restored owner window: owner=0x%p", owner);
    }

    LOG_INFO(L"Update prompt returning choice=%s", UpdatePromptChoiceName(promptState.choice));
    return promptState.choice;
}
