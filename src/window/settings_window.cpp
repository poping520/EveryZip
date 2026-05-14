#include "settings_window.h"

#include <algorithm>

#include "../logger.h"
#include "../resource.h"
#include "window_utils.h"

namespace {

constexpr wchar_t kSettingsClass[] = L"EveryZipSettingsWindow";
constexpr int IDC_SETTINGS_SHOW_FULL_PATH = 3001;
constexpr int IDC_SETTINGS_REMEMBER_UI_STATE = 3002;
constexpr int IDC_SETTINGS_DEFAULT_ZIP = 3010;
constexpr int IDC_SETTINGS_DEFAULT_RAR = 3011;
constexpr int IDC_SETTINGS_DEFAULT_7Z = 3012;
constexpr int IDC_SETTINGS_KNOWN_APK = 3020;
constexpr int IDC_SETTINGS_KNOWN_IPA = 3021;
constexpr int IDC_SETTINGS_KNOWN_JAR = 3022;
constexpr int IDC_SETTINGS_KNOWN_WAR = 3023;
constexpr int IDC_SETTINGS_CUSTOM_LIST = 3030;
constexpr int IDC_SETTINGS_CUSTOM_EXT = 3031;
constexpr int IDC_SETTINGS_CUSTOM_PARSER = 3032;
constexpr int IDC_SETTINGS_CUSTOM_ADD = 3033;
constexpr int IDC_SETTINGS_CUSTOM_DELETE = 3034;
constexpr int IDC_SETTINGS_LANGUAGE = 3040;

struct SettingsWindowState {
    HWND owner = nullptr;
    MainWindowState* mainState = nullptr;
    const SettingsWindowCallbacks* callbacks = nullptr;
    bool restartIndexerOnApply = true;
    HWND hCheckFullPath = nullptr;
    HWND hCheckRememberUiState = nullptr;
    HWND hCustomList = nullptr;
    HWND hCustomExt = nullptr;
    HWND hCustomParser = nullptr;
    HWND hLanguage = nullptr;
    std::vector<UserConfig::ArchiveFormatRule> rules;
    HFONT hFont = nullptr;
};

bool SaveUiStateThroughCallback(HWND hWnd, SettingsWindowState* sws) {
    return sws && sws->callbacks && sws->callbacks->saveUiState
        ? sws->callbacks->saveUiState(hWnd, sws->mainState)
        : true;
}

void RefreshListThroughCallback(SettingsWindowState* sws) {
    if (sws && sws->callbacks && sws->callbacks->refreshList) {
        sws->callbacks->refreshList(sws->mainState);
    }
}

void LoadRowsThroughCallback(HWND hWnd, SettingsWindowState* sws) {
    if (sws && sws->callbacks && sws->callbacks->loadRowsFromDbAndRefreshAsync) {
        sws->callbacks->loadRowsFromDbAndRefreshAsync(hWnd, sws->mainState);
    }
}

void UpdateStatusThroughCallback(SettingsWindowState* sws) {
    if (sws && sws->callbacks && sws->callbacks->updateStatusBar) {
        sws->callbacks->updateStatusBar(sws->mainState);
    }
}

void RefreshLocalizedMainWindowThroughCallback(HWND hWnd, SettingsWindowState* sws) {
    if (sws && sws->callbacks && sws->callbacks->refreshLocalizedMainWindow) {
        sws->callbacks->refreshLocalizedMainWindow(hWnd, sws->mainState);
    }
}

UserConfig::ArchiveFormatRule* FindSettingsRule(SettingsWindowState* sws, const std::wstring& ext) {
    if (!sws) return nullptr;
    const std::wstring normalized = UserConfig::NormalizeArchiveExtension(ext);
    for (auto& rule : sws->rules) {
        if (rule.extension == normalized) return &rule;
    }
    return nullptr;
}

bool HasSettingsRule(SettingsWindowState* sws, const std::wstring& ext) {
    return FindSettingsRule(sws, ext) != nullptr;
}

void SetRuleCheckbox(HWND hWnd, SettingsWindowState* sws, int controlId, const std::wstring& ext) {
    HWND hCheck = GetDlgItem(hWnd, controlId);
    const auto* rule = FindSettingsRule(sws, ext);
    if (hCheck && rule) {
        SendMessageW(hCheck, BM_SETCHECK, rule->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void UpdateRuleFromCheckbox(HWND hWnd, SettingsWindowState* sws, int controlId, const std::wstring& ext) {
    HWND hCheck = GetDlgItem(hWnd, controlId);
    auto* rule = FindSettingsRule(sws, ext);
    if (hCheck && rule) {
        rule->enabled = SendMessageW(hCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
}

void RefreshCustomFormatList(SettingsWindowState* sws) {
    if (!sws || !sws->hCustomList) return;
    ListView_DeleteAllItems(sws->hCustomList);
    int row = 0;
    for (const auto& rule : sws->rules) {
        if (rule.group != L"custom") continue;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        std::wstring enabledText = rule.enabled ? L"Y" : L"";
        item.pszText = const_cast<LPWSTR>(enabledText.c_str());
        item.lParam = row;
        const int inserted = ListView_InsertItem(sws->hCustomList, &item);
        if (inserted >= 0) {
            ListView_SetCheckState(sws->hCustomList, inserted, rule.enabled ? TRUE : FALSE);
            ListView_SetItemText(sws->hCustomList, inserted, 1, const_cast<LPWSTR>(rule.extension.c_str()));
            ListView_SetItemText(sws->hCustomList, inserted, 2, const_cast<LPWSTR>(rule.parser.c_str()));
        }
        ++row;
    }
}

void SyncCustomChecksFromList(SettingsWindowState* sws) {
    if (!sws || !sws->hCustomList) return;
    const int count = ListView_GetItemCount(sws->hCustomList);
    for (int i = 0; i < count; ++i) {
        wchar_t extBuf[64]{};
        ListView_GetItemText(sws->hCustomList, i, 1, extBuf, _countof(extBuf));
        auto* rule = FindSettingsRule(sws, extBuf);
        if (rule && rule->group == L"custom") {
            rule->enabled = ListView_GetCheckState(sws->hCustomList, i) != FALSE;
        }
    }
}

void SetupCustomFormatListColumns(HWND hList, MainWindowState* s) {
    if (!hList || !s) return;
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
    static const UINT ids[] = { IDS_SETTINGS_COL_ENABLED, IDS_SETTINGS_COL_EXTENSION, IDS_SETTINGS_COL_PARSER };
    static const int widths[] = { 70, 120, 110 };
    for (int i = 0; i < 3; ++i) {
        std::wstring text = LoadStateString(s, ids[i]);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(text.c_str());
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(hList, i, &col);
    }
}

void AddCustomFormat(HWND hWnd, SettingsWindowState* sws) {
    if (!sws || !sws->mainState || !sws->hCustomExt || !sws->hCustomParser) return;
    wchar_t extBuf[64]{};
    GetWindowTextW(sws->hCustomExt, extBuf, _countof(extBuf));
    const std::wstring ext = UserConfig::NormalizeArchiveExtension(extBuf);
    if (!UserConfig::IsValidCustomArchiveExtension(ext)) {
        MessageBoxW(hWnd, LoadStateString(sws->mainState, IDS_SETTINGS_INVALID_EXTENSION).c_str(),
            LoadStateString(sws->mainState, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    if (HasSettingsRule(sws, ext)) {
        MessageBoxW(hWnd, LoadStateString(sws->mainState, IDS_SETTINGS_DUPLICATE_EXTENSION).c_str(),
            LoadStateString(sws->mainState, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t parserBuf[16]{};
    GetWindowTextW(sws->hCustomParser, parserBuf, _countof(parserBuf));
    std::wstring parser = parserBuf;
    if (parser.empty()) parser = L"zip";
    sws->rules.push_back({ ext, parser, true, L"custom" });
    SetWindowTextW(sws->hCustomExt, L"");
    RefreshCustomFormatList(sws);
}

void DeleteSelectedCustomFormat(SettingsWindowState* sws) {
    if (!sws || !sws->hCustomList) return;
    const int selected = ListView_GetNextItem(sws->hCustomList, -1, LVNI_SELECTED);
    if (selected < 0) return;
    wchar_t extBuf[64]{};
    ListView_GetItemText(sws->hCustomList, selected, 1, extBuf, _countof(extBuf));
    const std::wstring ext = UserConfig::NormalizeArchiveExtension(extBuf);
    sws->rules.erase(std::remove_if(sws->rules.begin(), sws->rules.end(),
        [&](const UserConfig::ArchiveFormatRule& rule) {
            return rule.group == L"custom" && rule.extension == ext;
        }), sws->rules.end());
    RefreshCustomFormatList(sws);
}

void ApplyShowArchiveFullPathSetting(HWND hWnd, SettingsWindowState* sws) {
    if (!sws || !sws->mainState || !sws->hCheckFullPath) return;
    MainWindowState* s = sws->mainState;
    const bool checked = SendMessageW(sws->hCheckFullPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool previous = s->showArchiveFullPath;
    if (checked == previous) return;
    s->showArchiveFullPath = checked;
    s->userConfig.SetShowArchiveFullPath(checked);
    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        s->showArchiveFullPath = previous;
        s->userConfig.SetShowArchiveFullPath(previous);
        SendMessageW(sws->hCheckFullPath, BM_SETCHECK, previous ? BST_CHECKED : BST_UNCHECKED, 0);
        std::wstring msg = LoadStateString(s, IDS_SETTINGS_SAVE_FAILED);
        if (!err.empty()) msg += L"\n" + err;
        MessageBoxW(hWnd, msg.c_str(), LoadStateString(s, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    s->rowCache.Clear();
    RefreshListThroughCallback(sws);
}

void ApplyRememberUiStateSetting(HWND hWnd, SettingsWindowState* sws) {
    if (!sws || !sws->mainState || !sws->hCheckRememberUiState) return;
    MainWindowState* s = sws->mainState;
    const bool checked = SendMessageW(sws->hCheckRememberUiState, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool previous = s->userConfig.GetRememberUiState();
    if (checked == previous) return;
    s->userConfig.SetRememberUiState(checked);
    bool saved = false;
    std::wstring err;
    if (checked) saved = SaveUiStateThroughCallback(sws->owner, sws);
    else saved = s->userConfig.Save(&err);
    if (!saved) {
        s->userConfig.SetRememberUiState(previous);
        SendMessageW(sws->hCheckRememberUiState, BM_SETCHECK, previous ? BST_CHECKED : BST_UNCHECKED, 0);
        std::wstring msg = LoadStateString(s, IDS_SETTINGS_SAVE_FAILED);
        if (!err.empty()) msg += L"\n" + err;
        MessageBoxW(hWnd, msg.c_str(), LoadStateString(s, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
    }
}

bool ApplyFormatSettings(HWND hWnd, SettingsWindowState* sws) {
    if (!sws || !sws->mainState) return false;
    MainWindowState* s = sws->mainState;
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_ZIP, L".zip");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_RAR, L".rar");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_7Z, L".7z");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_APK, L".apk");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_IPA, L".ipa");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_JAR, L".jar");
    UpdateRuleFromCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_WAR, L".war");
    SyncCustomChecksFromList(sws);
    const auto previousRules = s->userConfig.GetArchiveFormatRules();
    const bool previousFullPath = s->showArchiveFullPath;
    const bool previousRemember = s->userConfig.GetRememberUiState();
    const UserConfig::LanguageMode previousLanguage = s->userConfig.GetLanguageMode();
    const bool fullPath = sws->hCheckFullPath &&
        SendMessageW(sws->hCheckFullPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool remember = sws->hCheckRememberUiState &&
        SendMessageW(sws->hCheckRememberUiState, BM_GETCHECK, 0, 0) == BST_CHECKED;
    UserConfig::LanguageMode language = previousLanguage;
    if (sws->hLanguage) {
        const LRESULT selected = SendMessageW(sws->hLanguage, CB_GETCURSEL, 0, 0);
        if (selected == 1) {
            language = UserConfig::LanguageMode::ZhCN;
        } else if (selected == 2) {
            language = UserConfig::LanguageMode::EnUS;
        } else {
            language = UserConfig::LanguageMode::System;
        }
    }
    s->userConfig.SetArchiveFormatRules(sws->rules);
    s->showArchiveFullPath = fullPath;
    s->userConfig.SetShowArchiveFullPath(fullPath);
    s->userConfig.SetRememberUiState(remember);
    s->userConfig.SetLanguageMode(language);
    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        s->userConfig.SetArchiveFormatRules(previousRules);
        s->showArchiveFullPath = previousFullPath;
        s->userConfig.SetShowArchiveFullPath(previousFullPath);
        s->userConfig.SetRememberUiState(previousRemember);
        s->userConfig.SetLanguageMode(previousLanguage);
        std::wstring msg = LoadStateString(s, IDS_SETTINGS_SAVE_FAILED);
        if (!err.empty()) msg += L"\n" + err;
        MessageBoxW(hWnd, msg.c_str(), LoadStateString(s, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return false;
    }
    if (language != previousLanguage) {
        RefreshLocalizedMainWindowThroughCallback(sws->owner, sws);
    }
    if (sws->restartIndexerOnApply) {
        s->parseDoneCount.store(0);
        s->parseTotalCount.store(0);
        s->indexer.Stop();
        s->indexer.SetArchiveFormatRules(s->userConfig.GetArchiveFormatRules());
        s->rowCache.Clear();
        LoadRowsThroughCallback(sws->owner, sws);
        s->indexer.Start(sws->owner);
        UpdateStatusThroughCallback(sws);
    }
    return true;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* sws = (SettingsWindowState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        sws = (SettingsWindowState*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)sws);
        MainWindowState* s = sws ? sws->mainState : nullptr;
        if (sws && s) sws->rules = s->userConfig.GetArchiveFormatRules();
        const UINT dpi = GetWindowDpiValue(hWnd);
        const int margin = ScaleDpiValue(16, dpi);
        const int checkH = ScaleDpiValue(24, dpi);
        const int checkGap = ScaleDpiValue(8, dpi);
        const int groupH = ScaleDpiValue(86, dpi);
        const int groupW = ScaleDpiValue(310, dpi);
        const int rightX = margin + groupW + ScaleDpiValue(14, dpi);
        const int rightW = ScaleDpiValue(360, dpi);
        HWND hDefaultGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_DEFAULT_FORMATS).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, margin, groupW, groupH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        HWND hKnownGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_KNOWN_ALIASES).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, margin + groupH + ScaleDpiValue(10, dpi), groupW, ScaleDpiValue(112, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        HWND hLanguageGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_LANGUAGE_TITLE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, margin + groupH + ScaleDpiValue(132, dpi), groupW, ScaleDpiValue(70, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        HWND hCustomGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_FORMATS).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            rightX, margin, rightW, ScaleDpiValue(266, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        auto createCheck = [&](int id, const wchar_t* text, int x, int y, int w) -> HWND {
            return CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, y, w, checkH,
                hWnd, (HMENU)(INT_PTR)id, s ? s->hInstance : nullptr, nullptr);
        };
        const int groupPadX = ScaleDpiValue(14, dpi);
        const int groupPadY = ScaleDpiValue(24, dpi);
        const int defaultY = margin + groupPadY;
        createCheck(IDC_SETTINGS_DEFAULT_ZIP, L".zip", margin + groupPadX, defaultY, ScaleDpiValue(80, dpi));
        createCheck(IDC_SETTINGS_DEFAULT_RAR, L".rar", margin + groupPadX + ScaleDpiValue(86, dpi), defaultY, ScaleDpiValue(80, dpi));
        createCheck(IDC_SETTINGS_DEFAULT_7Z, L".7z", margin + groupPadX + ScaleDpiValue(172, dpi), defaultY, ScaleDpiValue(80, dpi));
        const int knownBaseY = margin + groupH + ScaleDpiValue(10, dpi) + groupPadY;
        createCheck(IDC_SETTINGS_KNOWN_APK, L".apk", margin + groupPadX, knownBaseY, ScaleDpiValue(80, dpi));
        createCheck(IDC_SETTINGS_KNOWN_IPA, L".ipa", margin + groupPadX + ScaleDpiValue(86, dpi), knownBaseY, ScaleDpiValue(80, dpi));
        createCheck(IDC_SETTINGS_KNOWN_JAR, L".jar", margin + groupPadX, knownBaseY + checkH + checkGap, ScaleDpiValue(80, dpi));
        createCheck(IDC_SETTINGS_KNOWN_WAR, L".war", margin + groupPadX + ScaleDpiValue(86, dpi), knownBaseY + checkH + checkGap, ScaleDpiValue(80, dpi));
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_ZIP, L".zip");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_RAR, L".rar");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_7Z, L".7z");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_APK, L".apk");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_IPA, L".ipa");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_JAR, L".jar");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_WAR, L".war");
        sws->hLanguage = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            margin + groupPadX, margin + groupH + ScaleDpiValue(132, dpi) + groupPadY,
            ScaleDpiValue(180, dpi), ScaleDpiValue(120, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_LANGUAGE, s ? s->hInstance : nullptr, nullptr);
        if (sws->hLanguage && s) {
            SendMessageW(sws->hLanguage, CB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_LANGUAGE_SYSTEM).c_str());
            SendMessageW(sws->hLanguage, CB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_LANGUAGE_ZH_CN).c_str());
            SendMessageW(sws->hLanguage, CB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_LANGUAGE_EN_US).c_str());
            int languageIndex = 0;
            if (s->userConfig.GetLanguageMode() == UserConfig::LanguageMode::ZhCN) languageIndex = 1;
            else if (s->userConfig.GetLanguageMode() == UserConfig::LanguageMode::EnUS) languageIndex = 2;
            SendMessageW(sws->hLanguage, CB_SETCURSEL, languageIndex, 0);
        }
        sws->hCheckFullPath = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_SHOW_FULL_ARCHIVE_PATH).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            margin, margin + groupH + ScaleDpiValue(212, dpi), ScaleDpiValue(260, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_SHOW_FULL_PATH, s ? s->hInstance : nullptr, nullptr);
        if (sws->hCheckFullPath && s) {
            SendMessageW(sws->hCheckFullPath, BM_SETCHECK, s->showArchiveFullPath ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        sws->hCheckRememberUiState = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_REMEMBER_UI_STATE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            margin, margin + groupH + ScaleDpiValue(212, dpi) + checkH + checkGap, ScaleDpiValue(320, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_REMEMBER_UI_STATE, s ? s->hInstance : nullptr, nullptr);
        if (sws->hCheckRememberUiState && s) {
            SendMessageW(sws->hCheckRememberUiState, BM_SETCHECK,
                s->userConfig.GetRememberUiState() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        sws->hCustomList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            rightX + groupPadX, margin + groupPadY, rightW - groupPadX * 2, ScaleDpiValue(138, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_LIST, s ? s->hInstance : nullptr, nullptr);
        SetupCustomFormatListColumns(sws->hCustomList, s);
        RefreshCustomFormatList(sws);
        HWND hExtLabel = CreateWindowExW(0, L"STATIC",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_EXTENSION).c_str() : L"",
            WS_CHILD | WS_VISIBLE,
            rightX + groupPadX, margin + ScaleDpiValue(170, dpi), ScaleDpiValue(56, dpi), checkH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        sws->hCustomExt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            rightX + groupPadX + ScaleDpiValue(58, dpi), margin + ScaleDpiValue(168, dpi), ScaleDpiValue(86, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_EXT, s ? s->hInstance : nullptr, nullptr);
        HWND hParserLabel = CreateWindowExW(0, L"STATIC",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_PARSER).c_str() : L"",
            WS_CHILD | WS_VISIBLE,
            rightX + groupPadX + ScaleDpiValue(154, dpi), margin + ScaleDpiValue(170, dpi), ScaleDpiValue(56, dpi), checkH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        sws->hCustomParser = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            rightX + groupPadX + ScaleDpiValue(210, dpi), margin + ScaleDpiValue(168, dpi), ScaleDpiValue(78, dpi), ScaleDpiValue(120, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_PARSER, s ? s->hInstance : nullptr, nullptr);
        if (sws->hCustomParser) {
            SendMessageW(sws->hCustomParser, CB_ADDSTRING, 0, (LPARAM)L"zip");
            SendMessageW(sws->hCustomParser, CB_ADDSTRING, 0, (LPARAM)L"rar");
            SendMessageW(sws->hCustomParser, CB_ADDSTRING, 0, (LPARAM)L"7z");
            SendMessageW(sws->hCustomParser, CB_SETCURSEL, 0, 0);
        }
        HWND hAdd = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_ADD).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            rightX + groupPadX, margin + ScaleDpiValue(210, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_ADD, s ? s->hInstance : nullptr, nullptr);
        HWND hDelete = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_DELETE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            rightX + groupPadX + ScaleDpiValue(92, dpi), margin + ScaleDpiValue(210, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_DELETE, s ? s->hInstance : nullptr, nullptr);
        HWND hOk = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_OK).c_str() : L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            rightX + rightW - ScaleDpiValue(180, dpi), margin + ScaleDpiValue(344, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDOK, s ? s->hInstance : nullptr, nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_CANCEL).c_str() : L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            rightX + rightW - ScaleDpiValue(90, dpi), margin + ScaleDpiValue(344, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDCANCEL, s ? s->hInstance : nullptr, nullptr);
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            sws->hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (sws->hFont) {
                HWND controls[] = {
                    hDefaultGroup, hKnownGroup, hLanguageGroup, hCustomGroup,
                    GetDlgItem(hWnd, IDC_SETTINGS_DEFAULT_ZIP),
                    GetDlgItem(hWnd, IDC_SETTINGS_DEFAULT_RAR),
                    GetDlgItem(hWnd, IDC_SETTINGS_DEFAULT_7Z),
                    GetDlgItem(hWnd, IDC_SETTINGS_KNOWN_APK),
                    GetDlgItem(hWnd, IDC_SETTINGS_KNOWN_IPA),
                    GetDlgItem(hWnd, IDC_SETTINGS_KNOWN_JAR),
                    GetDlgItem(hWnd, IDC_SETTINGS_KNOWN_WAR),
                    sws->hLanguage, sws->hCheckFullPath, sws->hCheckRememberUiState,
                    sws->hCustomList, hExtLabel, sws->hCustomExt,
                    hParserLabel, sws->hCustomParser, hAdd, hDelete, hOk, hCancel
                };
                for (HWND hCtrl : controls) {
                    if (hCtrl) SendMessageW(hCtrl, WM_SETFONT, (WPARAM)sws->hFont, TRUE);
                }
            }
        }
        return 0;
    }
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SETTINGS_CUSTOM_ADD && HIWORD(wParam) == BN_CLICKED) {
            AddCustomFormat(hWnd, sws);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CUSTOM_DELETE && HIWORD(wParam) == BN_CLICKED) {
            DeleteSelectedCustomFormat(sws);
            return 0;
        }
        if (LOWORD(wParam) == IDOK && HIWORD(wParam) == BN_CLICKED) {
            if (ApplyFormatSettings(hWnd, sws)) DestroyWindow(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && HIWORD(wParam) == BN_CLICKED) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (sws && sws->hFont) {
            DeleteObject(sws->hFont);
            sws->hFont = nullptr;
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterSettingsClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);
    registered = true;
}

}

void ResetLayoutColumns(HWND hWnd, MainWindowState* s, const SettingsWindowCallbacks& callbacks) {
    if (!s) return;
    std::vector<int> previousWidths;
    if (s->hList) {
        previousWidths.reserve(6);
        for (int i = 0; i < 6; ++i) previousWidths.push_back(ListView_GetColumnWidth(s->hList, i));
    }
    const auto& defaultWidths = UserConfig::GetDefaultListColumnWidths();
    if (s->hList) {
        for (int i = 0; i < 6 && i < (int)defaultWidths.size(); ++i) {
            ListView_SetColumnWidth(s->hList, i, defaultWidths[i]);
        }
    }
    s->userConfig.ResetListColumnWidths();
    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        if (s->hList && previousWidths.size() == 6) {
            for (int i = 0; i < 6; ++i) ListView_SetColumnWidth(s->hList, i, previousWidths[i]);
            s->userConfig.SetListColumnWidths(previousWidths);
        }
        std::wstring msg = LoadStateString(s, IDS_SETTINGS_SAVE_FAILED);
        if (!err.empty()) msg += L"\n" + err;
        MessageBoxW(hWnd, msg.c_str(), LoadStateString(s, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    if (callbacks.refreshList) callbacks.refreshList(s);
}

void ShowSettingsPanel(HWND hOwner,
                       MainWindowState* s,
                       const SettingsWindowCallbacks& callbacks,
                       bool restartIndexerOnApply) {
    if (!s) return;
    RegisterSettingsClass(s->hInstance);
    const UINT dpi = GetWindowDpiValue(hOwner);
    const int width = ScaleDpiValue(730, dpi);
    const int height = ScaleDpiValue(430, dpi);
    RECT ownerRc{};
    GetWindowRect(hOwner, &ownerRc);
    int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
    int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;
    SettingsWindowState sws{};
    sws.owner = hOwner;
    sws.mainState = s;
    sws.callbacks = &callbacks;
    sws.restartIndexerOnApply = restartIndexerOnApply;
    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        kSettingsClass,
        LoadStateString(s, IDS_SETTINGS_TITLE).c_str(),
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, width, height,
        hOwner,
        nullptr,
        s->hInstance,
        &sws);
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
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hOwner, TRUE);
    SetForegroundWindow(hOwner);
}
