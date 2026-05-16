#pragma once

#include "main_window.h"

#include "../update_checker.h"

enum class UpdatePromptChoice {
    UpdateNow,
    OpenRelease,
    Cancel
};

UpdatePromptChoice ShowUpdatePromptWindow(HWND owner, MainWindowState* state,
    const EveryZip::ReleaseInfo& release);
