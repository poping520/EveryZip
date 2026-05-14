#pragma once

#include "main_window.h"

struct SettingsWindowCallbacks {
    bool (*saveUiState)(HWND hWnd, MainWindowState* s) = nullptr;
    void (*refreshList)(MainWindowState* s) = nullptr;
    void (*loadRowsFromDbAndRefreshAsync)(HWND hWnd, MainWindowState* s) = nullptr;
    void (*updateStatusBar)(MainWindowState* s) = nullptr;
};

void ShowSettingsPanel(HWND hOwner,
                       MainWindowState* s,
                       const SettingsWindowCallbacks& callbacks,
                       bool restartIndexerOnApply = true);

void ResetLayoutColumns(HWND hWnd,
                        MainWindowState* s,
                        const SettingsWindowCallbacks& callbacks);
