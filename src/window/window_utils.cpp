#include "window_utils.h"

bool HandlePlainDialogCtlColor(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    if (!result) return false;
    const bool isPlainText = msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN;
    const bool isReadonlyEdit = msg == WM_CTLCOLOREDIT &&
        (((LONG_PTR)GetWindowLongPtrW((HWND)lParam, GWL_STYLE) & ES_READONLY) != 0);
    if (!isPlainText && !isReadonlyEdit) return false;

    HDC hdc = (HDC)wParam;
    SetBkMode(hdc, TRANSPARENT);
    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
    *result = (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    return true;
}
