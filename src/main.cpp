#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "resource.h"
#include "logger.h"
#include "parser/zip_archive_parser.h"

#include "file_scanner.h"
#include "database.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

// 启用 ComCtl32 v6 视觉样式（PBS_MARQUEE 进度条需要）
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


static constexpr wchar_t kAppClassName[] = L"EveryArchiveMainWindow";
static constexpr wchar_t kAppTitle[] = L"EveryArchive";
static HINSTANCE g_hInstance = nullptr;

// 从 STRINGTABLE 资源加载本地化字符串
static const wchar_t* S(UINT id) {
    // LoadStringW with nBufferMax=0 returns a pointer to the resource string (read-only)
    const wchar_t* p = nullptr;
    int len = LoadStringW(g_hInstance, id, (LPWSTR)&p, 0);
    return (len > 0 && p) ? p : L"";
}

// S() 返回的指针指向只读资源段，不以 '\0' 结尾，需要拷贝到 std::wstring 使用
static std::wstring LS(UINT id) {
    const wchar_t* p = nullptr;
    int len = LoadStringW(g_hInstance, id, (LPWSTR)&p, 0);
    return (len > 0 && p) ? std::wstring(p, len) : std::wstring();
}

// ListView 行缓存项（按需从数据库加载，缓存可见行数据）
struct CachedRow {
    std::wstring name;
    std::wstring archivePath;
    std::wstring entryPath;
    std::wstring sizeStr;       // 格式化后的压缩大小
    std::wstring origSizeStr;   // 格式化后的原始大小
    int iconIndex = 0;
};

// ── 全局 UI 控件句柄 ──
static HWND g_hSearch = nullptr;      // 搜索输入框
static HWND g_hList = nullptr;        // 结果 ListView
static HWND g_hStatusBar = nullptr;   // 底部状态栏
static HWND g_hProgress = nullptr;    // 状态栏内的 Marquee 进度条
static WNDPROC g_EditOldProc = nullptr; // 搜索框原始窗口过程（子类化用）
static HFONT g_hSearchFont = nullptr; // 搜索框字体（非粗体）
static HFONT g_hNormalFont = nullptr; // ListView 普通字体
static HFONT g_hBoldFont = nullptr;   // ListView 加粗字体（高亮匹配文本）

// ── ListView 数据源（纯虚拟列表：只存 rowid 列表，按需查询行数据）──
static std::vector<int64_t> g_rowIds;       // entries 表的 rowid 列表（~3MB for 360K rows）
static std::mutex g_rowsMutex;
static int64_t g_totalEntryCount = 0;       // 当前 filter 下的条目总数

// ── 行数据 LRU 缓存（避免每次 LVN_GETDISPINFO 都查询数据库）──
static std::unordered_map<int64_t, CachedRow> g_rowCache;  // rowid → CachedRow
static std::deque<int64_t> g_rowCacheLru;                  // LRU 队列（最近使用的在前）
static constexpr size_t kRowCacheMaxSize = 2000;           // 缓存最多 2000 行
static Database g_cacheDb;                                 // 缓存专用数据库连接（UI 线程使用）
static bool g_cacheDbOpen = false;

// ── 列头排序状态 ──
static int g_sortColumn = -1;       // 当前排序列索引，-1 表示未排序
static bool g_sortAscending = true; // true=正序，false=倒序

// ── 文件图标缓存：按扩展名缓存系统图标索引 ──
static HIMAGELIST g_hSysSmallIcons = nullptr;
static std::unordered_map<std::wstring, int> g_iconCache;

// 根据文件名获取系统图标索引（按扩展名缓存，避免重复查询）
static int GetFileIconIndex(const std::wstring& fileName) {
    // 提取扩展名（包含点，如 ".xml"）并转小写
    std::wstring ext;
    size_t dotPos = fileName.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        ext = fileName.substr(dotPos);
        for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    }

    // 查找缓存
    auto it = g_iconCache.find(ext);
    if (it != g_iconCache.end()) {
        return it->second;
    }

    // 使用 SHGetFileInfo 按扩展名获取系统图标索引（SHGFI_USEFILEATTRIBUTES 无需文件实际存在）
    SHFILEINFOW sfi{};
    std::wstring fakeName = L"file" + ext;
    DWORD_PTR ret = SHGetFileInfoW(
        fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

    int idx = (ret != 0) ? sfi.iIcon : 0;
    g_iconCache[ext] = idx;
    return idx;
}

// ── 后台索引线程相关 ──
static std::atomic_bool g_indexCancel{ false };  // 取消标志
static std::atomic_bool g_indexRunning{ false }; // 运行状态
static std::thread g_indexThread;                // 索引工作线程
static HWND g_mainHwnd = nullptr;
static bool g_forceQuit = false;                 // true=真正退出程序，false=关闭窗口仅隐藏到托盘

// ── 系统托盘图标 ──
static NOTIFYICONDATAW g_nid{};

static void AddTrayIcon(HWND hWnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = IDI_TRAY_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"EveryArchive");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, LS(IDS_TRAY_SHOW).c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, LS(IDS_TRAY_EXIT).c_str());

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

// ── 异步加载结果（后台线程只查询 rowid 列表，传递到 UI 线程）──
struct AsyncLoadResult {
    std::vector<int64_t> rowIds;
    int64_t archiveCount = 0;
    int64_t entryCount = 0;
};
// 缓存的归档文件数量（避免 UpdateStatusBar 每次都查询数据库）
static std::atomic<int64_t> g_cachedArchiveCount{ 0 };

// ── 数据库 ──
static Database g_database;   // 全局数据库连接（用于解析归档条目）
static std::wstring g_dbPath; // 数据库文件路径

// 确保数据库文件存在并创建所需的表
static bool EnsureDatabaseReady() {
    const DWORD attr = GetFileAttributesW(g_dbPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(g_dbPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }

    std::wstring err;
    Database db;
    if (!db.Open(g_dbPath, &err)) {
        LOG_WARN(L"Database::Open failed: %s", err.c_str());
        return false;
    }
    if (!db.CreateArchivesTable(&err)) {
        LOG_WARN(L"CreateArchivesTable failed: %s", err.c_str());
        return false;
    }
    if (!db.CreateEntriesTable(&err)) {
        LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
        return false;
    }
    return true;
}

// 提升进程权限（用于 USN Journal 扫描需要的 SeManageVolume / SeBackup）
static bool EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        CloseHandle(hToken);
        return false;
    }

    const DWORD err = GetLastError();
    CloseHandle(hToken);
    return err != ERROR_NOT_ALL_ASSIGNED;
}

// 创建主窗口菜单栏
static HMENU CreateMainMenu() {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, LS(IDS_MENU_FILE_EXIT).c_str());

    HMENU hEdit = CreatePopupMenu();
    AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY, LS(IDS_MENU_EDIT_COPY).c_str());

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH, LS(IDS_MENU_VIEW_REFRESH).c_str());
    AppendMenuW(hView, MF_STRING, IDM_VIEW_STOP, LS(IDS_MENU_VIEW_STOP).c_str());

    HMENU hSearch = CreatePopupMenu();
    AppendMenuW(hSearch, MF_STRING, IDM_SEARCH_FIND, LS(IDS_MENU_SEARCH_FIND).c_str());

    HMENU hBookmark = CreatePopupMenu();
    AppendMenuW(hBookmark, MF_STRING, IDM_BOOKMARK_ADD, LS(IDS_MENU_BOOKMARK_ADD).c_str());

    HMENU hTools = CreatePopupMenu();
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_OPTIONS, LS(IDS_MENU_TOOLS_OPTIONS).c_str());

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, LS(IDS_MENU_HELP_ABOUT).c_str());

    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, LS(IDS_MENU_FILE).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hEdit, LS(IDS_MENU_EDIT).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, LS(IDS_MENU_VIEW).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hSearch, LS(IDS_MENU_SEARCH).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hBookmark, LS(IDS_MENU_BOOKMARK).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hTools, LS(IDS_MENU_TOOLS).c_str());
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, LS(IDS_MENU_HELP).c_str());

    return hMenuBar;
}

static std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) {
        ch = (wchar_t)towlower(ch);
    }
    return s;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out;
    out.resize((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWString(const char* s) {
    if (!s) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring w;
    w.resize((size_t)(needed - 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), needed);
    return w;
}

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    PathRemoveFileSpecW(path);
    return path;
}

// 获取搜索框中的过滤文本（转小写，去除通配符 *）
static std::wstring GetSearchFilter() {
    wchar_t buf[1024]{};
    if (g_hSearch) {
        GetWindowTextW(g_hSearch, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    }
    std::wstring f = buf;
    f = ToLower(f);
    for (auto& ch : f) {
        if (ch == L'*') ch = 0;
    }
    f.erase(std::remove(f.begin(), f.end(), 0), f.end());
    return f;
}

// 判断文件名是否匹配当前搜索过滤条件
static bool PassesFilter(const std::wstring& name) {
    const std::wstring filter = GetSearchFilter();
    if (filter.empty()) return true;
    std::wstring n = ToLower(name);
    return n.find(filter) != std::wstring::npos;
}

// 初始化 ListView 列头（名称、路径、压缩大小、原始大小）
static void SetupListColumns(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    HWND hHeader = ListView_GetHeader(hList);
    if (hHeader) {
        LONG style = GetWindowLongW(hHeader, GWL_STYLE);
        SetWindowLongW(hHeader, GWL_STYLE, style | HDS_FLAT);
    }

    // 获取系统小图标 ImageList 并关联到 ListView（用于显示文件类型图标）
    SHFILEINFOW sfi{};
    g_hSysSmallIcons = (HIMAGELIST)SHGetFileInfoW(
        L"", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (g_hSysSmallIcons) {
        ListView_SetImageList(hList, g_hSysSmallIcons, LVSIL_SMALL);
    }

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    std::wstring colName = LS(IDS_COL_NAME);
    col.pszText = const_cast<LPWSTR>(colName.c_str());
    col.cx = 200;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    // 归档文件路径列
    std::wstring colArchive = LS(IDS_COL_ARCHIVE);
    col.pszText = const_cast<LPWSTR>(colArchive.c_str());
    col.cx = 280;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    // 归档内部文件路径列
    std::wstring colPath = LS(IDS_COL_PATH);
    col.pszText = const_cast<LPWSTR>(colPath.c_str());
    col.cx = 280;
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    // 压缩大小列（右对齐）
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt = LVCFMT_RIGHT;
    std::wstring colCompSize = LS(IDS_COL_COMPRESSED_SIZE);
    col.pszText = const_cast<LPWSTR>(colCompSize.c_str());
    col.cx = 100;
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);

    // 原始大小列（右对齐）
    std::wstring colOrigSize = LS(IDS_COL_ORIGINAL_SIZE);
    col.pszText = const_cast<LPWSTR>(colOrigSize.c_str());
    col.cx = 100;
    col.iSubItem = 4;
    ListView_InsertColumn(hList, 4, &col);
}

static std::wstring GetEntryNameFromPath(const std::wstring& path) {
    if (path.empty()) return path;
    size_t pos = path.find_last_of(L"/\\");
    if (pos == std::wstring::npos) return path;
    if (pos + 1 >= path.size()) return L"";
    return path.substr(pos + 1);
}

// 解析归档文件列表，提取内部条目并写入数据库
static void ParseArchivesToEntries(const std::vector<ArchiveFile_t>& archives) {
    std::wstring err;
    if (!g_database.Open(g_dbPath, &err)) {
        LOG_WARN(L"Database::Open failed: %s", err.c_str());
        return;
    }

    if (!g_database.CreateEntriesTable(&err)) {
        LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
        return;
    }

    for (const auto& a : archives) {
        if (g_indexCancel.load()) return;

        EveryArchive::ZipArchiveParser parser;
        std::string perr;
        if (!parser.Open(a.filePath, &perr)) {
            LOG_WARN(L"ZipArchiveParser::Open failed: %s", Utf8ToWString(perr.c_str()).c_str());
            continue;
        }

        std::vector<EveryArchive::ArchiveEntry> parsed;
        if (!parser.ListEntries(&parsed, &perr)) {
            LOG_WARN(L"ZipArchiveParser::ListEntries failed: %s", Utf8ToWString(perr.c_str()).c_str());
            parser.Close();
            continue;
        }
        parser.Close();

        int64_t archiveId = g_database.GetArchiveIdByPath(a.filePath);
        if (archiveId < 0) {
            LOG_WARN(L"GetArchiveIdByPath failed for: %s", a.filePath.c_str());
            continue;
        }

        std::vector<ArchiveEntry_t> entries;
        entries.reserve(parsed.size());
        for (const auto& e : parsed) {
            if (e.is_directory) continue;

            ArchiveEntry_t out;
            out.archiveId = archiveId;
            out.entryPath = e.name_w.empty() ? Utf8ToWString(e.name.c_str()) : e.name_w;
            out.compressed_size = e.compressed_size;
            out.uncompressed_size = e.uncompressed_size;
            entries.push_back(std::move(out));
        }

        if (!g_database.DeleteEntriesByArchiveId(archiveId, &err)) {
            LOG_WARN(L"DeleteEntriesByArchiveId failed: %s", err.c_str());
        }
        if (!g_database.InsertEntriesBatch(entries, &err)) {
            LOG_WARN(L"InsertEntriesBatch failed: %s", err.c_str());
        }
    }
}

// 前向声明（GetCachedRow 需要调用）
static std::wstring FormatSizeULongLong(ULONGLONG v);

// 清空行数据缓存（排序或搜索条件变化时调用）
static void ClearRowCache() {
    g_rowCache.clear();
    g_rowCacheLru.clear();
}

// 确保缓存数据库连接已打开
static void EnsureCacheDbOpen() {
    if (!g_cacheDbOpen) {
        std::wstring err;
        g_cacheDbOpen = g_cacheDb.Open(g_dbPath, &err);
    }
}

// 按 rowid 从 LRU 缓存中获取行数据，缓存未命中时从数据库查询
static const CachedRow* GetCachedRow(int64_t rowId) {
    // 缓存命中
    auto it = g_rowCache.find(rowId);
    if (it != g_rowCache.end()) {
        return &it->second;
    }

    // 缓存未命中：从数据库查询
    EnsureCacheDbOpen();
    if (!g_cacheDbOpen) return nullptr;

    ArchiveEntry_t entry;
    if (!g_cacheDb.QueryEntryById(rowId, &entry)) {
        return nullptr;
    }

    // 构建缓存项
    CachedRow cr;
    cr.name = GetEntryNameFromPath(entry.entryPath);
    cr.archivePath = entry.archivePath;
    cr.entryPath = entry.entryPath;
    cr.sizeStr = FormatSizeULongLong((ULONGLONG)entry.compressed_size);
    cr.origSizeStr = FormatSizeULongLong((ULONGLONG)entry.uncompressed_size);
    cr.iconIndex = GetFileIconIndex(cr.name);

    // 插入缓存并维护 LRU
    auto [inserted, _] = g_rowCache.emplace(rowId, std::move(cr));
    g_rowCacheLru.push_front(rowId);

    // 超出容量时淘汰最旧的
    while (g_rowCache.size() > kRowCacheMaxSize && !g_rowCacheLru.empty()) {
        int64_t oldest = g_rowCacheLru.back();
        g_rowCacheLru.pop_back();
        g_rowCache.erase(oldest);
    }

    return &inserted->second;
}

// 更新 ListView 列头文本，在当前排序列后附加箭头指示符（▲/▼）
static void UpdateColumnHeaders() {
    static const UINT colIds[] = {
        IDS_COL_NAME, IDS_COL_ARCHIVE, IDS_COL_PATH,
        IDS_COL_COMPRESSED_SIZE, IDS_COL_ORIGINAL_SIZE
    };

    for (int i = 0; i < 5; ++i) {
        std::wstring text = LS(colIds[i]);
        if (i == g_sortColumn) {
            text += g_sortAscending ? LS(IDS_SORT_ASC) : LS(IDS_SORT_DESC);
        }

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_SetColumn(g_hList, i, &col);
    }
}

// 刷新 ListView 显示内容（虚拟列表模式：只需设置条目数量，数据由 LVN_GETDISPINFO 按需提供）
static void RefreshList() {
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        count = (int)g_rowIds.size();
    }
    ListView_SetItemCountEx(g_hList, count, LVSICF_NOSCROLL);
    InvalidateRect(g_hList, nullptr, TRUE);
}

// 根据主窗口大小重新布局子控件（搜索框、ListView、状态栏）
static void LayoutChildren(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);

    const int margin = 6;
    const int editH = 24;

    RECT statusRect{};
    if (g_hStatusBar) {
        SendMessageW(g_hStatusBar, WM_SIZE, 0, 0);
        GetWindowRect(g_hStatusBar, &statusRect);
    }
    const int statusH = statusRect.bottom - statusRect.top;

    int x = margin;
    int y = margin;
    int w = (rc.right - rc.left) - margin * 2;
    int h = (rc.bottom - rc.top) - margin * 2 - statusH;

    MoveWindow(g_hSearch, x, y, w, editH, TRUE);

    y += editH + margin;
    h -= editH + margin;

    MoveWindow(g_hList, x, y, w, h, TRUE);
}

// 为整数字符串添加千位分隔符（如 "1234567" → "1,234,567"）
static std::wstring AddThousandsSeparator(const std::wstring& num) {
    std::wstring result;
    int count = 0;
    for (int i = (int)num.size() - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) {
            result.insert(result.begin(), L',');
        }
        result.insert(result.begin(), num[i]);
        ++count;
    }
    return result;
}

// 将字节数格式化为 KB 单位的字符串（>=1KB 取整数并添加千位分隔符，<1KB 保留两位小数）
static std::wstring FormatSizeULongLong(ULONGLONG v) {
    double kb = (double)v / 1024.0;
    if (kb >= 1.0) {
        // >=1KB：取整后添加千位分隔符
        ULONGLONG kbInt = (ULONGLONG)(kb + 0.5);
        std::wstring numStr = std::to_wstring(kbInt);
        return AddThousandsSeparator(numStr) + L" KB";
    } else {
        // <1KB：保留两位小数
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.2f KB", kb);
    return buf;
    }
}

// 将 UTC FILETIME 转换为本地时间字符串
static std::wstring FormatFileTimeLocal(const FILETIME& ftUtc) {
    FILETIME ftLocal{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal)) return L"";
    if (!FileTimeToSystemTime(&ftLocal, &st)) return L"";

    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u/%u/%u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

static FILETIME U64ToFileTime(uint64_t v) {
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)((v >> 32) & 0xFFFFFFFFu);
    return ft;
}

// 异步加载：在后台线程中只查询 rowid 列表（内存极低），完成后通知 UI 线程
static void LoadRowsFromDbAndRefreshAsync(HWND hWnd) {
    // 捕获当前搜索条件和排序状态（在 UI 线程中获取）
    std::wstring filter = GetSearchFilter();
    std::wstring dbPath = g_dbPath;
    int sortCol = g_sortColumn;
    bool sortAsc = g_sortAscending;

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


// 停止后台索引线程（设置取消标志并等待线程结束）
static void StopIndexing() {
    LOG_INFO(L"StopIndexing requested");
    g_indexCancel.store(true);
    if (g_indexThread.joinable()) {
        g_indexThread.join();
    }
    g_indexRunning.store(false);
    LOG_INFO(L"StopIndexing done");
}

// 解析单个归档文件并将条目写入数据库（先删除旧条目再插入新条目）
static void ParseAndStoreArchive(Database& db, const ArchiveFile_t& a) {
    EveryArchive::ZipArchiveParser parser;
    std::string perr;
    if (!parser.Open(a.filePath, &perr)) {
        LOG_WARN(L"ZipArchiveParser::Open failed: %s", Utf8ToWString(perr.c_str()).c_str());
        return;
    }

    std::vector<EveryArchive::ArchiveEntry> parsed;
    if (!parser.ListEntries(&parsed, &perr)) {
        LOG_WARN(L"ZipArchiveParser::ListEntries failed: %s", Utf8ToWString(perr.c_str()).c_str());
        parser.Close();
        return;
    }
    parser.Close();

    int64_t archiveId = db.GetArchiveIdByPath(a.filePath);
    if (archiveId < 0) {
        LOG_WARN(L"GetArchiveIdByPath failed for: %s", a.filePath.c_str());
        return;
    }


    std::vector<ArchiveEntry_t> entries;
    entries.reserve(parsed.size());
    for (const auto& e : parsed) {
        if (e.is_directory) continue;

        ArchiveEntry_t out;
        out.archiveId = archiveId;
        out.entryPath = e.name_w.empty() ? Utf8ToWString(e.name.c_str()) : e.name_w;
        out.compressed_size = e.compressed_size;
        out.uncompressed_size = e.uncompressed_size;
        entries.push_back(std::move(out));
    }

    std::wstring entryErr;
    if (!db.DeleteEntriesByArchiveId(archiveId, &entryErr)) {
        LOG_WARN(L"DeleteEntriesByArchiveId failed: %s", entryErr.c_str());
    }
    if (!entries.empty()) {
        if (!db.InsertEntriesBatch(entries, &entryErr)) {
            LOG_WARN(L"InsertEntriesBatch failed: %s", entryErr.c_str());
        }
    }
}

// 获取所有需要监控的 NTFS 盘符列表
static std::vector<wchar_t> GetMonitoredDrives() {
    std::vector<wchar_t> drives;
    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) return drives;

    std::wstring buf;
    buf.resize(needed);
    if (GetLogicalDriveStringsW(needed, buf.data()) == 0) return drives;

    const wchar_t* p = buf.c_str();
    while (*p) {
        std::wstring root = p;
        p += root.size() + 1;
        if (root.size() < 2) continue;

        UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN) continue;

        // 检查是否为 NTFS
        wchar_t fsName[MAX_PATH] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) continue;
        if (_wcsicmp(fsName, L"NTFS") != 0) continue;

        wchar_t driveLetter = root[0];

        // test: 仅监控 E 盘（与 FileScanner::Scan 保持一致）
        if (driveLetter != L'E') continue;

        drives.push_back(driveLetter);
    }
    return drives;
}

// 启动后台索引线程：扫描磁盘 → 增量更新数据库 → 解析归档条目 → 进入监控循环
static void StartIndexing(HWND hWnd) {
    LOG_INFO(L"StartIndexing requested");
    StopIndexing();
    g_indexCancel.store(false);
    g_indexRunning.store(true);

    g_indexThread = std::thread([hWnd]() {
        FileScanner scanner;
        std::vector<ArchiveFile_t> scanned;
        std::wstring err;
        const bool scanOk = scanner.Scan(&scanned, &err, &g_indexCancel);
        if (!scanOk) {
            LOG_WARN(L"FileScanner::Scan failed: %s", err.c_str());
        }

        if (!g_indexCancel.load() && scanOk) {
            Database db;
            if (!db.Open(g_dbPath, &err)) {
                LOG_WARN(L"Database::Open failed: %s", err.c_str());
            } else if (!db.CreateArchivesTable(&err)) {
                LOG_WARN(L"CreateArchivesTable failed: %s", err.c_str());
            } else if (!db.CreateEntriesTable(&err)) {
                LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
            } else if (!db.CreateConfigsTable(&err)) {
                LOG_WARN(L"CreateConfigsTable failed: %s", err.c_str());
            } else {
                std::vector<ArchiveFile_t> old;
                if (!db.QueryArchives(L"", &old, &err)) {
                    LOG_WARN(L"QueryArchives failed: %s", err.c_str());
                } else {
                    struct OldInfo {
                        ArchiveFile_t file;
                        bool seen = false;
                    };
                    std::unordered_map<std::wstring, OldInfo> oldMap;
                    oldMap.reserve(old.size());

                    auto makeKey = [](const ArchiveFile_t& af) -> std::wstring {
                        return af.driveLetter + L":" + std::to_wstring((unsigned long long)af.fileRefNumber);
                    };

                    for (const auto& o : old) {
                        oldMap.emplace(makeKey(o), OldInfo{ o, false });
                    }

                    std::vector<ArchiveFile_t> upserts;
                    std::vector<ArchiveFile_t> toParse;

                    for (const auto& cur : scanned) {
                        if (g_indexCancel.load()) break;

                        const std::wstring key = makeKey(cur);
                        auto it = oldMap.find(key);
                        if (it == oldMap.end()) {
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                            continue;
                        }

                        it->second.seen = true;
                        const auto& prev = it->second.file;
                        const bool changed = (cur.usn != prev.usn) || (cur.modifyTime != prev.modifyTime) || (cur.fileSize != prev.fileSize) || (cur.filePath != prev.filePath);
                        if (changed) {
                            if (!prev.filePath.empty()) {
                                std::wstring delErr;
                                if (!db.DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                    LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                                }
                            }
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                        }
                    }

                    for (const auto& kv : oldMap) {
                        if (g_indexCancel.load()) break;
                        const auto& prev = kv.second.file;
                        if (kv.second.seen) continue;

                        if (!prev.driveLetter.empty()) {
                            db.DeleteArchiveByRefNumber(prev.driveLetter[0], (uint64_t)prev.fileRefNumber);
                        }
                        if (!prev.filePath.empty()) {
                            std::wstring delErr;
                            if (!db.DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                            }
                        }
                    }

                    if (!g_indexCancel.load() && !upserts.empty()) {
                        if (!db.InsertArchivesBatch(upserts, &err)) {
                            LOG_WARN(L"InsertArchivesBatch failed: %s", err.c_str());
                        }
                    }

                    for (const auto& a : toParse) {
                        if (g_indexCancel.load()) break;
                        ParseAndStoreArchive(db, a);
                    }
                }

                // 初始扫描完成后，记录每个盘符的 Journal NextUsn 作为监控起点
                if (!g_indexCancel.load()) {
                    auto drives = GetMonitoredDrives();
                    for (wchar_t dl : drives) {
                        JournalInfo ji;
                        if (FileScanner::QueryJournalInfo(dl, &ji, &err)) {
                            db.SaveJournalUsn(dl, ji.journalId, ji.nextUsn, &err);
                            LOG_INFO(L"Saved Journal USN for drive %c: journalId=%lld, nextUsn=%lld",
                                     dl, (long long)ji.journalId, (long long)ji.nextUsn);
                        }
                    }
                }

            }
        }

        // 通知 UI 刷新并标记初始索引完成
        PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);

        // ═══════════════════════════════════════════════════════════
        //  监控循环：定期读取 USN Journal 增量变化，实时同步数据库
        // ═══════════════════════════════════════════════════════════
        LOG_INFO(L"Entering USN Journal monitoring loop");

        while (!g_indexCancel.load()) {
            // 每 2 秒检查一次变化
            for (int i = 0; i < 20 && !g_indexCancel.load(); ++i) {
                Sleep(100);
            }
            if (g_indexCancel.load()) break;

            Database db;
            std::wstring err;
            if (!db.Open(g_dbPath, &err)) {
                LOG_WARN(L"Monitor: Database::Open failed: %s", err.c_str());
                continue;
            }

            auto drives = GetMonitoredDrives();
            bool anyChanged = false;

            for (wchar_t dl : drives) {
                if (g_indexCancel.load()) break;

                // 读取上次保存的 Journal 位置
                int64_t savedJournalId = 0;
                USN savedNextUsn = 0;
                db.GetJournalUsn(dl, &savedJournalId, &savedNextUsn);

                if (savedJournalId == 0 && savedNextUsn == 0) {
                    // 没有保存过，跳过（等待下次全量扫描）
                    continue;
                }

                // 增量读取 USN Journal 变化
                std::vector<UsnChangeRecord_t> changes;
                USN newNextUsn = 0;
                std::wstring scanErr;
                if (!FileScanner::ScanUsnJournal(dl, savedJournalId, savedNextUsn,
                                                  &changes, &newNextUsn, &scanErr, &g_indexCancel)) {
                    LOG_WARN(L"Monitor: ScanUsnJournal failed for %c: %s", dl, scanErr.c_str());
                    continue;
                }

                if (changes.empty()) {
                    // 即使没有归档文件变化，也更新 USN 位置避免重复扫描
                    if (newNextUsn > savedNextUsn) {
                        db.SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                    }
                    continue;
                }

                LOG_INFO(L"Monitor: %zu USN changes detected on drive %c", changes.size(), dl);

                // 按 fileRefNumber 去重，只保留每个文件的最后一条记录
                std::unordered_map<uint64_t, UsnChangeRecord_t> deduped;
                for (auto& cr : changes) {
                    deduped[(uint64_t)cr.fileRefNumber] = std::move(cr);
                }

                for (const auto& kv : deduped) {
                    if (g_indexCancel.load()) break;
                    const auto& cr = kv.second;

                    bool isDelete = (cr.reason & USN_REASON_FILE_DELETE) != 0;
                    bool isRenameOld = (cr.reason & USN_REASON_RENAME_OLD_NAME) != 0;

                    if (isDelete || isRenameOld) {
                        // 文件被删除或重命名（旧名）：从数据库中移除
                        ArchiveFile_t oldAf;
                        if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            LOG_INFO(L"Monitor: Archive deleted/renamed: %s", oldAf.filePath.c_str());
                            if (!oldAf.filePath.empty()) {
                                std::wstring delErr;
                                db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                            db.DeleteArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber);
                            anyChanged = true;
                        }
                    } else {
                        // 文件新增或修改：获取最新文件信息，重新解析入库
                        // 通过 OpenFileById 获取文件的完整路径和元数据
                        wchar_t volPath[] = L"\\\\.\\X:";
                        volPath[4] = dl;
                        HANDLE hVol = CreateFileW(volPath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
                        if (hVol == INVALID_HANDLE_VALUE) continue;

                        FILE_ID_DESCRIPTOR fid{};
                        fid.dwSize = sizeof(fid);
                        fid.Type = FileIdType;
                        fid.FileId.QuadPart = (LONGLONG)cr.fileRefNumber;

                        HANDLE hFile = OpenFileById(hVol, &fid, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, 0);

                        if (hFile == INVALID_HANDLE_VALUE) {
                            CloseHandle(hVol);
                            // 文件可能已被删除，清理数据库
                            ArchiveFile_t oldAf;
                            if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                                if (!oldAf.filePath.empty()) {
                                    std::wstring delErr;
                                    db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                                }
                                db.DeleteArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber);
                                anyChanged = true;
                            }
                            continue;
                        }

                        BY_HANDLE_FILE_INFORMATION fileInfo{};
                        GetFileInformationByHandle(hFile, &fileInfo);

                        uint64_t fileSize = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;
                        ULARGE_INTEGER ui{};
                        ui.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;
                        ui.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
                        uint64_t modifyTime = ui.QuadPart;

                        // 获取完整路径
                        std::wstring fullPath;
                        DWORD need = GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                        if (need > 0) {
                            fullPath.resize(need);
                            DWORD got = GetFinalPathNameByHandleW(hFile, fullPath.data(), need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                            if (got > 0 && got < need) {
                                fullPath.resize(got);
                                if (fullPath.size() >= 4 && fullPath[0] == L'\\' && fullPath[1] == L'\\' && fullPath[2] == L'?' && fullPath[3] == L'\\') {
                                    fullPath.erase(0, 4);
                                }
                            }
                        }
                        CloseHandle(hFile);
                        CloseHandle(hVol);

                        if (fullPath.empty()) continue;

                        // 先删除旧的条目（如果路径变了）
                        ArchiveFile_t oldAf;
                        if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            if (!oldAf.filePath.empty() && oldAf.filePath != fullPath) {
                                std::wstring delErr;
                                db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                        }

                        // 更新 archives 表
                        ArchiveFile_t af;
                        af.driveLetter = std::wstring(1, dl);
                        af.fileName = cr.fileName;
                        af.filePath = fullPath;
                        af.fileSize = fileSize;
                        af.modifyTime = modifyTime;
                        af.fileRefNumber = cr.fileRefNumber;
                        af.parentFileRefNumber = cr.parentFileRefNumber;
                        af.usn = cr.usn;
                        db.InsertOrUpdateArchive(af);

                        // 重新解析归档文件内容
                        LOG_INFO(L"Monitor: Re-parsing archive: %s", fullPath.c_str());
                        ParseAndStoreArchive(db, af);
                        anyChanged = true;
                    }
                }

                // 更新 Journal USN 位置
                if (newNextUsn > savedNextUsn) {
                    db.SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                }
            }

            // 如果有变化，通知 UI 刷新
            if (anyChanged && !g_indexCancel.load()) {
                PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
            }
        }

        LOG_INFO(L"USN Journal monitoring loop exited");
    });
}

// 搜索框子类化窗口过程：拦截回车键触发搜索，监听文本变化实时检索
static LRESULT CALLBACK SearchEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND hParent = GetParent(hWnd);
        if (hParent) {
            PostMessageW(hParent, WM_COMMAND, MAKEWPARAM(IDM_SEARCH_FIND, 0), 0);
        }
        return 0;
    }
    LRESULT result = CallWindowProcW(g_EditOldProc, hWnd, msg, wParam, lParam);
    if (msg == WM_CHAR || msg == WM_CLEAR || msg == WM_CUT || msg == WM_PASTE || msg == WM_UNDO ||
        (msg == WM_KEYUP && (wParam == VK_DELETE || wParam == VK_BACK))) {
        HWND hParent = GetParent(hWnd);
        if (hParent) {
            PostMessageW(hParent, WM_COMMAND, MAKEWPARAM(IDM_SEARCH_FIND, 0), 0);
        }
    }
    return result;
}

// 更新底部状态栏（显示索引状态、文件数量和归档文件数量）
static void UpdateStatusBar() {
    if (!g_hStatusBar) return;

    int fileCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        fileCount = (int)g_rowIds.size();
    }

    // 使用缓存的归档文件数量（由异步加载结果更新，避免频繁查询数据库）
    int64_t archiveCount = g_cachedArchiveCount.load();

    // 格式化数字（添加千位分隔符）
    std::wstring fileCountStr = AddThousandsSeparator(std::to_wstring(fileCount));
    std::wstring archiveCountStr = AddThousandsSeparator(std::to_wstring((long long)archiveCount));

    bool running = g_indexRunning.load();

    // 控制 Marquee 进度条的显示/隐藏，并定位到状态栏右侧
    if (g_hProgress) {
        bool isVisible = (GetWindowLongW(g_hProgress, GWL_STYLE) & WS_VISIBLE) != 0;
        if (running && !isVisible) {
            SendMessageW(g_hProgress, PBM_SETMARQUEE, TRUE, 30);
            ShowWindow(g_hProgress, SW_SHOW);
        } else if (!running && isVisible) {
            SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);
            ShowWindow(g_hProgress, SW_HIDE);
        }
        // 将进度条定位到状态栏右侧
        if (running) {
            RECT sbRect{};
            GetClientRect(g_hStatusBar, &sbRect);
            const int pbWidth = 120;
            const int pbHeight = sbRect.bottom - sbRect.top - 4;
            const int pbX = sbRect.right - pbWidth - 20; // 留出 sizegrip 空间
            const int pbY = 2;
            MoveWindow(g_hProgress, pbX, pbY, pbWidth, pbHeight, TRUE);
        }
    }

    std::wstring statusText;
    if (running) {
        statusText = LS(IDS_STATUS_PROCESSING) + L" | " + LS(IDS_STATUS_FILE_COUNT) + L": " + fileCountStr +
                     L" | " + LS(IDS_STATUS_ARCHIVE_COUNT) + L": " + archiveCountStr;
    } else {
        statusText = LS(IDS_STATUS_READY) + L" | " + LS(IDS_STATUS_FILE_COUNT) + L": " + fileCountStr +
                     L" | " + LS(IDS_STATUS_ARCHIVE_COUNT) + L": " + archiveCountStr;
    }

    SendMessageW(g_hStatusBar, SB_SETTEXTW, 0, (LPARAM)statusText.c_str());
}

static void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
}

// 自绘文本：将匹配搜索关键词的部分用粗体绘制，其余用普通字体
static void DrawTextWithBoldMatch(HDC hdc, const RECT& rcCell, const std::wstring& text, const std::wstring& filter, COLORREF textColor) {
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
            SelectObject(hdc, g_hNormalFont);
            std::wstring seg = text.substr(pos, found - pos);
            SIZE sz{};
            GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.size(), &sz);
            RECT rc = { x, rcCell.top, x + sz.cx, rcCell.bottom };
            DrawTextW(hdc, seg.c_str(), (int)seg.size(), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += sz.cx;
        }

        if (found < text.size()) {
            SelectObject(hdc, g_hBoldFont);
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

// ══════════════════════════════════════════════════════════════
//  主窗口消息处理
// ══════════════════════════════════════════════════════════════
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: { // 窗口创建：初始化所有子控件、字体、数据库，启动索引
        LOG_INFO(L"WM_CREATE");
        EnsureCommonControls();

        g_mainHwnd = hWnd;

        SetMenu(hWnd, CreateMainMenu());

        g_hSearch = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_SEARCH_EDIT,
            (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE),
            nullptr);

        if (!g_hSearch) {
            LOG_ERROR(L"CreateWindowExW search edit failed (err=%lu)", GetLastError());
        }

        g_hList = CreateWindowExW(
            0,
            WC_LISTVIEWW,
            L"",
            // LVS_OWNERDATA: 虚拟列表模式，数据由 LVN_GETDISPINFO 回调提供，排序切换无需重建条目
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_RESULTS_LIST,
            (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE),
            nullptr);

        if (!g_hList) {
            LOG_ERROR(L"CreateWindowExW listview failed (err=%lu)", GetLastError());
        }

        if (g_hList) {
            SetupListColumns(g_hList);
        }

        g_hStatusBar = CreateWindowExW(
            0,
            STATUSCLASSNAMEW,
            nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hWnd,
            (HMENU)(INT_PTR)IDC_STATUSBAR,
            (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE),
            nullptr);

        if (!g_hStatusBar) {
            LOG_ERROR(L"CreateWindowExW statusbar failed (err=%lu)", GetLastError());
        }

        // 在状态栏内创建 Marquee 进度条（转圈样式，初始隐藏）
        if (g_hStatusBar) {
            g_hProgress = CreateWindowExW(
                0,
                PROGRESS_CLASSW,
                nullptr,
                WS_CHILD | PBS_MARQUEE,  // 不含 WS_VISIBLE，初始隐藏
                0, 0, 0, 0,
                g_hStatusBar,
                (HMENU)(INT_PTR)IDC_PROGRESS,
                (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE),
                nullptr);
        }

        g_EditOldProc = (WNDPROC)SetWindowLongPtrW(g_hSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);

        {
            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

            ncm.lfMessageFont.lfWeight = FW_NORMAL;
            g_hSearchFont = CreateFontIndirectW(&ncm.lfMessageFont);
            if (g_hSearchFont && g_hSearch) {
                SendMessageW(g_hSearch, WM_SETFONT, (WPARAM)g_hSearchFont, TRUE);
            }

            g_hNormalFont = CreateFontIndirectW(&ncm.lfMessageFont);

            ncm.lfMessageFont.lfWeight = FW_BOLD;
            g_hBoldFont = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        RefreshList();
        LayoutChildren(hWnd);

        SetTimer(hWnd, IDT_STATUSBAR_TIMER, 200, nullptr);
        UpdateStatusBar();

        EnsureDatabaseReady();
        // 异步加载数据库数据，避免阻塞 UI 线程
        LoadRowsFromDbAndRefreshAsync(hWnd);

        StartIndexing(hWnd);

        // 创建系统托盘图标
        AddTrayIcon(hWnd);
        return 0;
    }

    case WM_CLOSE:
        // 关闭窗口只是隐藏到托盘，不退出程序
        if (!g_forceQuit) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        // g_forceQuit=true 时落入 DefWindowProc 触发 WM_DESTROY
        break;

    case WM_SIZE:
        LayoutChildren(hWnd);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_STATUSBAR_TIMER) {
            UpdateStatusBar();
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        LOG_INFO(L"WM_COMMAND id=%d", id);
        switch (id) {
        case IDM_FILE_EXIT:
            g_forceQuit = true;
            DestroyWindow(hWnd);
            return 0;
        case IDM_TRAY_SHOW:
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            return 0;
        case IDM_TRAY_EXIT:
            g_forceQuit = true;
            DestroyWindow(hWnd);
            return 0;
        case IDM_VIEW_REFRESH:
            return 0;
        case IDM_VIEW_STOP:
            StopIndexing();
            return 0;
        case IDM_SEARCH_FIND:
            LoadRowsFromDbAndRefreshAsync(hWnd);
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd, LS(IDS_ABOUT_TEXT).c_str(), LS(IDS_ABOUT_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            return 0;
        }
    }

    case WM_APP_INDEX_DONE:
        g_indexRunning.store(false);
        UpdateStatusBar();
        LOG_INFO(L"WM_APP_INDEX_DONE");
        return 0;

    case WM_APP_DB_REFRESH:
        LoadRowsFromDbAndRefreshAsync(hWnd);
        return 0;

    case WM_APP_ROWS_READY: {
        // 后台线程查询完成，交换 rowid 列表到 UI 线程
        auto* result = (AsyncLoadResult*)lParam;
        if (result) {
            {
                std::lock_guard<std::mutex> lk(g_rowsMutex);
                g_rowIds = std::move(result->rowIds);
                g_totalEntryCount = result->entryCount;
            }
            // 清空行缓存（数据可能已变化）
            ClearRowCache();
            g_cachedArchiveCount.store(result->archiveCount);
            delete result;

            RefreshList();
            UpdateColumnHeaders();
            UpdateStatusBar();
        }
        return 0;
    }

    case WM_APP_UPDATE_STATUSBAR:
        UpdateStatusBar();
        return 0;

    case WM_NOTIFY: { // ListView 通知：双击事件 + NM_CUSTOMDRAW 自绘（关键词加粗）
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr && hdr->hwndFrom == g_hList) {
            // LVN_GETDISPINFO: 虚拟列表回调，按需从缓存/数据库获取行数据
            if (hdr->code == LVN_GETDISPINFOW) {
                NMLVDISPINFOW* pdi = (NMLVDISPINFOW*)lParam;
                int iItem = pdi->item.iItem;
                int iSub = pdi->item.iSubItem;

                // 获取 rowid
                int64_t rowId = 0;
                {
                    std::lock_guard<std::mutex> lk(g_rowsMutex);
                    if (iItem < 0 || iItem >= (int)g_rowIds.size()) return 0;
                    rowId = g_rowIds[iItem];
                }

                // 从 LRU 缓存获取行数据（缓存未命中时自动查询数据库）
                const CachedRow* cr = GetCachedRow(rowId);
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
                if (clickedCol == g_sortColumn) {
                    g_sortAscending = !g_sortAscending;
                } else {
                    g_sortColumn = clickedCol;
                    g_sortAscending = true;
                }
                // 异步重新查询（带新排序条件）
                LoadRowsFromDbAndRefreshAsync(hWnd);
                return 0;
            }
            if (hdr->code == NM_DBLCLK) {
                MessageBoxW(hWnd, LS(IDS_DBLCLICK_PLACEHOLDER).c_str(), L"Info", MB_OK);
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
                                std::lock_guard<std::mutex> lk(g_rowsMutex);
                                if (iItem >= 0 && iItem < (int)g_rowIds.size()) {
                                    rowId = g_rowIds[iItem];
                                }
                            }
                            if (rowId > 0) {
                                const CachedRow* cr = GetCachedRow(rowId);
                                if (cr) {
                                    switch (iSubItem) {
                                    case 0: text = cr->name; iconIdx = cr->iconIndex; break;
                                    case 1: text = cr->archivePath; break;
                                    case 2: text = cr->entryPath; break;
                                    }
                                }
                            }
                        }

                        std::wstring filter = GetSearchFilter();

                        // 列0：获取图标区域和文本区域分别绘制
                        RECT rcIcon{}, rcText{};
                        if (iSubItem == 0) {
                            ListView_GetItemRect(g_hList, iItem, &rcIcon, LVIR_ICON);
                            ListView_GetItemRect(g_hList, iItem, &rcText, LVIR_LABEL);
                        } else {
                            ListView_GetSubItemRect(g_hList, iItem, iSubItem, LVIR_BOUNDS, &rcText);
                        }

                        // 绘制整行背景（列0需要覆盖图标+文本区域）
                        RECT rcFill = (iSubItem == 0) ? RECT{ rcIcon.left, rcIcon.top, rcText.right, rcText.bottom } : rcText;
                        bool selected = (ListView_GetItemState(g_hList, iItem, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);

                        if (selected) {
                            FillRect(lvcd->nmcd.hdc, &rcFill, GetSysColorBrush(COLOR_HIGHLIGHT));
                        } else {
                            FillRect(lvcd->nmcd.hdc, &rcFill, GetSysColorBrush(COLOR_WINDOW));
                        }

                        // 列0：在图标区域绘制文件类型图标
                        if (iSubItem == 0 && g_hSysSmallIcons) {
                            int iconX = rcIcon.left;
                            int iconY = rcIcon.top + ((rcIcon.bottom - rcIcon.top) - 16) / 2;
                            ImageList_Draw(g_hSysSmallIcons, iconIdx, lvcd->nmcd.hdc,
                                iconX, iconY, ILD_TRANSPARENT);
                        }

                        DrawTextWithBoldMatch(lvcd->nmcd.hdc, rcText, text, filter, textColor);
                        return CDRF_SKIPDEFAULT;
                    }
                    // 列3/4（压缩大小、原始大小）：强制使用普通字体，防止继承自绘时的粗体
                    if (g_hNormalFont) {
                        SelectObject(lvcd->nmcd.hdc, g_hNormalFont);
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
            ShowTrayMenu(hWnd);
            break;
        }
        return 0;

    case WM_DESTROY: // 窗口销毁：移除托盘图标、停止索引、关闭数据库、释放字体资源
        LOG_INFO(L"WM_DESTROY");
        RemoveTrayIcon();
        KillTimer(hWnd, IDT_STATUSBAR_TIMER);
        StopIndexing();
        g_cacheDb.Close();
        g_cacheDbOpen = false;
        g_database.Close();
        if (g_hSearchFont) { DeleteObject(g_hSearchFont); g_hSearchFont = nullptr; }
        if (g_hNormalFont) { DeleteObject(g_hNormalFont); g_hNormalFont = nullptr; }
        if (g_hBoldFont) { DeleteObject(g_hBoldFont); g_hBoldFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ══════════════════════════════════════════════════════════════
//  程序入口
// ══════════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInstance = hInstance;
    Logger::Init();
    LOG_INFO(L"wWinMain start");

    g_dbPath = GetExeDir() + L"\\everyarchive.db";

    const bool p1 = EnablePrivilege(SE_MANAGE_VOLUME_NAME);
    const bool p2 = EnablePrivilege(SE_BACKUP_NAME);
    LOG_INFO(L"EnablePrivilege SeManageVolume=%d SeBackup=%d", (int)p1, (int)p2);

    EnsureCommonControls();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kAppClassName;

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR(L"RegisterClassExW failed (err=%lu)", GetLastError());
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND hWnd = CreateWindowExW(
        0,
        kAppClassName,
        kAppTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 620,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd) {
        LOG_ERROR(L"CreateWindowExW main window failed (err=%lu)", GetLastError());
        MessageBoxW(nullptr, L"CreateWindowExW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // 主消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOG_INFO(L"Message loop exit code=%lld", (long long)msg.wParam);
    Logger::Shutdown();
    return (int)msg.wParam;
}
