#include "settings_window.h"

#include <algorithm>

#include "../logger.h"
#include "../resource.h"
#include "window_utils.h"

namespace {

constexpr wchar_t kSettingsClass[] = L"EveryZipSettingsWindow";
constexpr int IDC_SETTINGS_SHOW_FULL_PATH = 3001;
constexpr int IDC_SETTINGS_REMEMBER_UI_STATE = 3002;
constexpr int IDC_SETTINGS_AUTO_UPDATE_CHECK = 3003;
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
constexpr int IDC_SETTINGS_NAV = 3050;
constexpr int IDC_SETTINGS_RESET_COLUMNS = 3051;

enum class SettingsPage {
    Formats = 0,
    Ui = 1,
    Language = 2
};

struct SettingsWindowState {
    HWND owner = nullptr;
    MainWindowState* mainState = nullptr;
    const SettingsWindowCallbacks* callbacks = nullptr;
    bool restartIndexerOnApply = true;
    HWND hCheckFullPath = nullptr;
    HWND hCheckRememberUiState = nullptr;
    HWND hCheckAutoUpdate = nullptr;
    HWND hCustomList = nullptr;
    HWND hCustomExt = nullptr;
    HWND hCustomParser = nullptr;
    HWND hLanguage = nullptr;
    HWND hResetColumns = nullptr;
    HWND hNav = nullptr;
    SettingsPage currentPage = SettingsPage::Formats;
    std::vector<HWND> formatControls;
    std::vector<HWND> uiControls;
    std::vector<HWND> languageControls;
    std::vector<HWND> allControls;
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

void ShowSettingsPage(SettingsWindowState* sws, SettingsPage page) {
    if (!sws) return;
    sws->currentPage = page;

    auto setVisible = [&](const std::vector<HWND>& controls, bool visible) {
        for (HWND hCtrl : controls) {
            if (hCtrl) ShowWindow(hCtrl, visible ? SW_SHOW : SW_HIDE);
        }
    };

    setVisible(sws->formatControls, page == SettingsPage::Formats);
    setVisible(sws->uiControls, page == SettingsPage::Ui);
    setVisible(sws->languageControls, page == SettingsPage::Language);
}

void AddPageControl(std::vector<HWND>* pageControls, SettingsWindowState* sws, HWND hCtrl) {
    if (!hCtrl) return;
    if (pageControls) pageControls->push_back(hCtrl);
    if (sws) sws->allControls.push_back(hCtrl);
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

void ResetLayoutColumnsFromSettings(HWND hWnd, SettingsWindowState* sws) {
    if (!sws || !sws->mainState) return;
    MainWindowState* s = sws->mainState;

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

    RefreshListThroughCallback(sws);
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
    const bool previousAutoUpdate = s->userConfig.GetAutoUpdateCheckEnabled();
    const UserConfig::LanguageMode previousLanguage = s->userConfig.GetLanguageMode();
    const bool fullPath = sws->hCheckFullPath &&
        SendMessageW(sws->hCheckFullPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool remember = sws->hCheckRememberUiState &&
        SendMessageW(sws->hCheckRememberUiState, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool autoUpdate = sws->hCheckAutoUpdate &&
        SendMessageW(sws->hCheckAutoUpdate, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
    s->userConfig.SetAutoUpdateCheckEnabled(autoUpdate);
    s->userConfig.SetLanguageMode(language);
    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        s->userConfig.SetArchiveFormatRules(previousRules);
        s->showArchiveFullPath = previousFullPath;
        s->userConfig.SetShowArchiveFullPath(previousFullPath);
        s->userConfig.SetRememberUiState(previousRemember);
        s->userConfig.SetAutoUpdateCheckEnabled(previousAutoUpdate);
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
        s->indexer.SetParseThreadCount(s->userConfig.GetParseThreadCount());
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
        const int navW = ScaleDpiValue(132, dpi);
        const int contentX = margin + navW + ScaleDpiValue(18, dpi);
        const int contentW = ScaleDpiValue(560, dpi);
        const int checkH = ScaleDpiValue(24, dpi);
        const int checkGap = ScaleDpiValue(8, dpi);
        const int groupH = ScaleDpiValue(86, dpi);
        const int groupW = ScaleDpiValue(232, dpi);
        const int rightX = contentX + groupW + ScaleDpiValue(14, dpi);
        const int rightW = contentW - groupW - ScaleDpiValue(14, dpi);

        sws->hNav = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_HASSTRINGS,
            margin, margin, navW, ScaleDpiValue(330, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_NAV, s ? s->hInstance : nullptr, nullptr);
        if (sws->hNav && s) {
            SendMessageW(sws->hNav, LB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_FORMATS_TITLE).c_str());
            SendMessageW(sws->hNav, LB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_UI_TITLE).c_str());
            SendMessageW(sws->hNav, LB_ADDSTRING, 0, (LPARAM)LoadStateString(s, IDS_SETTINGS_LANGUAGE_TITLE).c_str());
            SendMessageW(sws->hNav, LB_SETCURSEL, 0, 0);
            sws->allControls.push_back(sws->hNav);
        }

        HWND hDefaultGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_DEFAULT_FORMATS).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            contentX, margin, groupW, groupH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hDefaultGroup);
        HWND hKnownGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_KNOWN_ALIASES).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            contentX, margin + groupH + ScaleDpiValue(10, dpi), groupW, ScaleDpiValue(112, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hKnownGroup);
        HWND hCustomGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_FORMATS).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            rightX, margin, rightW, ScaleDpiValue(266, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hCustomGroup);
        HWND hUiGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_UI_TITLE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            contentX, margin, contentW, ScaleDpiValue(148, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->uiControls, sws, hUiGroup);
        HWND hLanguageGroup = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_LANGUAGE_TITLE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            contentX, margin, contentW, ScaleDpiValue(86, dpi),
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->languageControls, sws, hLanguageGroup);
        auto createCheck = [&](int id, const wchar_t* text, int x, int y, int w) -> HWND {
            return CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, y, w, checkH,
                hWnd, (HMENU)(INT_PTR)id, s ? s->hInstance : nullptr, nullptr);
        };
        const int groupPadX = ScaleDpiValue(14, dpi);
        const int groupPadY = ScaleDpiValue(24, dpi);
        const int defaultY = margin + groupPadY;
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_DEFAULT_ZIP, L".zip", contentX + groupPadX, defaultY, ScaleDpiValue(72, dpi)));
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_DEFAULT_RAR, L".rar", contentX + groupPadX + ScaleDpiValue(72, dpi), defaultY, ScaleDpiValue(72, dpi)));
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_DEFAULT_7Z, L".7z", contentX + groupPadX + ScaleDpiValue(144, dpi), defaultY, ScaleDpiValue(72, dpi)));
        const int knownBaseY = margin + groupH + ScaleDpiValue(10, dpi) + groupPadY;
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_KNOWN_APK, L".apk", contentX + groupPadX, knownBaseY, ScaleDpiValue(80, dpi)));
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_KNOWN_IPA, L".ipa", contentX + groupPadX + ScaleDpiValue(86, dpi), knownBaseY, ScaleDpiValue(80, dpi)));
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_KNOWN_JAR, L".jar", contentX + groupPadX, knownBaseY + checkH + checkGap, ScaleDpiValue(80, dpi)));
        AddPageControl(&sws->formatControls, sws,
            createCheck(IDC_SETTINGS_KNOWN_WAR, L".war", contentX + groupPadX + ScaleDpiValue(86, dpi), knownBaseY + checkH + checkGap, ScaleDpiValue(80, dpi)));
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_ZIP, L".zip");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_RAR, L".rar");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_DEFAULT_7Z, L".7z");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_APK, L".apk");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_IPA, L".ipa");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_JAR, L".jar");
        SetRuleCheckbox(hWnd, sws, IDC_SETTINGS_KNOWN_WAR, L".war");
        sws->hLanguage = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            contentX + groupPadX, margin + groupPadY,
            ScaleDpiValue(180, dpi), ScaleDpiValue(120, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_LANGUAGE, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->languageControls, sws, sws->hLanguage);
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
            contentX + groupPadX, margin + groupPadY, ScaleDpiValue(320, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_SHOW_FULL_PATH, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->uiControls, sws, sws->hCheckFullPath);
        if (sws->hCheckFullPath && s) {
            SendMessageW(sws->hCheckFullPath, BM_SETCHECK, s->showArchiveFullPath ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        sws->hCheckRememberUiState = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_REMEMBER_UI_STATE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            contentX + groupPadX, margin + groupPadY + checkH + checkGap, ScaleDpiValue(360, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_REMEMBER_UI_STATE, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->uiControls, sws, sws->hCheckRememberUiState);
        if (sws->hCheckRememberUiState && s) {
            SendMessageW(sws->hCheckRememberUiState, BM_SETCHECK,
                s->userConfig.GetRememberUiState() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        sws->hCheckAutoUpdate = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_AUTO_UPDATE_CHECK).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            contentX + groupPadX, margin + groupPadY + (checkH + checkGap) * 2,
            ScaleDpiValue(360, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_AUTO_UPDATE_CHECK, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->uiControls, sws, sws->hCheckAutoUpdate);
        if (sws->hCheckAutoUpdate && s) {
            SendMessageW(sws->hCheckAutoUpdate, BM_SETCHECK,
                s->userConfig.GetAutoUpdateCheckEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        sws->hResetColumns = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_RESET_LAYOUT).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            contentX + groupPadX, margin + groupPadY + (checkH + checkGap) * 3 + ScaleDpiValue(8, dpi),
            ScaleDpiValue(110, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_RESET_COLUMNS, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->uiControls, sws, sws->hResetColumns);
        sws->hCustomList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            rightX + groupPadX, margin + groupPadY, rightW - groupPadX * 2, ScaleDpiValue(138, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_LIST, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, sws->hCustomList);
        SetupCustomFormatListColumns(sws->hCustomList, s);
        RefreshCustomFormatList(sws);
        HWND hExtLabel = CreateWindowExW(0, L"STATIC",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_EXTENSION).c_str() : L"",
            WS_CHILD | WS_VISIBLE,
            rightX + groupPadX, margin + ScaleDpiValue(170, dpi), ScaleDpiValue(56, dpi), checkH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hExtLabel);
        sws->hCustomExt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            rightX + groupPadX + ScaleDpiValue(58, dpi), margin + ScaleDpiValue(168, dpi), ScaleDpiValue(86, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_EXT, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, sws->hCustomExt);
        HWND hParserLabel = CreateWindowExW(0, L"STATIC",
            s ? LoadStateString(s, IDS_SETTINGS_CUSTOM_PARSER).c_str() : L"",
            WS_CHILD | WS_VISIBLE,
            rightX + groupPadX + ScaleDpiValue(154, dpi), margin + ScaleDpiValue(170, dpi), ScaleDpiValue(56, dpi), checkH,
            hWnd, nullptr, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hParserLabel);
        sws->hCustomParser = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            rightX + groupPadX + ScaleDpiValue(210, dpi), margin + ScaleDpiValue(168, dpi), ScaleDpiValue(78, dpi), ScaleDpiValue(120, dpi),
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_PARSER, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, sws->hCustomParser);
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
        AddPageControl(&sws->formatControls, sws, hAdd);
        HWND hDelete = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_DELETE).c_str() : L"",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            rightX + groupPadX + ScaleDpiValue(92, dpi), margin + ScaleDpiValue(210, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDC_SETTINGS_CUSTOM_DELETE, s ? s->hInstance : nullptr, nullptr);
        AddPageControl(&sws->formatControls, sws, hDelete);
        HWND hOk = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_OK).c_str() : L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            contentX + contentW - ScaleDpiValue(180, dpi), margin + ScaleDpiValue(344, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDOK, s ? s->hInstance : nullptr, nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON",
            s ? LoadStateString(s, IDS_SETTINGS_CANCEL).c_str() : L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            contentX + contentW - ScaleDpiValue(90, dpi), margin + ScaleDpiValue(344, dpi), ScaleDpiValue(82, dpi), checkH,
            hWnd, (HMENU)(INT_PTR)IDCANCEL, s ? s->hInstance : nullptr, nullptr);
        sws->allControls.push_back(hOk);
        sws->allControls.push_back(hCancel);
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            sws->hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (sws->hFont) {
                for (HWND hCtrl : sws->allControls) {
                    if (hCtrl) SendMessageW(hCtrl, WM_SETFONT, (WPARAM)sws->hFont, TRUE);
                }
            }
        }
        ShowSettingsPage(sws, SettingsPage::Formats);
        return 0;
    }
    LRESULT ctlColorResult = 0;
    if (HandlePlainDialogCtlColor(msg, wParam, lParam, &ctlColorResult)) {
        return ctlColorResult;
    }

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SETTINGS_NAV && HIWORD(wParam) == LBN_SELCHANGE) {
            const LRESULT selected = sws && sws->hNav ? SendMessageW(sws->hNav, LB_GETCURSEL, 0, 0) : 0;
            if (selected == 1) {
                ShowSettingsPage(sws, SettingsPage::Ui);
            } else if (selected == 2) {
                ShowSettingsPage(sws, SettingsPage::Language);
            } else {
                ShowSettingsPage(sws, SettingsPage::Formats);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CUSTOM_ADD && HIWORD(wParam) == BN_CLICKED) {
            AddCustomFormat(hWnd, sws);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CUSTOM_DELETE && HIWORD(wParam) == BN_CLICKED) {
            DeleteSelectedCustomFormat(sws);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_RESET_COLUMNS && HIWORD(wParam) == BN_CLICKED) {
            ResetLayoutColumnsFromSettings(hWnd, sws);
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
    const int width = ScaleDpiValue(790, dpi);
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
