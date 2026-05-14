#include "main_window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

#include <shlobj.h>

#include "../logger.h"
#include "../parser/archive_parser_factory.h"
#include "../resource.h"
#include "../string_utils.h"
#include "../tray_icon.h"
#include "about_window.h"
#include "settings_window.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

static constexpr LANGID kZhCnLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
static constexpr LANGID kEnUsLangId = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

static LANGID ResolveSystemLanguageId() {
    const LANGID systemLang = GetUserDefaultUILanguage();
    return PRIMARYLANGID(systemLang) == LANG_CHINESE ? kZhCnLangId : kEnUsLangId;
}

LANGID GetEffectiveLanguageId(const UserConfig& config) {
    switch (config.GetLanguageMode()) {
    case UserConfig::LanguageMode::ZhCN:
        return kZhCnLangId;
    case UserConfig::LanguageMode::EnUS:
        return kEnUsLangId;
    case UserConfig::LanguageMode::System:
    default:
        return ResolveSystemLanguageId();
    }
}

static std::wstring LoadStringForLanguage(HINSTANCE hInstance, UINT id, LANGID languageId) {
    const UINT blockId = (id >> 4) + 1;
    HRSRC hRes = FindResourceExW(hInstance, RT_STRING, MAKEINTRESOURCEW(blockId), languageId);
    if (!hRes) return {};

    HGLOBAL hData = LoadResource(hInstance, hRes);
    if (!hData) return {};

    const wchar_t* strings = static_cast<const wchar_t*>(LockResource(hData));
    if (!strings) return {};

    const UINT index = id & 0xF;
    for (UINT i = 0; i < index; ++i) {
        strings += 1 + static_cast<UINT>(*strings);
    }

    const int len = *strings++;
    return len > 0 ? std::wstring(strings, strings + len) : std::wstring();
}

std::wstring LS(HINSTANCE hInstance, UINT id, LANGID languageId) {
    std::wstring text = LoadStringForLanguage(hInstance, id, languageId);
    if (!text.empty()) return text;

    text = LoadStringForLanguage(hInstance, id, kEnUsLangId);
    if (!text.empty()) return text;

    text = LoadStringForLanguage(hInstance, id, kZhCnLangId);
    if (!text.empty()) return text;

    return {};
}

std::wstring LS(HINSTANCE hInstance, UINT id) {
    return LS(hInstance, id, ResolveSystemLanguageId());
}

// ── Spinner 自绘窗口类名 ──
static constexpr wchar_t kSpinnerClass[] = L"EveryZipSpinner";

// Spinner 窗口过程：自绘旋转圆弧动画
static LRESULT CALLBACK SpinnerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        // 双缓冲避免闪烁
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // 填充状态栏背景色
        HBRUSH bgBrush = GetSysColorBrush(COLOR_BTNFACE);
        FillRect(memDC, &rc, bgBrush);

        // 读取当前角度（存储在窗口用户数据）
        int angle = (int)(LONG_PTR)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

        // 超采样：在 2 倍尺寸的离屏 DC 上绘制，再缩放到实际尺寸，消除 GDI 锯齿
        const int scale = 2;
        const int sw = w * scale;
        const int sh = h * scale;
        HDC scaleDC = CreateCompatibleDC(hdc);
        HBITMAP scaleBmp = CreateCompatibleBitmap(hdc, sw, sh);
        HBITMAP oldScaleBmp = (HBITMAP)SelectObject(scaleDC, scaleBmp);

        // 填充背景
        RECT scaleRc = { 0, 0, sw, sh };
        FillRect(scaleDC, &scaleRc, bgBrush);

        // 计算圆弧区域（居中留边距），全部坐标 ×scale
        const int margin = 3 * scale;
        const int size = (min(sw, sh) - margin * 2);
        if (size > 4) {
            const int cx = sw / 2;
            const int cy = sh / 2;
            const int r  = size / 2;

            // 笔宽约为直径的 1/5，至少 3px（scaled）
            const int penW = max(3, size / 5);
            HBRUSH oldBrush = (HBRUSH)SelectObject(scaleDC, GetStockObject(NULL_BRUSH));

            // 先画灰色轨道（完整圆）
            HPEN trackPen = CreatePen(PS_SOLID, penW, RGB(220, 220, 220));
            HPEN oldPen = (HPEN)SelectObject(scaleDC, trackPen);
            Ellipse(scaleDC, cx - r, cy - r, cx + r, cy + r);
            SelectObject(scaleDC, oldPen);
            DeleteObject(trackPen);

            // 再画经典绿色旋转弧（270°）
            const double pi = 3.14159265358979323846;
            const double startRad = (angle - 90) * pi / 180.0;
            const double sweepRad = 270.0 * pi / 180.0;
            const int x1 = cx + (int)(r * cos(startRad));
            const int y1 = cy + (int)(r * sin(startRad));
            const double endRad = startRad + sweepRad;
            const int x2 = cx + (int)(r * cos(endRad));
            const int y2 = cy + (int)(r * sin(endRad));

            HPEN arcPen = CreatePen(PS_SOLID, penW, RGB(0, 164, 0));
            SelectObject(scaleDC, arcPen);
            SetArcDirection(scaleDC, AD_CLOCKWISE);
            Arc(scaleDC, cx - r, cy - r, cx + r, cy + r, x1, y1, x2, y2);
            SelectObject(scaleDC, oldPen);
            DeleteObject(arcPen);

            SelectObject(scaleDC, oldBrush);
        }

        // 缩放回实际尺寸（HALFTONE 算法产生平滑效果）
        SetStretchBltMode(memDC, HALFTONE);
        SetBrushOrgEx(memDC, 0, 0, nullptr);
        StretchBlt(memDC, 0, 0, w, h, scaleDC, 0, 0, sw, sh, SRCCOPY);

        SelectObject(scaleDC, oldScaleBmp);
        DeleteObject(scaleBmp);
        DeleteDC(scaleDC);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// 注册 Spinner 窗口类（应在创建第一个 spinner 前调用一次）
static void RegisterSpinnerClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SpinnerWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kSpinnerClass;
    RegisterClassExW(&wc);
}

// ── 内部辅助：从窗口获取 MainWindowState ──
static MainWindowState* GetState(HWND hWnd) {
    return (MainWindowState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
}

static void TrackAsyncThread(MainWindowState* s, std::thread&& worker) {
    std::lock_guard<std::mutex> lk(s->asyncThreadsMutex);
    s->asyncThreads.push_back(std::move(worker));
}

static void JoinAsyncThreads(MainWindowState* s) {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(s->asyncThreadsMutex);
        threads.swap(s->asyncThreads);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

static void DrainAsyncResultMessages(HWND hWnd) {
    MSG msg{};
    while (PeekMessageW(&msg, hWnd, WM_APP_ROWS_READY, WM_APP_ROWS_READY, PM_REMOVE)) {
        delete (AsyncLoadResult*)msg.lParam;
    }
    while (PeekMessageW(&msg, hWnd, WM_APP_EXTRACT_DONE, WM_APP_EXTRACT_DONE, PM_REMOVE)) {
        delete (ExtractResult*)msg.lParam;
    }
}

// 获取窗口所在显示器的 DPI（Win8.1+ 返回真实值，否则回退到系统 DPI）
static UINT GetWindowDpi(HWND hWnd) {
    using FnGetDpiForWindow = UINT(WINAPI*)(HWND);
    static auto fn = (FnGetDpiForWindow)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) return fn(hWnd);
    HDC hdc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi ? dpi : 96;
}

// 将 96dpi 下的逻辑像素换算为当前 DPI 下的物理像素
static int ScaleDpi(int px, UINT dpi) {
    return MulDiv(px, (int)dpi, 96);
}

// 按当前 DPI 创建/重建 UI 字体并应用到控件
static void RecreateFonts(HWND hWnd, MainWindowState* s, UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    // SPI_GETNONCLIENTMETRICS 在 PerMonitorV2 下仍返回主显示器 DPI 的值，
    // 需手动将 lfHeight 按目标 DPI 重新缩放
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    // SPI_GETNONCLIENTMETRICS 返回的 lfHeight 基于主显示器 DPI，需按目标 DPI 重新缩放
    // lfHeight 为负时其绝对值是字符高度（点数），为正时是单元格高度
    const UINT sysDpi = []() -> UINT {
        HDC hdc = GetDC(nullptr);
        UINT d = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        return d ? d : 96;
    }();
    if (ncm.lfMessageFont.lfHeight != 0) {
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)dpi, (int)sysDpi);
    }

    HFONT hOldSearch = s->hSearchFont;
    HFONT hOldNormal = s->hNormalFont;
    HFONT hOldBold   = s->hBoldFont;

    ncm.lfMessageFont.lfWeight = FW_NORMAL;
    s->hSearchFont = CreateFontIndirectW(&ncm.lfMessageFont);
    s->hNormalFont = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    s->hBoldFont = CreateFontIndirectW(&ncm.lfMessageFont);

    if (s->hSearchFont && s->hSearch)
        SendMessageW(s->hSearch, WM_SETFONT, (WPARAM)s->hSearchFont, TRUE);

    if (hOldSearch) DeleteObject(hOldSearch);
    if (hOldNormal) DeleteObject(hOldNormal);
    if (hOldBold)   DeleteObject(hOldBold);
}

// ── 内部辅助：LS 的便捷包装（从 state 获取 hInstance）──
static std::wstring LS_(MainWindowState* s, UINT id) {
    return LS(s->hInstance, id, GetEffectiveLanguageId(s->userConfig));
}

// 获取搜索框中的过滤文本（转小写，去除通配符 *）
static std::wstring GetSearchFilter(MainWindowState* s) {
    wchar_t buf[1024]{};
    if (s->hSearch) {
        GetWindowTextW(s->hSearch, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    }
    std::wstring f = buf;
    f = ToLower(f);
    for (auto& ch : f) {
        if (ch == L'*') ch = 0;
    }
    f.erase(std::remove(f.begin(), f.end(), 0), f.end());
    return f;
}

static std::wstring GetArchiveColumnText(MainWindowState* s, const CachedRow* cr) {
    if (!cr) return {};
    return s->showArchiveFullPath ? cr->archivePath : GetEntryNameFromPath(cr->archivePath);
}

static std::wstring GetArchiveColumnTipText(MainWindowState* s, const CachedRow* cr) {
    if (!cr) return {};
    return s->showArchiveFullPath ? GetEntryNameFromPath(cr->archivePath) : cr->archivePath;
}

static bool IsSubItemTextTruncated(HWND hList, MainWindowState* s, int item, int subItem, const std::wstring& text) {
    if (!hList || !s || text.empty()) return false;

    RECT rc{};
    if (subItem == 0) {
        if (!ListView_GetItemRect(hList, item, &rc, LVIR_LABEL)) {
            return false;
        }
    } else {
        if (!ListView_GetSubItemRect(hList, item, subItem, LVIR_BOUNDS, &rc)) {
            return false;
        }
    }

    const int padding = 8;
    const int availableWidth = (rc.right - rc.left) - padding;
    if (availableWidth <= 0) return true;

    HDC hdc = GetDC(hList);
    if (!hdc) return false;

    HFONT oldFont = nullptr;
    if (s->hNormalFont) {
        oldFont = (HFONT)SelectObject(hdc, s->hNormalFont);
    }

    SIZE sz{};
    const BOOL measured = GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hList, hdc);

    return measured && sz.cx > availableWidth;
}

static void LoadRowsFromDbAndRefreshAsync(HWND hWnd, MainWindowState* s);
static void UpdateStatusBar(MainWindowState* s);
static void StartInitialIndexingAfterConsent(HWND hWnd, MainWindowState* s);
static void RefreshLocalizedMainWindow(HWND hWnd, MainWindowState* s);

// 创建主窗口菜单栏
static HMENU CreateMainMenu(MainWindowState* s) {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, LS_(s, IDS_MENU_FILE_EXIT).c_str());

    HMENU hAbout = CreatePopupMenu();
    AppendMenuW(hAbout, MF_STRING, IDM_HELP_ABOUT,     LS_(s, IDS_MENU_HELP_ABOUT).c_str());
    AppendMenuW(hAbout, MF_STRING, IDM_CHECK_UPDATE,   LS_(s, IDS_MENU_CHECK_UPDATE).c_str());

    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile,     LS_(s, IDS_MENU_FILE).c_str());
    AppendMenuW(hMenuBar, MF_STRING, IDM_TOOLS_OPTIONS, LS_(s, IDS_MENU_SETTINGS).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hAbout,    LS_(s, IDS_MENU_HELP).c_str());

    return hMenuBar;
}

// 初始化 ListView 列头（名称、路径、压缩大小、原始大小）
static void SetupListColumns(MainWindowState* s) {
    HWND hList = s->hList;
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    HWND hHeader = ListView_GetHeader(hList);
    if (hHeader) {
        LONG style = GetWindowLongW(hHeader, GWL_STYLE);
        SetWindowLongW(hHeader, GWL_STYLE, style | HDS_FLAT);
    }

    // 获取系统小图标 ImageList 并关联到 ListView（用于显示文件类型图标）
    HIMAGELIST hIml = s->iconCache.InitSysImageList();
    if (hIml) {
        ListView_SetImageList(hList, hIml, LVSIL_SMALL);
    }

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    int widths[] = { 210, 210, 280, 100, 100, 140 };
    if (s->userConfig.GetRememberUiState()) {
        const auto& savedWidths = s->userConfig.GetListColumnWidths();
        if (savedWidths.size() == 6) {
            for (int i = 0; i < 6; ++i) {
                if (savedWidths[i] > 0) {
                    widths[i] = savedWidths[i];
                }
            }
        }
    }

    std::wstring colName = LS_(s, IDS_COL_NAME);
    col.pszText = const_cast<LPWSTR>(colName.c_str());
    col.cx = widths[0];
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    // 归档文件路径列
    std::wstring colArchive = LS_(s, IDS_COL_ARCHIVE);
    col.pszText = const_cast<LPWSTR>(colArchive.c_str());
    col.cx = widths[1];
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    // 归档内部文件路径列
    std::wstring colPath = LS_(s, IDS_COL_PATH);
    col.pszText = const_cast<LPWSTR>(colPath.c_str());
    col.cx = widths[2];
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    // 压缩大小列（右对齐）
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt = LVCFMT_RIGHT;
    std::wstring colCompSize = LS_(s, IDS_COL_COMPRESSED_SIZE);
    col.pszText = const_cast<LPWSTR>(colCompSize.c_str());
    col.cx = widths[3];
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);

    // 原始大小列（右对齐）
    std::wstring colOrigSize = LS_(s, IDS_COL_ORIGINAL_SIZE);
    col.pszText = const_cast<LPWSTR>(colOrigSize.c_str());
    col.cx = widths[4];
    col.iSubItem = 4;
    ListView_InsertColumn(hList, 4, &col);

    col.fmt = LVCFMT_LEFT;
    std::wstring colModifiedTime = LS_(s, IDS_COL_MODIFIED_TIME);
    col.pszText = const_cast<LPWSTR>(colModifiedTime.c_str());
    col.cx = widths[5];
    col.iSubItem = 5;
    ListView_InsertColumn(hList, 5, &col);
}

// 刷新 ListView 显示内容（虚拟列表模式：只需设置条目数量）
static void RefreshList(MainWindowState* s) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(s->rowsMutex);
        count = (int)s->rowIds.size();
    }
    ListView_SetItemCountEx(s->hList, count, LVSICF_NOSCROLL);
    InvalidateRect(s->hList, nullptr, TRUE);
}

static bool SaveUiState(HWND hWnd, MainWindowState* s) {
    if (!hWnd || !s || !s->userConfig.GetRememberUiState()) {
        return true;
    }

    if (IsWindowVisible(hWnd)) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hWnd, &wp)) {
            UserConfig::WindowPlacementConfig placement{};
            placement.left = wp.rcNormalPosition.left;
            placement.top = wp.rcNormalPosition.top;
            placement.right = wp.rcNormalPosition.right;
            placement.bottom = wp.rcNormalPosition.bottom;
            placement.maximized = IsZoomed(hWnd) || wp.showCmd == SW_SHOWMAXIMIZED;
            s->userConfig.SetWindowPlacement(placement);
        }
    }

    if (s->hList) {
        std::vector<int> widths;
        widths.reserve(6);
        for (int i = 0; i < 6; ++i) {
            widths.push_back(ListView_GetColumnWidth(s->hList, i));
        }
        s->userConfig.SetListColumnWidths(widths);
    }

    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        LOG_WARN(L"Save UI state failed: %s", err.c_str());
        return false;
    }
    return true;
}
static SettingsWindowCallbacks MakeSettingsWindowCallbacks() {
    SettingsWindowCallbacks callbacks{};
    callbacks.saveUiState = SaveUiState;
    callbacks.refreshList = RefreshList;
    callbacks.loadRowsFromDbAndRefreshAsync = LoadRowsFromDbAndRefreshAsync;
    callbacks.updateStatusBar = UpdateStatusBar;
    callbacks.refreshLocalizedMainWindow = RefreshLocalizedMainWindow;
    return callbacks;
}

// 更新 ListView 列头文本，在当前排序列后附加箭头指示符（▲/▼）
static void UpdateColumnHeaders(MainWindowState* s) {
    static const UINT colIds[] = {
        IDS_COL_NAME, IDS_COL_ARCHIVE, IDS_COL_PATH,
        IDS_COL_COMPRESSED_SIZE, IDS_COL_ORIGINAL_SIZE, IDS_COL_MODIFIED_TIME
    };

    for (int i = 0; i < 6; ++i) {
        std::wstring text = LS_(s, colIds[i]);
        if (i == s->sortColumn) {
            text += s->sortAscending ? LS_(s, IDS_SORT_ASC) : LS_(s, IDS_SORT_DESC);
        }

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_SetColumn(s->hList, i, &col);
    }
}

static void RefreshLocalizedMainWindow(HWND hWnd, MainWindowState* s) {
    if (!hWnd || !s) return;

    HMENU oldMenu = GetMenu(hWnd);
    SetMenu(hWnd, CreateMainMenu(s));
    if (oldMenu) DestroyMenu(oldMenu);
    DrawMenuBar(hWnd);

    UpdateColumnHeaders(s);
    UpdateStatusBar(s);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static int MeasureWindowTextWidth(HWND hWnd, const std::wstring& text) {
    if (!hWnd || text.empty()) return 0;

    HDC hdc = GetDC(hWnd);
    if (!hdc) return 0;

    HFONT hFont = (HFONT)SendMessageW(hWnd, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : nullptr;

    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &size);

    if (hOld) SelectObject(hdc, hOld);
    ReleaseDC(hWnd, hdc);
    return size.cx;
}

static void UpdateStatusBarParts(HWND hWnd, MainWindowState* s,
                                 const std::wstring& rightText = std::wstring(),
                                 bool reserveSpinner = false) {
    if (!s || !s->hStatusBar) return;

    RECT rc{};
    GetClientRect(hWnd, &rc);
    const UINT dpi = GetWindowDpi(hWnd);
    int rightWidth = ScaleDpi(140, dpi);
    if (!rightText.empty()) {
        RECT sbRect{};
        GetClientRect(s->hStatusBar, &sbRect);
        const int spinnerReserve = reserveSpinner
            ? max(0, sbRect.bottom - sbRect.top) + ScaleDpi(8, dpi)
            : 0;
        rightWidth = max(ScaleDpi(56, dpi),
                         MeasureWindowTextWidth(s->hStatusBar, rightText) +
                         ScaleDpi(24, dpi) + spinnerReserve);
    }

    int parts[2] = {
        max(0, rc.right - rightWidth),
        -1
    };
    SendMessageW(s->hStatusBar, SB_SETPARTS, 2, (LPARAM)parts);
}

// 根据主窗口大小重新布局子控件（搜索框、ListView、状态栏）
static void LayoutChildren(HWND hWnd, MainWindowState* s) {
    RECT rc{};
    GetClientRect(hWnd, &rc);

    const UINT dpi = GetWindowDpi(hWnd);
    const int margin = ScaleDpi(6, dpi);

    // 根据字体度量动态计算搜索框高度，避免高 DPI 下出现过大空白
    int editH = ScaleDpi(24, dpi);
    if (s->hSearch) {
        HDC hdc = GetDC(s->hSearch);
        if (hdc) {
            HFONT hFont = s->hSearchFont ? s->hSearchFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT hOld = (HFONT)SelectObject(hdc, hFont);
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOld);
            ReleaseDC(s->hSearch, hdc);
            // 字体高度 + 行间距 + 上下内边距(4px×2) + 边框(2px×2)
            const int border = ScaleDpi(2, dpi);
            const int padding = ScaleDpi(4, dpi);
            editH = tm.tmHeight + tm.tmExternalLeading + padding * 2 + border * 2;
        }
    }

    RECT statusRect{};
    if (s->hStatusBar) {
        SendMessageW(s->hStatusBar, WM_SIZE, 0, 0);
        UpdateStatusBarParts(hWnd, s);
        GetWindowRect(s->hStatusBar, &statusRect);
    }
    const int statusH = statusRect.bottom - statusRect.top;

    int x = margin;
    int y = margin;
    int w = (rc.right - rc.left) - margin * 2;
    int h = (rc.bottom - rc.top) - margin * 2 - statusH;

    MoveWindow(s->hSearch, x, y, w, editH, TRUE);

    y += editH + margin;
    h -= editH + margin;

    MoveWindow(s->hList, x, y, w, h, TRUE);
}

// 异步加载：在后台线程中只查询 rowid 列表，完成后通知 UI 线程
static void LoadRowsFromDbAndRefreshAsync(HWND hWnd, MainWindowState* s) {
    // 捕获当前搜索条件和排序状态（在 UI 线程中获取）
    std::wstring filter = GetSearchFilter(s);
    std::wstring dbPath = s->dbPath;
    int sortCol = s->sortColumn;
    bool sortAsc = s->sortAscending;
    uint64_t generation = ++s->loadGeneration;
    s->pendingRowLoads.fetch_add(1);
    UpdateStatusBar(s);

    TrackAsyncThread(s, std::thread([hWnd, s, filter = std::move(filter), dbPath = std::move(dbPath), sortCol, sortAsc, generation]() {
        auto* result = new AsyncLoadResult();
        result->generation = generation;

        std::wstring err;
        Database db;
        if (!db.Open(dbPath, &err)) {
            LOG_WARN(L"Database::Open failed: %s", err.c_str());
            if (s->shuttingDown.load() || !PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result)) {
                s->pendingRowLoads.fetch_sub(1);
                delete result;
            }
            return;
        }

        db.CreateEntriesTable(&err);

        // 只查询 rowid 列表（36万条 ≈ 3MB，而非 1.4GB 的完整数据）
        if (!db.QueryEntryIds(filter, sortCol, sortAsc, &result->rowIds, &err)) {
            LOG_WARN(L"QueryEntryIds failed: %s", err.c_str());
            if (s->shuttingDown.load() || !PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result)) {
                s->pendingRowLoads.fetch_sub(1);
                delete result;
            }
            return;
        }

        result->entryCount = (int64_t)result->rowIds.size();
        result->archiveCount = db.GetArchiveCount();

        if (s->shuttingDown.load() || !PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result)) {
            s->pendingRowLoads.fetch_sub(1);
            delete result;
        }
    }));
}

// 更新底部状态栏（显示索引状态、文件数量和归档文件数量）
static bool SaveStartupScanConfirmed(MainWindowState* s) {
    if (!s) return false;
    s->userConfig.SetStartupScanConfirmed(true);
    std::wstring err;
    if (!s->userConfig.Save(&err)) {
        std::wstring msg = LS_(s, IDS_SETTINGS_SAVE_FAILED);
        if (!err.empty()) msg += L"\n" + err;
        HWND hWnd = s->hStatusBar ? GetParent(s->hStatusBar) : nullptr;
        MessageBoxW(hWnd, msg.c_str(), LS_(s, IDS_ERROR).c_str(), MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

static void StartInitialIndexingAfterConsent(HWND hWnd, MainWindowState* s) {
    if (!s) return;
    s->parseDoneCount.store(0);
    s->parseTotalCount.store(0);
    s->indexer.EnsureDatabaseReady();
    s->rowCache.Clear();
    LoadRowsFromDbAndRefreshAsync(hWnd, s);
    s->indexer.SetArchiveFormatRules(s->userConfig.GetArchiveFormatRules());
    s->indexer.Start(hWnd);
    UpdateStatusBar(s);
}

static void ShowStartupScanPrompt(HWND hWnd, MainWindowState* s) {
    if (!s || s->userConfig.GetStartupScanConfirmed()) return;

    static constexpr int kStartupScanSettingsButton = 10001;
    const std::wstring okText = LS_(s, IDS_STARTUP_SCAN_OK);
    const std::wstring settingsText = LS_(s, IDS_STARTUP_SCAN_SETTINGS);
    TASKDIALOG_BUTTON buttons[] = {
        { IDOK, okText.c_str() },
        { kStartupScanSettingsButton, settingsText.c_str() },
    };

    const std::wstring title = LS_(s, IDS_STARTUP_SCAN_TITLE);
    const std::wstring content = LS_(s, IDS_STARTUP_SCAN_TEXT);
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = hWnd;
    cfg.hInstance = s->hInstance;
    cfg.dwFlags = TDF_SIZE_TO_CONTENT;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = title.c_str();
    cfg.pszMainInstruction = title.c_str();
    cfg.pszContent = content.c_str();
    cfg.cButtons = _countof(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = IDOK;

    int pressed = IDCANCEL;
    HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (FAILED(hr)) {
        pressed = MessageBoxW(hWnd, content.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONINFORMATION);
    }

    if (pressed == IDOK) {
        if (SaveStartupScanConfirmed(s)) {
            StartInitialIndexingAfterConsent(hWnd, s);
        }
        return;
    }

    if (pressed == kStartupScanSettingsButton) {
        ShowSettingsPanel(hWnd, s, MakeSettingsWindowCallbacks(), false);
        if (SaveStartupScanConfirmed(s)) {
            StartInitialIndexingAfterConsent(hWnd, s);
        }
    }
}

static void UpdateStatusBar(MainWindowState* s) {
    if (!s->hStatusBar) return;
    HWND hWnd = GetParent(s->hStatusBar);

    int fileCount = 0;
    {
        std::lock_guard<std::mutex> lk(s->rowsMutex);
        fileCount = (int)s->rowIds.size();
    }

    // 使用缓存的归档文件数量（由异步加载结果更新，避免频繁查询数据库）
    int64_t archiveCount = s->cachedArchiveCount.load();

    // 格式化数字（添加千位分隔符）
    std::wstring fileCountStr = AddThousandsSeparator(std::to_wstring(fileCount));
    std::wstring archiveCountStr = AddThousandsSeparator(std::to_wstring((long long)archiveCount));

    const Indexer::Stage indexerStage = s->indexer.GetStage();
    const bool indexerBusy =
        indexerStage == Indexer::Stage::InitialScanning ||
        indexerStage == Indexer::Stage::SyncingDatabase ||
        indexerStage == Indexer::Stage::ParsingArchives ||
        indexerStage == Indexer::Stage::Stopping;
    const bool rowsLoading = s->pendingRowLoads.load() > 0;
    const int64_t parseDone = s->parseDoneCount.load();
    const int64_t parseTotal = s->parseTotalCount.load();
    const bool parsing = indexerStage == Indexer::Stage::ParsingArchives;
    const bool hasParseProgress = parsing && parseTotal > 0;
    const bool showSpinner = indexerBusy || rowsLoading;

    // 控制 Spinner 的显示/隐藏，并定位到状态栏右侧
    if (s->hProgress) {
        bool isVisible = (GetWindowLongW(s->hProgress, GWL_STYLE) & WS_VISIBLE) != 0;
        if (showSpinner) {
            RECT sbRect{};
            GetClientRect(s->hStatusBar, &sbRect);
            const int size   = sbRect.bottom - sbRect.top - 2;
            const UINT dpi = hWnd ? GetWindowDpi(hWnd) : 96;
            const int pbX = max(0, sbRect.right - size - ScaleDpi(2, dpi));
            const int pbY    = 1;
            MoveWindow(s->hProgress, pbX, pbY, size, size, FALSE);
            if (!isVisible) {
                ShowWindow(s->hProgress, SW_SHOW);
                // 启动动画定时器（每 40ms 旋转一帧），挂在主窗口上
                if (hWnd) SetTimer(hWnd, IDT_SPINNER_ANIM, 40, nullptr);
            }
        } else if (!showSpinner && isVisible) {
            if (hWnd) KillTimer(hWnd, IDT_SPINNER_ANIM);
            ShowWindow(s->hProgress, SW_HIDE);
        }
    }

    std::wstring leftText = LS_(s, IDS_STATUS_FILE_COUNT) + L": " + fileCountStr +
                            L" | " + LS_(s, IDS_STATUS_ARCHIVE_COUNT) + L": " + archiveCountStr;

    std::wstring rightText = LS_(s, IDS_STATUS_READY);
    if (hasParseProgress) {
        const int64_t clampedDone = min(parseDone, parseTotal);
        const int percent = parseTotal > 0 ? (int)((clampedDone * 100) / parseTotal) : 0;
        rightText = LS_(s, IDS_STATUS_PARSING) + L" " +
                    std::to_wstring((long long)clampedDone) + L"/" +
                    std::to_wstring((long long)parseTotal) + L"  " +
                    std::to_wstring(percent) + L"%";
    } else if (parsing) {
        rightText = LS_(s, IDS_STATUS_PARSING);
    } else if (indexerStage == Indexer::Stage::InitialScanning) {
        rightText = LS_(s, IDS_STATUS_SCANNING);
    } else if (indexerStage == Indexer::Stage::SyncingDatabase) {
        rightText = LS_(s, IDS_STATUS_UPDATING_INDEX);
    } else if (rowsLoading) {
        rightText = LS_(s, IDS_STATUS_REFRESHING_LIST);
    } else if (indexerStage == Indexer::Stage::Stopping) {
        rightText = LS_(s, IDS_STATUS_PROCESSING);
    }

    if (hWnd) {
        UpdateStatusBarParts(hWnd, s, rightText, showSpinner);
    }

    SendMessageW(s->hStatusBar, SB_SETTEXTW, 0, (LPARAM)leftText.c_str());
    SendMessageW(s->hStatusBar, SB_SETTEXTW, 1, (LPARAM)rightText.c_str());
}

// 自绘文本：将匹配搜索关键词的部分用粗体绘制，其余用普通字体
static void DrawTextWithBoldMatch(HDC hdc, const RECT& rcCell, const std::wstring& text, const std::wstring& filter,
                                  COLORREF textColor, HFONT hNormalFont, HFONT hBoldFont) {
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);

    if (filter.empty()) {
        RECT rc = rcCell;
        rc.left += 4;
        DrawTextW(hdc, text.c_str(), (int)text.size(), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }

    std::wstring lower = ToLower(text);
    int x = rcCell.left + 4;
    size_t pos = 0;

    while (pos < text.size()) {
        size_t found = lower.find(filter, pos);
        if (found == std::wstring::npos) found = text.size();

        if (found > pos) {
            SelectObject(hdc, hNormalFont);
            std::wstring seg = text.substr(pos, found - pos);
            SIZE sz{};
            GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.size(), &sz);
            RECT rc = { x, rcCell.top, x + sz.cx, rcCell.bottom };
            DrawTextW(hdc, seg.c_str(), (int)seg.size(), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += sz.cx;
        }

        if (found < text.size()) {
            SelectObject(hdc, hBoldFont);
            std::wstring seg = text.substr(found, filter.size());
            SIZE sz{};
            GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.size(), &sz);
            RECT rc = { x, rcCell.top, x + sz.cx, rcCell.bottom };
            DrawTextW(hdc, seg.c_str(), (int)seg.size(), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += sz.cx;
            pos = found + filter.size();
        } else {
            break;
        }
    }
}

static void HideArchiveTooltip(MainWindowState* s) {
    if (!s || !s->hArchiveTooltip || !s->archiveTooltipTracking) return;

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = s->hList;
    ti.uId = 1;
    SendMessageW(s->hArchiveTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    s->archiveTooltipTracking = false;
    s->archiveTooltipItem = -1;
    s->archiveTooltipSubItem = -1;
    s->archiveTooltipText.clear();
}

static int64_t GetRowIdByListItem(MainWindowState* s, int item) {
    if (!s || item < 0) return 0;
    std::lock_guard<std::mutex> lk(s->rowsMutex);
    if (item >= 0 && item < (int)s->rowIds.size()) {
        return s->rowIds[item];
    }
    return 0;
}

static void UpdateArchiveTooltip(HWND hList, MainWindowState* s, LPARAM lParam) {
    if (!s || !s->hArchiveTooltip) return;

    POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
    LVHITTESTINFO hit{};
    hit.pt = pt;
    const int item = ListView_SubItemHitTest(hList, &hit);
    if (item < 0 || (hit.iSubItem != 0 && hit.iSubItem != 1 && hit.iSubItem != 2)) {
        HideArchiveTooltip(s);
        return;
    }

    const int64_t rowId = GetRowIdByListItem(s, item);
    if (rowId <= 0) {
        HideArchiveTooltip(s);
        return;
    }

    const CachedRow* cr = s->rowCache.Get(rowId);
    if (!cr) {
        HideArchiveTooltip(s);
        return;
    }

    std::wstring tip;
    if (hit.iSubItem == 0) {
        if (cr->name.empty() || !IsSubItemTextTruncated(hList, s, item, hit.iSubItem, cr->name)) {
            HideArchiveTooltip(s);
            return;
        }
        tip = cr->name;
    } else if (hit.iSubItem == 1) {
        if (cr->archivePath.empty()) {
            HideArchiveTooltip(s);
            return;
        }
        tip = GetArchiveColumnTipText(s, cr);
    } else if (hit.iSubItem == 2) {
        if (cr->entryPath.empty() || !IsSubItemTextTruncated(hList, s, item, hit.iSubItem, cr->entryPath)) {
            HideArchiveTooltip(s);
            return;
        }
        tip = cr->entryPath;
    }

    if (tip.empty()) {
        HideArchiveTooltip(s);
        return;
    }

    POINT screenPt = pt;
    ClientToScreen(hList, &screenPt);
    screenPt.x += 12;
    screenPt.y += 20;

    const bool changed = item != s->archiveTooltipItem ||
                         hit.iSubItem != s->archiveTooltipSubItem ||
                         tip != s->archiveTooltipText;

    if (changed) {
        s->archiveTooltipItem = item;
        s->archiveTooltipSubItem = hit.iSubItem;
        s->archiveTooltipText = std::move(tip);

        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hList;
        ti.uId = 1;
        ti.lpszText = const_cast<LPWSTR>(s->archiveTooltipText.c_str());
        SendMessageW(s->hArchiveTooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hList;
    ti.uId = 1;
    SendMessageW(s->hArchiveTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(screenPt.x, screenPt.y));
    SendMessageW(s->hArchiveTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    s->archiveTooltipTracking = true;

    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hList;
    TrackMouseEvent(&tme);
}

static void InitArchiveTooltip(MainWindowState* s) {
    if (!s || !s->hList || s->hArchiveTooltip) return;

    s->hArchiveTooltip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        s->hList,
        nullptr,
        s->hInstance,
        nullptr);
    if (!s->hArchiveTooltip) return;

    SetWindowPos(s->hArchiveTooltip, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(s->hArchiveTooltip, TTM_SETMAXTIPWIDTH, 0, 600);
    SendMessageW(s->hArchiveTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 300);
    SendMessageW(s->hArchiveTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);

    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = s->hList;
    ti.uId = 1;
    ti.lpszText = const_cast<LPWSTR>(L"");
    GetClientRect(s->hList, &ti.rect);
    SendMessageW(s->hArchiveTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static LRESULT CALLBACK ResultsListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND hParent = GetParent(hWnd);
    MainWindowState* s = hParent ? GetState(hParent) : nullptr;
    WNDPROC oldProc = s ? s->listOldProc : DefWindowProcW;

    switch (msg) {
    case WM_MOUSEMOVE:
        UpdateArchiveTooltip(hWnd, s, lParam);
        break;
    case WM_MOUSELEAVE:
    case WM_MOUSEWHEEL:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_KEYDOWN:
        HideArchiveTooltip(s);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            UINT commandId = 0;
            if (wParam == 'C') {
                commandId = IDM_CTX_COPY_ARCHIVE;
            } else if (wParam == 'N') {
                commandId = IDM_CTX_COPY_NAME;
            } else if (wParam == 'P') {
                commandId = IDM_CTX_COPY_ENTRY_PATH;
            }
            if (hParent && ListView_GetNextItem(hWnd, -1, LVNI_SELECTED) >= 0) {
                if (commandId != 0) {
                    PostMessageW(hParent, WM_COMMAND, MAKEWPARAM(commandId, 0), 0);
                    return 0;
                }
            }
        }
        break;
    case WM_DESTROY:
        HideArchiveTooltip(s);
        break;
    }

    return CallWindowProcW(oldProc, hWnd, msg, wParam, lParam);
}

// 搜索框子类化窗口过程：拦截回车键触发搜索，监听文本变化实时检索
static LRESULT CALLBACK SearchEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 获取父窗口的 state 以访问 editOldProc
    HWND hParent = GetParent(hWnd);
    MainWindowState* s = hParent ? GetState(hParent) : nullptr;
    WNDPROC oldProc = s ? s->editOldProc : DefWindowProcW;

    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        if (hParent) {
            // 回车键立即执行搜索（跳过防抖）
            KillTimer(hParent, IDT_SEARCH_DEBOUNCE);
            PostMessageW(hParent, WM_COMMAND, MAKEWPARAM(IDM_SEARCH_FIND, 0), 0);
        }
        return 0;
    }
    LRESULT result = CallWindowProcW(oldProc, hWnd, msg, wParam, lParam);
    if (msg == WM_CHAR || msg == WM_CLEAR || msg == WM_CUT || msg == WM_PASTE || msg == WM_UNDO ||
        (msg == WM_KEYUP && (wParam == VK_DELETE || wParam == VK_BACK))) {
        if (hParent) {
            // 文本变化时使用防抖定时器，避免频繁创建查询线程
            SetTimer(hParent, IDT_SEARCH_DEBOUNCE, 200, nullptr);
        }
    }
    return result;
}

// ══════════════════════════════════════════════════════════════
//  主窗口消息处理
// ══════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // WM_CREATE 时从 CREATESTRUCT 获取 state 并存入 GWLP_USERDATA
    if (msg == WM_CREATE) {
        LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lParam;
        MainWindowState* s = (MainWindowState*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)s);
    }

    MainWindowState* s = GetState(hWnd);
    if (!s) return DefWindowProcW(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE: { // 窗口创建：初始化所有子控件、字体、数据库，启动索引
        LOG_INFO(L"WM_CREATE");

        SetMenu(hWnd, CreateMainMenu(s));

        s->hSearch = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_SEARCH_EDIT,
            s->hInstance,
            nullptr);

        if (!s->hSearch) {
            LOG_ERROR(L"CreateWindowExW search edit failed (err=%lu)", GetLastError());
        }

        s->hList = CreateWindowExW(
            0,
            WC_LISTVIEWW,
            L"",
            // LVS_OWNERDATA: 虚拟列表模式，数据由 LVN_GETDISPINFO 回调提供，排序切换无需重建条目
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_RESULTS_LIST,
            s->hInstance,
            nullptr);

        if (!s->hList) {
            LOG_ERROR(L"CreateWindowExW listview failed (err=%lu)", GetLastError());
        }

        if (s->hList) {
            SetupListColumns(s);
            InitArchiveTooltip(s);
            s->listOldProc = (WNDPROC)SetWindowLongPtrW(s->hList, GWLP_WNDPROC, (LONG_PTR)ResultsListProc);
        }

        s->hStatusBar = CreateWindowExW(
            0,
            STATUSCLASSNAMEW,
            nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_STATUSBAR,
            s->hInstance,
            nullptr);

        if (!s->hStatusBar) {
            LOG_ERROR(L"CreateWindowExW statusbar failed (err=%lu)", GetLastError());
        }

        // 在状态栏内创建自绘圆形转圈 Spinner（初始隐藏）
        if (s->hStatusBar) {
            RegisterSpinnerClass(s->hInstance);
            s->hProgress = CreateWindowExW(
                0,
                kSpinnerClass,
                nullptr,
                WS_CHILD,  // 不含 WS_VISIBLE，初始隐藏
                0, 0, 0, 0,
                s->hStatusBar,
                (HMENU)(INT_PTR)IDC_PROGRESS,
                s->hInstance,
                nullptr);
        }

        s->editOldProc = (WNDPROC)SetWindowLongPtrW(s->hSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);

        RecreateFonts(hWnd, s, GetWindowDpi(hWnd));

        RefreshList(s);
        LayoutChildren(hWnd, s);

        SetTimer(hWnd, IDT_STATUSBAR_TIMER, 200, nullptr);
        UpdateStatusBar(s);

        // 创建系统托盘图标
        AddTrayIcon(hWnd, IDI_TRAY_ICON, WM_APP_TRAY);
        if (s->userConfig.GetStartupScanConfirmed()) {
            StartInitialIndexingAfterConsent(hWnd, s);
        } else {
            s->indexer.EnsureDatabaseReady();
            LoadRowsFromDbAndRefreshAsync(hWnd, s);
            PostMessageW(hWnd, WM_APP_STARTUP_SCAN_PROMPT, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
        // 关闭窗口只是隐藏到托盘，不退出程序
        if (!s->forceQuit) {
            SaveUiState(hWnd, s);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        SaveUiState(hWnd, s);
        // g_forceQuit=true 时落入 DefWindowProc 触发 WM_DESTROY
        break;

    case WM_DPICHANGED: {
        // 系统提供新 DPI 和建议窗口矩形，直接采纳以避免模糊
        UINT newDpi = HIWORD(wParam);
        const RECT* suggested = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateFonts(hWnd, s, newDpi);
        LayoutChildren(hWnd, s);
        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hWnd, s);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_STATUSBAR_TIMER) {
            UpdateStatusBar(s);
            return 0;
        }
        if (wParam == IDT_SEARCH_DEBOUNCE) {
            KillTimer(hWnd, IDT_SEARCH_DEBOUNCE);
            LoadRowsFromDbAndRefreshAsync(hWnd, s);
            return 0;
        }
        if (wParam == IDT_LIST_RETRY) {
            KillTimer(hWnd, IDT_LIST_RETRY);
            s->needsListRetry = false;
            LoadRowsFromDbAndRefreshAsync(hWnd, s);
            return 0;
        }
        if (wParam == IDT_SPINNER_ANIM) {
            if (s->hProgress && (GetWindowLongW(s->hProgress, GWL_STYLE) & WS_VISIBLE)) {
                s->spinnerAngle = (s->spinnerAngle + 12) % 360;
                SetWindowLongPtrW(s->hProgress, GWLP_USERDATA, (LONG_PTR)s->spinnerAngle);
                InvalidateRect(s->hProgress, nullptr, FALSE);
            }
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        LOG_INFO(L"WM_COMMAND id=%d", id);
        switch (id) {
        case IDM_FILE_EXIT:
            s->forceQuit = true;
            DestroyWindow(hWnd);
            return 0;
        case IDM_TRAY_SHOW:
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            return 0;
        case IDM_TRAY_EXIT:
            s->forceQuit = true;
            DestroyWindow(hWnd);
            return 0;
        case IDM_VIEW_REFRESH:
            return 0;
        case IDM_VIEW_STOP:
            s->indexer.Stop();
            UpdateStatusBar(s);
            return 0;
        case IDM_SEARCH_FIND:
            LoadRowsFromDbAndRefreshAsync(hWnd, s);
            return 0;
        case IDM_TOOLS_OPTIONS:
            ShowSettingsPanel(hWnd, s, MakeSettingsWindowCallbacks());
            return 0;
        case IDM_HELP_ABOUT:
            ShowAboutPanel(hWnd, s);
            return 0;
        case IDM_CHECK_UPDATE:
            return 0;
        case IDM_CTX_PROPERTIES: {
            int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
            if (iItem < 0) return 0;
            int64_t rowId = 0;
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                if (iItem >= 0 && iItem < (int)s->rowIds.size())
                    rowId = s->rowIds[iItem];
            }
            if (rowId <= 0) return 0;
            const CachedRow* cr = s->rowCache.Get(rowId);
            if (!cr || cr->archivePath.empty()) return 0;
            // 调用 Windows Shell 文件属性对话框
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask  = SEE_MASK_INVOKEIDLIST;
            sei.hwnd   = hWnd;
            sei.lpVerb = L"properties";
            sei.lpFile = cr->archivePath.c_str();
            sei.nShow  = SW_SHOW;
            ShellExecuteExW(&sei);
            return 0;
        }
        case IDM_CTX_EXTRACT: {
            // 获取选中条目的归档路径和条目路径
            int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
            if (iItem < 0) return 0;
            int64_t rowId = 0;
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                if (iItem >= 0 && iItem < (int)s->rowIds.size())
                    rowId = s->rowIds[iItem];
            }
            if (rowId <= 0) return 0;
            const CachedRow* cr = s->rowCache.Get(rowId);
            if (!cr || cr->archivePath.empty()) return 0;
            std::wstring archivePath = cr->archivePath;
            std::wstring entryPath   = cr->entryPath;

            // 将 entryPath（宽字符）转为 UTF-8 窄字符串，供解析器定位条目
            std::string entryPathA = cr->entryRawPath;
            if (entryPathA.empty()) {
                int need = WideCharToMultiByte(CP_UTF8, 0, entryPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (need > 0) {
                    entryPathA.resize(need - 1);
                    WideCharToMultiByte(CP_UTF8, 0, entryPath.c_str(), -1, entryPathA.data(), need, nullptr, nullptr);
                }
                // 将路径分隔符统一为正斜杠（minizip 使用正斜杠）
                std::replace(entryPathA.begin(), entryPathA.end(), '\\', '/');
            }

            // 用 SHBrowseForFolder 让用户选择目标目录
            wchar_t destBuf[MAX_PATH] = {};
            BROWSEINFOW bi{};
            bi.hwndOwner = hWnd;
            bi.pszDisplayName = destBuf;
            bi.lpszTitle = LS_(s, IDS_CTX_EXTRACT).c_str();
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (!pidl) return 0;
            if (!SHGetPathFromIDListW(pidl, destBuf)) {
                CoTaskMemFree(pidl);
                return 0;
            }
            CoTaskMemFree(pidl);
            std::wstring destDir = destBuf;
            std::wstring parserType = s->userConfig.GetParserForPath(archivePath);

            // 后台线程异步解压，完成后 PostMessage 通知 UI
            TrackAsyncThread(s, std::thread([hWnd, s, archivePath, entryPathA, destDir, parserType]() {
                auto* res = new ExtractResult();
                res->destDir = destDir;
                std::unique_ptr<EveryZip::IArchiveParser> parser =
                    EveryZip::CreateArchiveParserByType(parserType);
                std::string err;
                if (!parser) {
                    res->success = false;
                    res->errorMsg = "no parser for archive";
                } else if (!parser->Open(archivePath, &err)) {
                    res->success  = false;
                    res->errorMsg = err;
                } else {
                    res->success = parser->ExtractEntry(entryPathA, destDir, &err);
                    if (!res->success) res->errorMsg = err;
                    parser->Close();
                }
                if (s->shuttingDown.load() || !PostMessageW(hWnd, WM_APP_EXTRACT_DONE, res->success ? 1 : 0, (LPARAM)res)) {
                    delete res;
                }
            }));
            return 0;
        }
        case IDM_CTX_COPY_NAME:
        case IDM_CTX_COPY_ENTRY_PATH:
        case IDM_CTX_COPY_ARCHIVE: {
            int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
            if (iItem < 0) return 0;
            int64_t rowId = 0;
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                if (iItem >= 0 && iItem < (int)s->rowIds.size())
                    rowId = s->rowIds[iItem];
            }
            if (rowId <= 0) return 0;
            const CachedRow* cr = s->rowCache.Get(rowId);
            if (!cr) return 0;
            std::wstring text;
            if (id == IDM_CTX_COPY_NAME)       text = cr->name;
            else if (id == IDM_CTX_COPY_ENTRY_PATH) text = cr->entryPath;
            else                               text = cr->archivePath;
            if (text.empty()) return 0;
            // 将文本写入剩切板
            if (OpenClipboard(hWnd)) {
                EmptyClipboard();
                size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (hMem) {
                    void* p = GlobalLock(hMem);
                    if (p) {
                        memcpy(p, text.c_str(), bytes);
                        GlobalUnlock(hMem);
                    }
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
            }
            return 0;
        }
        case IDM_CTX_OPEN_ARCHIVE: {
            int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
            if (iItem < 0) return 0;
            int64_t rowId = 0;
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                if (iItem >= 0 && iItem < (int)s->rowIds.size())
                    rowId = s->rowIds[iItem];
            }
            if (rowId <= 0) return 0;
            const CachedRow* cr = s->rowCache.Get(rowId);
            if (!cr || cr->archivePath.empty()) return 0;
            ShellExecuteW(hWnd, L"open", cr->archivePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDM_CTX_OPEN_FOLDER: {
            // 获取当前选中项的归档文件路径
            int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
            if (iItem < 0) return 0;
            int64_t rowId = 0;
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                if (iItem >= 0 && iItem < (int)s->rowIds.size())
                    rowId = s->rowIds[iItem];
            }
            if (rowId <= 0) return 0;
            const CachedRow* cr = s->rowCache.Get(rowId);
            if (!cr || cr->archivePath.empty()) return 0;
            // 使用 Explorer /select 打开文件夹并选中归档文件
            std::wstring param = L"/select,\"" + cr->archivePath + L"\"";
            ShellExecuteW(hWnd, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
        default:
            return 0;
        }
    }

    case WM_APP_INDEX_DONE:
        s->parseDoneCount.store(s->parseTotalCount.load());
        UpdateStatusBar(s);
        LOG_INFO(L"WM_APP_INDEX_DONE");
        return 0;

    case WM_APP_DB_REFRESH:
        LoadRowsFromDbAndRefreshAsync(hWnd, s);
        return 0;

    case WM_APP_ROWS_READY: {
        // 后台线程查询完成，交换 rowid 列表到 UI 线程
        auto* result = (AsyncLoadResult*)lParam;
        s->pendingRowLoads.fetch_sub(1);
        if (result) {
            if (s->shuttingDown.load() || result->generation != s->loadGeneration.load()) {
                delete result;
                UpdateStatusBar(s);
                return 0;
            }
            {
                std::lock_guard<std::mutex> lk(s->rowsMutex);
                s->rowIds = std::move(result->rowIds);
                s->totalEntryCount = result->entryCount;
            }
            // 清空行缓存（数据可能已变化）
            s->rowCache.Clear();
            s->cachedArchiveCount.store(result->archiveCount);
            delete result;

            RefreshList(s);
            UpdateColumnHeaders(s);
            UpdateStatusBar(s);
        }
        return 0;
    }

    case WM_APP_UPDATE_STATUSBAR:
        UpdateStatusBar(s);
        return 0;

    case WM_APP_STARTUP_SCAN_PROMPT:
        ShowStartupScanPrompt(hWnd, s);
        return 0;

    case WM_APP_PARSE_PROGRESS:
        s->parseDoneCount.store((int64_t)wParam);
        s->parseTotalCount.store((int64_t)lParam);
        UpdateStatusBar(s);
        return 0;

    case WM_NOTIFY: { // ListView 通知：双击事件 + NM_CUSTOMDRAW 自绘（关键词加粗）
        LPNMHDR hdr = (LPNMHDR)lParam;
        HWND hHeader = s->hList ? ListView_GetHeader(s->hList) : nullptr;
        if (hdr && hHeader && hdr->hwndFrom == hHeader) {
            if (hdr->code == HDN_ENDTRACKW || hdr->code == HDN_ENDTRACKA ||
                hdr->code == HDN_ITEMCHANGEDW || hdr->code == HDN_ITEMCHANGEDA) {
                SaveUiState(hWnd, s);
                return 0;
            }
        }
        if (hdr && hdr->hwndFrom == s->hList) {
            // LVN_GETDISPINFO: 虚拟列表回调，按需从缓存/数据库获取行数据
            if (hdr->code == LVN_GETDISPINFOW) {
                NMLVDISPINFOW* pdi = (NMLVDISPINFOW*)lParam;
                int iItem = pdi->item.iItem;
                int iSub = pdi->item.iSubItem;

                // 获取 rowid
                int64_t rowId = 0;
                {
                    std::lock_guard<std::mutex> lk(s->rowsMutex);
                    if (iItem < 0 || iItem >= (int)s->rowIds.size()) return 0;
                    rowId = s->rowIds[iItem];
                }

                // 从 LRU 缓存获取行数据（缓存未命中时自动查询数据库）
                const CachedRow* cr = s->rowCache.Get(rowId);
                if (!cr) {
                    // rowId 已失效（索引器重建期间删除了旧条目），调度一次延迟刷新
                    if (!s->needsListRetry) {
                        s->needsListRetry = true;
                        SetTimer(hWnd, IDT_LIST_RETRY, 500, nullptr);
                    }
                    return 0;
                }

                if ((pdi->item.mask & LVIF_IMAGE) && iSub == 0) {
                    pdi->item.iImage = cr->iconIndex;
                }

                if (pdi->item.mask & LVIF_TEXT) {
                    static thread_local wchar_t buf[1024];
                    buf[0] = L'\0';
                    const std::wstring* src = nullptr;
                    switch (iSub) {
                    case 0: src = &cr->name; break;
                    case 1: {
                        static thread_local std::wstring archiveText;
                        archiveText = GetArchiveColumnText(s, cr);
                        src = &archiveText;
                        break;
                    }
                    case 2: src = &cr->entryPath; break;
                    case 3: src = &cr->sizeStr; break;
                    case 4: src = &cr->origSizeStr; break;
                    case 5: src = &cr->modifiedTimeStr; break;
                    }
                    if (src) {
                        wcsncpy_s(buf, src->c_str(), _TRUNCATE);
                    }
                    pdi->item.pszText = buf;
                }
                return 0;
            }
            // 列头点击排序：通过数据库 ORDER BY 重新查询 rowid 列表
            if (hdr->code == LVN_COLUMNCLICK) {
                LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
                int clickedCol = nmlv->iSubItem;
                if (clickedCol == s->sortColumn) {
                    s->sortAscending = !s->sortAscending;
                } else {
                    s->sortColumn = clickedCol;
                    s->sortAscending = true;
                }
                // 异步重新查询（带新排序条件）
                LoadRowsFromDbAndRefreshAsync(hWnd, s);
                return 0;
            }
            if (hdr->code == NM_DBLCLK) {
                MessageBoxW(hWnd, LS_(s, IDS_DBLCLICK_PLACEHOLDER).c_str(), L"Info", MB_OK);
                return 0;
            }
            if (hdr->code == NM_RCLICK) {
                // 获取当前选中项索引
                int iItem = ListView_GetNextItem(s->hList, -1, LVNI_SELECTED);
                if (iItem < 0) return 0;

                // 构建右键菜单
                HMENU hMenu = CreatePopupMenu();
                if (!hMenu) return 0;
                std::wstring openFolderText = LS_(s, IDS_CTX_OPEN_FOLDER) + L"(&I)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPEN_FOLDER,
                    openFolderText.c_str());
                std::wstring openArchiveText = LS_(s, IDS_CTX_OPEN_ARCHIVE) + L"(&O)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPEN_ARCHIVE,
                    openArchiveText.c_str());
                std::wstring extractText = LS_(s, IDS_CTX_EXTRACT) + L"(&E)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXTRACT,
                    extractText.c_str());
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                std::wstring copyArchiveText = LS_(s, IDS_CTX_COPY_ARCHIVE) + L"(&C)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPY_ARCHIVE,
                    copyArchiveText.c_str());
                std::wstring copyNameText = LS_(s, IDS_CTX_COPY_NAME) + L"(&N)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPY_NAME,
                    copyNameText.c_str());
                std::wstring copyEntryPathText = LS_(s, IDS_CTX_COPY_ENTRY_PATH) + L"(&P)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPY_ENTRY_PATH,
                    copyEntryPathText.c_str());
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                std::wstring propertiesText = LS_(s, IDS_CTX_PROPERTIES) + L"(&R)";
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_PROPERTIES,
                    propertiesText.c_str());

                // 使用鼠标位置弹出菜单
                POINT pt{};
                GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(hMenu);
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW) {
                LPNMLVCUSTOMDRAW lvcd = (LPNMLVCUSTOMDRAW)lParam;
                switch (lvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                    int iItem = (int)lvcd->nmcd.dwItemSpec;
                    int iSubItem = lvcd->iSubItem;

                    // 列 0/1/2（名称、归档文件、内部路径）使用自绘加粗匹配
                    if (iSubItem == 0 || iSubItem == 1 || iSubItem == 2) {
                        std::wstring text;
                        int iconIdx = 0;
                        {
                            int64_t rowId = 0;
                            {
                                std::lock_guard<std::mutex> lk(s->rowsMutex);
                                if (iItem >= 0 && iItem < (int)s->rowIds.size()) {
                                    rowId = s->rowIds[iItem];
                                }
                            }
                            if (rowId > 0) {
                                const CachedRow* cr = s->rowCache.Get(rowId);
                                if (cr) {
                                    switch (iSubItem) {
                                    case 0: text = cr->name; iconIdx = cr->iconIndex; break;
                                    case 1: text = GetArchiveColumnText(s, cr); break;
                                    case 2: text = cr->entryPath; break;
                                    }
                                } else if (!s->needsListRetry) {
                                    s->needsListRetry = true;
                                    SetTimer(hWnd, IDT_LIST_RETRY, 500, nullptr);
                                }
                            }
                        }

                        std::wstring filter = GetSearchFilter(s);

                        // 列0：获取图标区域和文本区域分别绘制
                        RECT rcIcon{}, rcText{};
                        if (iSubItem == 0) {
                            ListView_GetItemRect(s->hList, iItem, &rcIcon, LVIR_ICON);
                            ListView_GetItemRect(s->hList, iItem, &rcText, LVIR_LABEL);
                        } else {
                            ListView_GetSubItemRect(s->hList, iItem, iSubItem, LVIR_BOUNDS, &rcText);
                        }

                        // 绘制整行背景（列0需要覆盖图标+文本区域）
                        RECT rcFill = (iSubItem == 0) ? RECT{ rcIcon.left, rcIcon.top, rcText.right, rcText.bottom } : rcText;
                        bool selected = (ListView_GetItemState(s->hList, iItem, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);

                        if (selected) {
                            FillRect(lvcd->nmcd.hdc, &rcFill, GetSysColorBrush(COLOR_HIGHLIGHT));
                        } else {
                            FillRect(lvcd->nmcd.hdc, &rcFill, GetSysColorBrush(COLOR_WINDOW));
                        }

                        // 列0：在图标区域绘制文件类型图标
                        if (iSubItem == 0 && s->iconCache.GetSysSmallIcons()) {
                            int iconX = rcIcon.left;
                            int iconY = rcIcon.top + ((rcIcon.bottom - rcIcon.top) - 16) / 2;
                            ImageList_Draw(s->iconCache.GetSysSmallIcons(), iconIdx, lvcd->nmcd.hdc,
                                iconX, iconY, ILD_TRANSPARENT);
                        }

                        DrawTextWithBoldMatch(lvcd->nmcd.hdc, rcText, text, filter, textColor, s->hNormalFont, s->hBoldFont);
                        return CDRF_SKIPDEFAULT;
                    }
                    // 列3/4（压缩大小、原始大小）：强制使用普通字体，防止继承自绘时的粗体
                    if (s->hNormalFont) {
                        SelectObject(lvcd->nmcd.hdc, s->hNormalFont);
                    }
                    return CDRF_NEWFONT;
                }
                }
            }
        }
        break;
    }

    case WM_APP_EXTRACT_DONE: {
        auto* res = (ExtractResult*)lParam;
        if (!res) return 0;
        if (res->success) {
            MessageBoxW(hWnd, LS_(s, IDS_CTX_EXTRACT_OK).c_str(),
                LS_(s, IDS_CTX_EXTRACT_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
        } else {
            // 将 errorMsg 转为宽字符串显示
            std::wstring wmsg;
            if (!res->errorMsg.empty()) {
                int need = MultiByteToWideChar(CP_UTF8, 0, res->errorMsg.c_str(), -1, nullptr, 0);
                if (need > 0) {
                    wmsg.resize(need - 1);
                    MultiByteToWideChar(CP_UTF8, 0, res->errorMsg.c_str(), -1, wmsg.data(), need);
                }
            }
            std::wstring msg = LS_(s, IDS_CTX_EXTRACT_FAIL);
            if (!wmsg.empty()) msg += L"\n" + wmsg;
            MessageBoxW(hWnd, msg.c_str(),
                LS_(s, IDS_CTX_EXTRACT_TITLE).c_str(), MB_OK | MB_ICONERROR);
        }
        delete res;
        return 0;
    }

    case WM_APP_TRAY:
        // 托盘图标回调消息
        switch (lParam) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            // 左键点击/双击：显示窗口
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            break;
        case WM_RBUTTONUP:
            // 右键点击：显示托盘菜单
            ShowTrayMenu(hWnd, s);
            break;
        }
        return 0;

    case WM_DESTROY: // 窗口销毁：移除托盘图标、停止索引、关闭数据库、释放字体资源
        LOG_INFO(L"WM_DESTROY");
        SaveUiState(hWnd, s);
        s->shuttingDown.store(true);
        ++s->loadGeneration;
        RemoveTrayIcon();
        KillTimer(hWnd, IDT_STATUSBAR_TIMER);
        KillTimer(hWnd, IDT_SEARCH_DEBOUNCE);
        KillTimer(hWnd, IDT_LIST_RETRY);
        s->indexer.Stop();
        JoinAsyncThreads(s);
        DrainAsyncResultMessages(hWnd);
        HideArchiveTooltip(s);
        if (s->hArchiveTooltip) {
            DestroyWindow(s->hArchiveTooltip);
            s->hArchiveTooltip = nullptr;
        }
        s->rowCache.Close();
        if (s->hSearchFont) { DeleteObject(s->hSearchFont); s->hSearchFont = nullptr; }
        if (s->hNormalFont) { DeleteObject(s->hNormalFont); s->hNormalFont = nullptr; }
        if (s->hBoldFont) { DeleteObject(s->hBoldFont); s->hBoldFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
