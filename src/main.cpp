#include <windows.h>
#include <commctrl.h>
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

static constexpr wchar_t kAppClassName[] = L"EveryArchiveMainWindow";
static constexpr wchar_t kAppTitle[] = L"EveryArchive";

// ListView 每一行的显示数据
struct ResultRow {
    std::wstring name;
    std::wstring archivePath;  // 归档文件路径
    std::wstring entryPath;    // 归档内部文件路径
    std::wstring size;
    std::wstring mtime;
    // 排序用原始数值（压缩大小、原始大小）
    int64_t compressed_size_raw = 0;
    int64_t uncompressed_size_raw = 0;
};

static void PostAddResult(HWND hWnd, ResultRow* row);

// ── 全局 UI 控件句柄 ──
static HWND g_hSearch = nullptr;      // 搜索输入框
static HWND g_hList = nullptr;        // 结果 ListView
static HWND g_hStatusBar = nullptr;   // 底部状态栏
static WNDPROC g_EditOldProc = nullptr; // 搜索框原始窗口过程（子类化用）
static HFONT g_hSearchFont = nullptr; // 搜索框字体（非粗体）
static HFONT g_hNormalFont = nullptr; // ListView 普通字体
static HFONT g_hBoldFont = nullptr;   // ListView 加粗字体（高亮匹配文本）

// ── ListView 数据源（需加锁访问）──
static std::vector<ResultRow> g_rows;
static std::mutex g_rowsMutex;

// ── 列头排序状态 ──
static int g_sortColumn = -1;       // 当前排序列索引，-1 表示未排序
static bool g_sortAscending = true; // true=正序，false=倒序

// ── 后台索引线程相关 ──
static std::atomic_bool g_indexCancel{ false };  // 取消标志
static std::atomic_bool g_indexRunning{ false }; // 运行状态
static std::thread g_indexThread;                // 索引工作线程
static HWND g_mainHwnd = nullptr;
static int g_spinnerFrame = 0;
static const wchar_t* g_spinnerChars[] = { L"|", L"/", L"-", L"\\" };

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
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"\u6587\u4ef6(&F)\tAlt+F");

    HMENU hEdit = CreatePopupMenu();
    AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY, L"\u7f16\u8f91(&E)");

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH, L"\u5237\u65b0\u7d22\u5f15(&R)\tF5");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_STOP, L"\u505c\u6b62\u7d22\u5f15(&S)");

    HMENU hSearch = CreatePopupMenu();
    AppendMenuW(hSearch, MF_STRING, IDM_SEARCH_FIND, L"\u641c\u7d22(&S)");

    HMENU hBookmark = CreatePopupMenu();
    AppendMenuW(hBookmark, MF_STRING, IDM_BOOKMARK_ADD, L"\u4e66\u7b7e(&B)");

    HMENU hTools = CreatePopupMenu();
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_OPTIONS, L"\u5de5\u5177(&T)");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"\u5e2e\u52a9(&H)");

    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, L"\u6587\u4ef6(&F)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hEdit, L"\u7f16\u8f91(&E)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, L"\u89c6\u56fe(&V)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hSearch, L"\u641c\u7d22(&S)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hBookmark, L"\u4e66\u7b7e(&B)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hTools, L"\u5de5\u5177(&T)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, L"\u5e2e\u52a9(&H)");

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

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = const_cast<LPWSTR>(L"\u540d\u79f0");
    col.cx = 200;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    // 归档文件路径列
    col.pszText = const_cast<LPWSTR>(L"\u5f52\u6863\u6587\u4ef6");
    col.cx = 280;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    // 归档内部文件路径列
    col.pszText = const_cast<LPWSTR>(L"\u5185\u90e8\u8def\u5f84");
    col.cx = 280;
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    // 压缩大小列（右对齐）
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt = LVCFMT_RIGHT;
    col.pszText = const_cast<LPWSTR>(L"\u538b\u7f29\u5927\u5c0f");
    col.cx = 100;
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);

    // 原始大小列（右对齐）
    col.pszText = const_cast<LPWSTR>(L"\u539f\u59cb\u5927\u5c0f");
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

        std::vector<ArchiveEntry_t> entries;
        entries.reserve(parsed.size());
        for (const auto& e : parsed) {
            if (e.is_directory) continue;

            ArchiveEntry_t out;
            out.archivePath = a.filePath;
            out.entryPath = e.name_w.empty() ? Utf8ToWString(e.name.c_str()) : e.name_w;
            out.entryName = GetEntryNameFromPath(out.entryPath);
            out.compressed_size = e.compressed_size;
            out.uncompressed_size = e.uncompressed_size;
            entries.push_back(std::move(out));
        }

        if (!g_database.DeleteEntriesByArchivePath(a.filePath, &err)) {
            LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", err.c_str());
        }
        if (!g_database.InsertEntriesBatch(entries, &err)) {
            LOG_WARN(L"InsertEntriesBatch failed: %s", err.c_str());
        }
    }
}

// 对 g_rows 按当前排序列和方向进行排序（调用前需持有 g_rowsMutex）
static void SortRows() {
    if (g_sortColumn < 0 || g_sortColumn > 4) return;

    std::sort(g_rows.begin(), g_rows.end(),
        [](const ResultRow& a, const ResultRow& b) -> bool {
            int cmp = 0;
            switch (g_sortColumn) {
            case 0: // 名称：按字符串字典序比较（不区分大小写）
                cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
            case 1: // 归档文件路径：按字符串字典序比较（不区分大小写）
                cmp = _wcsicmp(a.archivePath.c_str(), b.archivePath.c_str());
                break;
            case 2: // 内部路径：按字符串字典序比较（不区分大小写）
                cmp = _wcsicmp(a.entryPath.c_str(), b.entryPath.c_str());
                break;
            case 3: // 压缩大小：按数值大小比较
                cmp = (a.compressed_size_raw < b.compressed_size_raw) ? -1
                    : (a.compressed_size_raw > b.compressed_size_raw) ? 1 : 0;
                break;
            case 4: // 原始大小：按数值大小比较
                cmp = (a.uncompressed_size_raw < b.uncompressed_size_raw) ? -1
                    : (a.uncompressed_size_raw > b.uncompressed_size_raw) ? 1 : 0;
                break;
            }
            return g_sortAscending ? (cmp < 0) : (cmp > 0);
        });
}

// 更新 ListView 列头文本，在当前排序列后附加箭头指示符（▲/▼）
static void UpdateColumnHeaders() {
    // 列头原始文本（5列：名称、归档文件、内部路径、压缩大小、原始大小）
    static const wchar_t* colNames[] = {
        L"\u540d\u79f0",       // 名称
        L"\u5f52\u6863\u6587\u4ef6",   // 归档文件
        L"\u5185\u90e8\u8def\u5f84",   // 内部路径
        L"\u538b\u7f29\u5927\u5c0f",   // 压缩大小
        L"\u539f\u59cb\u5927\u5c0f"    // 原始大小
    };

    for (int i = 0; i < 5; ++i) {
        std::wstring text = colNames[i];
        // 在当前排序列后添加箭头指示符
        if (i == g_sortColumn) {
            text += g_sortAscending ? L" \u25B2" : L" \u25BC"; // ▲=正序 ▼=倒序
        }

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_SetColumn(g_hList, i, &col);
    }
}

// 根据 g_rows 刷新 ListView 显示内容（虚拟列表模式：只需设置条目数量，数据由 LVN_GETDISPINFO 提供）
static void RefreshList() {
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        count = (int)g_rows.size();
    }
    // LVSICF_NOINVALIDATEALL: 避免全量重绘闪烁，仅更新可见区域
    ListView_SetItemCountEx(g_hList, count, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(g_hList, nullptr, FALSE);
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

// 根据当前搜索条件从数据库查询条目，更新 g_rows 并刷新 ListView
static void LoadRowsFromDbAndRefresh() {
    std::wstring err;
    const std::wstring filter = GetSearchFilter();
    Database db;
    if (!db.Open(g_dbPath, &err)) {
        LOG_WARN(L"Database::Open failed: %s", err.c_str());
        return;
    }

    if (!db.CreateEntriesTable(&err)) {
        LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
        return;
    }

    std::vector<ArchiveEntry_t> entries;
    if (!db.QueryEntries(filter, &entries, &err)) {
        LOG_WARN(L"QueryEntries failed: %s", err.c_str());
        return;
    }

    std::vector<ResultRow> rows;
    rows.reserve(entries.size());
    for (const auto& e : entries) {
        ResultRow r;
        r.name = e.entryName;
        r.archivePath = e.archivePath;
        r.entryPath = e.entryPath;
        r.size = FormatSizeULongLong((ULONGLONG)e.compressed_size);
        r.mtime = FormatSizeULongLong((ULONGLONG)e.uncompressed_size);
        // 保存原始数值用于排序
        r.compressed_size_raw = e.compressed_size;
        r.uncompressed_size_raw = e.uncompressed_size;
        rows.push_back(std::move(r));
    }

    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        g_rows = std::move(rows);
        // 如果当前有排序状态，对新数据应用排序
        SortRows();
    }

    RefreshList();
    UpdateColumnHeaders();
}

static void PostAddResult(HWND hWnd, ResultRow* row) {
    PostMessageW(hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)row);
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

// 启动后台索引线程：扫描磁盘 → 增量更新数据库 → 解析归档条目
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

                        std::vector<ArchiveEntry_t> entries;
                        entries.reserve(parsed.size());
                        for (const auto& e : parsed) {
                            if (e.is_directory) continue;

                            ArchiveEntry_t out;
                            out.archivePath = a.filePath;
                            out.entryPath = e.name_w.empty() ? Utf8ToWString(e.name.c_str()) : e.name_w;
                            out.entryName = GetEntryNameFromPath(out.entryPath);
                            out.compressed_size = e.compressed_size;
                            out.uncompressed_size = e.uncompressed_size;
                            entries.push_back(std::move(out));
                        }

                        std::wstring entryErr;
                        if (!db.DeleteEntriesByArchivePath(a.filePath, &entryErr)) {
                            LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", entryErr.c_str());
                        }
                        if (!db.InsertEntriesBatch(entries, &entryErr)) {
                            LOG_WARN(L"InsertEntriesBatch failed: %s", entryErr.c_str());
                        }
                    }
                }
            }
        }

        PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);
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

// 更新底部状态栏（显示索引状态和文件数量）
static void UpdateStatusBar() {
    if (!g_hStatusBar) return;

    std::wstring statusText;
    
    int fileCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        fileCount = (int)g_rows.size();
    }

    if (g_indexRunning.load()) {
        const wchar_t* spinner = g_spinnerChars[g_spinnerFrame % 4];
        statusText = std::wstring(L"\u6B63\u5728\u5904\u7406 ") + spinner + L" | \u6587\u4EF6\u6570\u91CF: " + std::to_wstring(fileCount);
    } else {
        statusText = L"\u5C31\u7EEA | \u6587\u4EF6\u6570\u91CF: " + std::to_wstring(fileCount);
    }

    SendMessageW(g_hStatusBar, SB_SETTEXTW, 0, (LPARAM)statusText.c_str());
}

static void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
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
        LoadRowsFromDbAndRefresh();

        StartIndexing(hWnd);
        return 0;
    }
    case WM_SIZE:
        LayoutChildren(hWnd);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_STATUSBAR_TIMER) {
            if (g_indexRunning.load()) {
                g_spinnerFrame++;
            }
            UpdateStatusBar();
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        LOG_INFO(L"WM_COMMAND id=%d", id);
        switch (id) {
        case IDM_FILE_EXIT:
            DestroyWindow(hWnd);
            return 0;
        case IDM_VIEW_REFRESH:
            return 0;
        case IDM_VIEW_STOP:
            StopIndexing();
            return 0;
        case IDM_SEARCH_FIND:
            LoadRowsFromDbAndRefresh();
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd, L"EveryArchive", L"About", MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            return 0;
        }
    }

    case WM_APP_ADD_RESULT: {
        auto* row = (ResultRow*)lParam;
        if (!row) return 0;

        if (!PassesFilter(row->name)) {
            delete row;
            return 0;
        }

        {
            std::lock_guard<std::mutex> lk(g_rowsMutex);
            g_rows.push_back(*row);
        }

        delete row;

        // 虚拟列表模式：只需更新条目数量，无需逐行插入
        RefreshList();
        UpdateStatusBar();
        return 0;
    }

    case WM_APP_INDEX_DONE:
        g_indexRunning.store(false);
        g_spinnerFrame = 0;
        UpdateStatusBar();
        LOG_INFO(L"WM_APP_INDEX_DONE");
        return 0;

    case WM_APP_DB_REFRESH:
        LoadRowsFromDbAndRefresh();
        UpdateStatusBar();
        return 0;

    case WM_APP_UPDATE_STATUSBAR:
        UpdateStatusBar();
        return 0;

    case WM_NOTIFY: { // ListView 通知：双击事件 + NM_CUSTOMDRAW 自绘（关键词加粗）
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr && hdr->hwndFrom == g_hList) {
            // LVN_GETDISPINFO: 虚拟列表回调，ListView 需要显示某行数据时触发
            if (hdr->code == LVN_GETDISPINFOW) {
                NMLVDISPINFOW* pdi = (NMLVDISPINFOW*)lParam;
                if (pdi->item.mask & LVIF_TEXT) {
                    int iItem = pdi->item.iItem;
                    int iSub = pdi->item.iSubItem;
                    // 线程局部静态缓冲区，避免临时 wstring 被释放后指针悬挂
                    static thread_local wchar_t buf[1024];
                    buf[0] = L'\0';
                    {
                        std::lock_guard<std::mutex> lk(g_rowsMutex);
                        if (iItem >= 0 && iItem < (int)g_rows.size()) {
                            const auto& r = g_rows[iItem];
                            const std::wstring* src = nullptr;
                            switch (iSub) {
                            case 0: src = &r.name; break;
                            case 1: src = &r.archivePath; break;
                            case 2: src = &r.entryPath; break;
                            case 3: src = &r.size; break;
                            case 4: src = &r.mtime; break;
                            }
                            if (src) {
                                wcsncpy_s(buf, src->c_str(), _TRUNCATE);
                            }
                        }
                    }
                    pdi->item.pszText = buf;
                }
                return 0;
            }
            // 列头点击排序：点击同一列切换正序/倒序，点击不同列默认正序
            if (hdr->code == LVN_COLUMNCLICK) {
                LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
                int clickedCol = nmlv->iSubItem;
                if (clickedCol == g_sortColumn) {
                    // 再次点击同一列：切换排序方向
                    g_sortAscending = !g_sortAscending;
                } else {
                    // 点击新列：设为正序
                    g_sortColumn = clickedCol;
                    g_sortAscending = true;
                }
                {
                    std::lock_guard<std::mutex> lk(g_rowsMutex);
                    SortRows();
                }
                RefreshList();
                UpdateColumnHeaders();
                return 0;
            }
            if (hdr->code == NM_DBLCLK) {
                MessageBoxW(hWnd, L"Double click (placeholder).", L"Info", MB_OK);
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
                        {
                            std::lock_guard<std::mutex> lk(g_rowsMutex);
                            if (iItem >= 0 && iItem < (int)g_rows.size()) {
                                const auto& row = g_rows[iItem];
                                switch (iSubItem) {
                                case 0: text = row.name; break;
                                case 1: text = row.archivePath; break;
                                case 2: text = row.entryPath; break;
                                }
                            }
                        }

                        std::wstring filter = GetSearchFilter();

                        RECT rcSubItem{};
                        if (iSubItem == 0) {
                            ListView_GetItemRect(g_hList, iItem, &rcSubItem, LVIR_LABEL);
                        } else {
                            ListView_GetSubItemRect(g_hList, iItem, iSubItem, LVIR_BOUNDS, &rcSubItem);
                        }

                        bool selected = (ListView_GetItemState(g_hList, iItem, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);

                        if (selected) {
                            HBRUSH hBrush = GetSysColorBrush(COLOR_HIGHLIGHT);
                            FillRect(lvcd->nmcd.hdc, &rcSubItem, hBrush);
                        } else {
                            HBRUSH hBrush = GetSysColorBrush(COLOR_WINDOW);
                            FillRect(lvcd->nmcd.hdc, &rcSubItem, hBrush);
                        }

                        DrawTextWithBoldMatch(lvcd->nmcd.hdc, rcSubItem, text, filter, textColor);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
                }
            }
        }
        break;
    }

    case WM_DESTROY: // 窗口销毁：停止索引、关闭数据库、释放字体资源
        LOG_INFO(L"WM_DESTROY");
        KillTimer(hWnd, IDT_STATUSBAR_TIMER);
        StopIndexing();
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
