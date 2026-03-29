#include "main_window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

#include "logger.h"
#include "resource.h"
#include "string_utils.h"
#include "tray_icon.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

const wchar_t* S(HINSTANCE hInstance, UINT id) {
    const wchar_t* p = nullptr;
    int len = LoadStringW(hInstance, id, (LPWSTR)&p, 0);
    return (len > 0 && p) ? p : L"";
}

std::wstring LS(HINSTANCE hInstance, UINT id) {
    const wchar_t* p = nullptr;
    int len = LoadStringW(hInstance, id, (LPWSTR)&p, 0);
    return (len > 0 && p) ? std::wstring(p, len) : std::wstring();
}

// ── Spinner 自绘窗口类名 ──
static constexpr wchar_t kSpinnerClass[] = L"EveryArchiveSpinner";

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
    return LS(s->hInstance, id);
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

// 创建主窗口菜单栏
static HMENU CreateMainMenu(MainWindowState* s) {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, LS_(s, IDS_MENU_FILE_EXIT).c_str());

    HMENU hEdit = CreatePopupMenu();
    AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY, LS_(s, IDS_MENU_EDIT_COPY).c_str());

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH, LS_(s, IDS_MENU_VIEW_REFRESH).c_str());
    AppendMenuW(hView, MF_STRING, IDM_VIEW_STOP, LS_(s, IDS_MENU_VIEW_STOP).c_str());

    HMENU hSearch = CreatePopupMenu();
    AppendMenuW(hSearch, MF_STRING, IDM_SEARCH_FIND, LS_(s, IDS_MENU_SEARCH_FIND).c_str());

    HMENU hBookmark = CreatePopupMenu();
    AppendMenuW(hBookmark, MF_STRING, IDM_BOOKMARK_ADD, LS_(s, IDS_MENU_BOOKMARK_ADD).c_str());

    HMENU hTools = CreatePopupMenu();
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_OPTIONS, LS_(s, IDS_MENU_TOOLS_OPTIONS).c_str());

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, LS_(s, IDS_MENU_HELP_ABOUT).c_str());

    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, LS_(s, IDS_MENU_FILE).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hEdit, LS_(s, IDS_MENU_EDIT).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, LS_(s, IDS_MENU_VIEW).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hSearch, LS_(s, IDS_MENU_SEARCH).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hBookmark, LS_(s, IDS_MENU_BOOKMARK).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hTools, LS_(s, IDS_MENU_TOOLS).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, LS_(s, IDS_MENU_HELP).c_str());

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

    std::wstring colName = LS_(s, IDS_COL_NAME);
    col.pszText = const_cast<LPWSTR>(colName.c_str());
    col.cx = 200;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    // 归档文件路径列
    std::wstring colArchive = LS_(s, IDS_COL_ARCHIVE);
    col.pszText = const_cast<LPWSTR>(colArchive.c_str());
    col.cx = 280;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    // 归档内部文件路径列
    std::wstring colPath = LS_(s, IDS_COL_PATH);
    col.pszText = const_cast<LPWSTR>(colPath.c_str());
    col.cx = 280;
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    // 压缩大小列（右对齐）
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt = LVCFMT_RIGHT;
    std::wstring colCompSize = LS_(s, IDS_COL_COMPRESSED_SIZE);
    col.pszText = const_cast<LPWSTR>(colCompSize.c_str());
    col.cx = 100;
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);

    // 原始大小列（右对齐）
    std::wstring colOrigSize = LS_(s, IDS_COL_ORIGINAL_SIZE);
    col.pszText = const_cast<LPWSTR>(colOrigSize.c_str());
    col.cx = 100;
    col.iSubItem = 4;
    ListView_InsertColumn(hList, 4, &col);
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

// 更新 ListView 列头文本，在当前排序列后附加箭头指示符（▲/▼）
static void UpdateColumnHeaders(MainWindowState* s) {
    static const UINT colIds[] = {
        IDS_COL_NAME, IDS_COL_ARCHIVE, IDS_COL_PATH,
        IDS_COL_COMPRESSED_SIZE, IDS_COL_ORIGINAL_SIZE
    };

    for (int i = 0; i < 5; ++i) {
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

// 根据主窗口大小重新布局子控件（搜索框、ListView、状态栏）
static void LayoutChildren(HWND hWnd, MainWindowState* s) {
    RECT rc{};
    GetClientRect(hWnd, &rc);

    const UINT dpi = GetWindowDpi(hWnd);
    const int margin = ScaleDpi(6, dpi);
    const int editH  = ScaleDpi(24, dpi);

    RECT statusRect{};
    if (s->hStatusBar) {
        SendMessageW(s->hStatusBar, WM_SIZE, 0, 0);
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

    std::thread([hWnd, filter = std::move(filter), dbPath = std::move(dbPath), sortCol, sortAsc]() {
        auto* result = new AsyncLoadResult();

        std::wstring err;
        Database db;
        if (!db.Open(dbPath, &err)) {
            LOG_WARN(L"Database::Open failed: %s", err.c_str());
            PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result);
            return;
        }

        db.CreateEntriesTable(&err);

        // 只查询 rowid 列表（36万条 ≈ 3MB，而非 1.4GB 的完整数据）
        if (!db.QueryEntryIds(filter, sortCol, sortAsc, &result->rowIds, &err)) {
            LOG_WARN(L"QueryEntryIds failed: %s", err.c_str());
            PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result);
            return;
        }

        result->entryCount = (int64_t)result->rowIds.size();
        result->archiveCount = db.GetArchiveCount();

        PostMessageW(hWnd, WM_APP_ROWS_READY, 0, (LPARAM)result);
    }).detach();
}

// 更新底部状态栏（显示索引状态、文件数量和归档文件数量）
static void UpdateStatusBar(MainWindowState* s) {
    if (!s->hStatusBar) return;

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

    bool running = s->indexer.IsRunning();

    // 控制 Spinner 的显示/隐藏，并定位到状态栏右侧
    if (s->hProgress) {
        bool isVisible = (GetWindowLongW(s->hProgress, GWL_STYLE) & WS_VISIBLE) != 0;
        if (running && !isVisible) {
            // 定位到状态栏右侧
            RECT sbRect{};
            GetClientRect(s->hStatusBar, &sbRect);
            const int size   = sbRect.bottom - sbRect.top - 2;
            const int pbX    = sbRect.right - size - 18; // 留出 sizegrip 空间
            const int pbY    = 1;
            MoveWindow(s->hProgress, pbX, pbY, size, size, FALSE);
            ShowWindow(s->hProgress, SW_SHOW);
            // 启动动画定时器（每 40ms 旋转一帧），挂在主窗口上
            SetTimer(GetParent(s->hStatusBar), IDT_SPINNER_ANIM, 40, nullptr);
        } else if (!running && isVisible) {
            KillTimer(GetParent(s->hStatusBar), IDT_SPINNER_ANIM);
            ShowWindow(s->hProgress, SW_HIDE);
        }
    }

    std::wstring statusText;
    if (running) {
        statusText = LS_(s, IDS_STATUS_PROCESSING) + L" | " + LS_(s, IDS_STATUS_FILE_COUNT) + L": " + fileCountStr +
                     L" | " + LS_(s, IDS_STATUS_ARCHIVE_COUNT) + L": " + archiveCountStr;
    } else {
        statusText = LS_(s, IDS_STATUS_READY) + L" | " + LS_(s, IDS_STATUS_FILE_COUNT) + L": " + fileCountStr +
                     L" | " + LS_(s, IDS_STATUS_ARCHIVE_COUNT) + L": " + archiveCountStr;
    }

    SendMessageW(s->hStatusBar, SB_SETTEXTW, 0, (LPARAM)statusText.c_str());
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

        s->indexer.EnsureDatabaseReady();
        // 异步加载数据库数据，避免阻塞 UI 线程
        LoadRowsFromDbAndRefreshAsync(hWnd, s);

        s->indexer.Start(hWnd);

        // 创建系统托盘图标
        AddTrayIcon(hWnd, IDI_TRAY_ICON, WM_APP_TRAY);
        return 0;
    }

    case WM_CLOSE:
        // 关闭窗口只是隐藏到托盘，不退出程序
        if (!s->forceQuit) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
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
            return 0;
        case IDM_SEARCH_FIND:
            LoadRowsFromDbAndRefreshAsync(hWnd, s);
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd, LS_(s, IDS_ABOUT_TEXT).c_str(), LS_(s, IDS_ABOUT_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            return 0;
        }
    }

    case WM_APP_INDEX_DONE:
        UpdateStatusBar(s);
        LOG_INFO(L"WM_APP_INDEX_DONE");
        return 0;

    case WM_APP_DB_REFRESH:
        LoadRowsFromDbAndRefreshAsync(hWnd, s);
        return 0;

    case WM_APP_ROWS_READY: {
        // 后台线程查询完成，交换 rowid 列表到 UI 线程
        auto* result = (AsyncLoadResult*)lParam;
        if (result) {
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

    case WM_NOTIFY: { // ListView 通知：双击事件 + NM_CUSTOMDRAW 自绘（关键词加粗）
        LPNMHDR hdr = (LPNMHDR)lParam;
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
                if (!cr) return 0;

                if ((pdi->item.mask & LVIF_IMAGE) && iSub == 0) {
                    pdi->item.iImage = cr->iconIndex;
                }

                if (pdi->item.mask & LVIF_TEXT) {
                    static thread_local wchar_t buf[1024];
                    buf[0] = L'\0';
                    const std::wstring* src = nullptr;
                    switch (iSub) {
                    case 0: src = &cr->name; break;
                    case 1: src = &cr->archivePath; break;
                    case 2: src = &cr->entryPath; break;
                    case 3: src = &cr->sizeStr; break;
                    case 4: src = &cr->origSizeStr; break;
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
                                    case 1: text = cr->archivePath; break;
                                    case 2: text = cr->entryPath; break;
                                    }
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
            ShowTrayMenu(hWnd, s->hInstance);
            break;
        }
        return 0;

    case WM_DESTROY: // 窗口销毁：移除托盘图标、停止索引、关闭数据库、释放字体资源
        LOG_INFO(L"WM_DESTROY");
        RemoveTrayIcon();
        KillTimer(hWnd, IDT_STATUSBAR_TIMER);
        KillTimer(hWnd, IDT_SEARCH_DEBOUNCE);
        s->indexer.Stop();
        s->rowCache.Close();
        if (s->hSearchFont) { DeleteObject(s->hSearchFont); s->hSearchFont = nullptr; }
        if (s->hNormalFont) { DeleteObject(s->hNormalFont); s->hNormalFont = nullptr; }
        if (s->hBoldFont) { DeleteObject(s->hBoldFont); s->hBoldFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
