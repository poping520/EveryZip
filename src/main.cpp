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
#include <vector>

#include "resource.h"
#include "logger.h"

#include <sqlite3.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

static constexpr wchar_t kAppClassName[] = L"EveryArchiveMainWindow";
static constexpr wchar_t kAppTitle[] = L"EveryArchive";

struct ResultRow {
    std::wstring name;
    std::wstring path;
    std::wstring size;
    std::wstring mtime;
};

static void PostAddResult(HWND hWnd, ResultRow* row);

static HWND g_hSearch = nullptr;
static HWND g_hList = nullptr;
static WNDPROC g_EditOldProc = nullptr;

static std::vector<ResultRow> g_rows;
static std::mutex g_rowsMutex;

static std::atomic_bool g_indexCancel{ false };
static std::atomic_bool g_indexRunning{ false };
static std::thread g_indexThread;
static HWND g_mainHwnd = nullptr;

static sqlite3* g_db = nullptr;
static sqlite3_stmt* g_stmtInsert = nullptr;
static std::mutex g_dbMutex;
static std::condition_variable g_dbCv;
static std::deque<ResultRow> g_dbQueue;
static std::atomic_bool g_dbStop{ false };
static std::thread g_dbThread;

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

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    PathRemoveFileSpecW(path);
    return path;
}

static bool DbInit() {
    const std::wstring dbPathW = GetExeDir() + L"\\everyarchive.db";
    const std::string dbPath = WideToUtf8(dbPathW);

    int rc = sqlite3_open_v2(dbPath.c_str(), &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK || !g_db) {
        LOG_ERROR(L"sqlite3_open_v2 failed rc=%d", rc);
        return false;
    }

    sqlite3_busy_timeout(g_db, 2000);

    const char* sqlCreate =
        "CREATE TABLE IF NOT EXISTS archive_files("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "path TEXT NOT NULL UNIQUE,"
        "size INTEGER,"
        "mtime TEXT,"
        "indexed_at INTEGER"
        ");";

    char* errMsg = nullptr;
    rc = sqlite3_exec(g_db, sqlCreate, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) {
            LOG_ERROR(L"sqlite3_exec create table failed rc=%d msg=%S", rc, errMsg);
            sqlite3_free(errMsg);
        } else {
            LOG_ERROR(L"sqlite3_exec create table failed rc=%d", rc);
        }
        return false;
    }

    const char* sqlInsert =
        "INSERT INTO archive_files(name,path,size,mtime,indexed_at) VALUES(?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET name=excluded.name,size=excluded.size,mtime=excluded.mtime,indexed_at=excluded.indexed_at;";
    rc = sqlite3_prepare_v2(g_db, sqlInsert, -1, &g_stmtInsert, nullptr);
    if (rc != SQLITE_OK || !g_stmtInsert) {
        LOG_ERROR(L"sqlite3_prepare_v2 failed rc=%d", rc);
        return false;
    }

    g_dbStop.store(false);
    g_dbThread = std::thread([]() {
        LOG_INFO(L"DB thread started");
        int pendingInTxn = 0;
        sqlite3_exec(g_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

        while (true) {
            ResultRow row;
            {
                std::unique_lock<std::mutex> lk(g_dbMutex);
                g_dbCv.wait(lk, [] { return g_dbStop.load() || !g_dbQueue.empty(); });
                if (g_dbStop.load() && g_dbQueue.empty()) break;
                row = std::move(g_dbQueue.front());
                g_dbQueue.pop_front();
            }

            const std::string name = WideToUtf8(row.name);
            const std::string path = WideToUtf8(row.path);
            const std::string mtime = WideToUtf8(row.mtime);

            sqlite3_reset(g_stmtInsert);
            sqlite3_clear_bindings(g_stmtInsert);
            sqlite3_bind_text(g_stmtInsert, 1, name.c_str(), (int)name.size(), SQLITE_TRANSIENT);
            sqlite3_bind_text(g_stmtInsert, 2, path.c_str(), (int)path.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int64(g_stmtInsert, 3, _wtoi64(row.size.c_str()));
            sqlite3_bind_text(g_stmtInsert, 4, mtime.c_str(), (int)mtime.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int64(g_stmtInsert, 5, (sqlite3_int64)time(nullptr));

            const int rc = sqlite3_step(g_stmtInsert);
            if (rc != SQLITE_DONE) {
                LOG_WARN(L"sqlite3_step insert rc=%d", rc);
            }

            pendingInTxn++;
            if (pendingInTxn >= 200) {
                sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);
                sqlite3_exec(g_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
                pendingInTxn = 0;
            }
        }

        sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);
        LOG_INFO(L"DB thread finished");
    });

    return true;
}

static void DbEnqueue(const ResultRow& row) {
    if (!g_db) return;
    {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        g_dbQueue.push_back(row);
    }
    g_dbCv.notify_one();
}

static void DbShutdown() {
    if (!g_db) return;
    g_dbStop.store(true);
    g_dbCv.notify_all();
    if (g_dbThread.joinable()) {
        g_dbThread.join();
    }
    if (g_stmtInsert) {
        sqlite3_finalize(g_stmtInsert);
        g_stmtInsert = nullptr;
    }
    sqlite3_close(g_db);
    g_db = nullptr;
}

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

static bool PassesFilter(const std::wstring& name) {
    const std::wstring filter = GetSearchFilter();
    if (filter.empty()) return true;
    std::wstring n = ToLower(name);
    return n.find(filter) != std::wstring::npos;
}

static void SetupListColumns(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = const_cast<LPWSTR>(L"\u540d\u79f0");
    col.cx = 240;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = const_cast<LPWSTR>(L"\u8def\u5f84");
    col.cx = 520;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    col.pszText = const_cast<LPWSTR>(L"\u5927\u5c0f");
    col.cx = 100;
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    col.pszText = const_cast<LPWSTR>(L"\u4fee\u6539\u65f6\u95f4");
    col.cx = 150;
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);
}

static void RefreshList() {
    ListView_DeleteAllItems(g_hList);

    std::lock_guard<std::mutex> lk(g_rowsMutex);
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        const auto& r = g_rows[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(r.name.c_str());
        int idx = ListView_InsertItem(g_hList, &item);

        ListView_SetItemText(g_hList, idx, 1, const_cast<LPWSTR>(r.path.c_str()));
        ListView_SetItemText(g_hList, idx, 2, const_cast<LPWSTR>(r.size.c_str()));
        ListView_SetItemText(g_hList, idx, 3, const_cast<LPWSTR>(r.mtime.c_str()));
    }
}

static void LayoutChildren(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);

    const int margin = 6;
    const int editH = 24;

    int x = margin;
    int y = margin;
    int w = (rc.right - rc.left) - margin * 2;
    int h = (rc.bottom - rc.top) - margin * 2;

    MoveWindow(g_hSearch, x, y, w, editH, TRUE);

    y += editH + margin;
    h -= editH + margin;

    MoveWindow(g_hList, x, y, w, h, TRUE);
}

static std::wstring FormatSizeULongLong(ULONGLONG v) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"%llu", v);
    return buf;
}

static std::wstring FormatFileTimeLocal(const FILETIME& ftUtc) {
    FILETIME ftLocal{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &ftLocal)) return L"";
    if (!FileTimeToSystemTime(&ftLocal, &st)) return L"";

    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u/%u/%u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

static bool EndsWithInsensitive(const std::wstring& s, const wchar_t* suffix) {
    const size_t sl = s.size();
    const size_t tl = wcslen(suffix);
    if (sl < tl) return false;
    return _wcsicmp(s.c_str() + (sl - tl), suffix) == 0;
}

static bool TryGetPathSizeTimeByFrn(HANDLE hVolume, ULONGLONG frn, std::wstring& outPath, ULONGLONG& outSize, FILETIME& outWriteTimeUtc) {
    outPath.clear();
    outSize = 0;
    outWriteTimeUtc = {};

    FILE_ID_DESCRIPTOR fid{};
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    fid.FileId.QuadPart = frn;

    HANDLE hFile = OpenFileById(
        hVolume,
        &fid,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        0);

    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_WARN(L"OpenFileById failed for FRN=%llu (err=%lu)", frn, GetLastError());
        return false;
    }

    wchar_t pathBuf[32768]{};
    DWORD len = GetFinalPathNameByHandleW(hFile, pathBuf, (DWORD)(sizeof(pathBuf) / sizeof(pathBuf[0])), FILE_NAME_NORMALIZED);
    if (len == 0 || len >= (DWORD)(sizeof(pathBuf) / sizeof(pathBuf[0]))) {
        LOG_WARN(L"GetFinalPathNameByHandleW failed for FRN=%llu (err=%lu)", frn, GetLastError());
        CloseHandle(hFile);
        return false;
    }
    outPath.assign(pathBuf, len);

    FILE_STANDARD_INFO st{};
    if (GetFileInformationByHandleEx(hFile, FileStandardInfo, &st, sizeof(st))) {
        outSize = (ULONGLONG)st.EndOfFile.QuadPart;
    }

    FILE_BASIC_INFO bi{};
    if (GetFileInformationByHandleEx(hFile, FileBasicInfo, &bi, sizeof(bi))) {
        outWriteTimeUtc.dwLowDateTime = bi.LastWriteTime.LowPart;
        outWriteTimeUtc.dwHighDateTime = bi.LastWriteTime.HighPart;
    }

    CloseHandle(hFile);
    return true;
}

static void IndexDriveFallbackFind(HWND hWnd, wchar_t driveLetter) {
    wchar_t root[8]{};
    swprintf_s(root, L"%c:\\", driveLetter);

    std::deque<std::wstring> q;
    q.emplace_back(std::wstring(L"\\\\?\\") + root);

    while (!q.empty() && !g_indexCancel.load()) {
        std::wstring dir = std::move(q.front());
        q.pop_front();

        std::wstring pattern = dir;
        if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (g_indexCancel.load()) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            std::wstring full = dir;
            if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
            full.append(fd.cFileName);

            if (isDir) {
                if (!isReparse) {
                    q.emplace_back(std::move(full));
                }
                continue;
            }

            std::wstring name = fd.cFileName;
            if (!EndsWithInsensitive(name, L".apk")) {
                continue;
            }

            auto* row = new ResultRow();
            row->name = std::move(name);
            row->path = full;
            const ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | (ULONGLONG)fd.nFileSizeLow;
            row->size = FormatSizeULongLong(size);
            row->mtime = FormatFileTimeLocal(fd.ftLastWriteTime);
            PostAddResult(hWnd, row);
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
    }
}

static void PostAddResult(HWND hWnd, ResultRow* row) {
    PostMessageW(hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)row);
}

static void IndexVolumeUsn(HWND hWnd, const wchar_t driveLetter) {
    wchar_t volPath[16]{};
    swprintf_s(volPath, L"\\\\.\\%c:", driveLetter);

    LOG_INFO(L"IndexVolumeUsn start drive=%c", driveLetter);

    HANDLE hVol = CreateFileW(
        volPath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hVol == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        LOG_WARN(L"CreateFileW volume failed path=%s (err=%lu)", volPath, err);
        if (err == ERROR_ACCESS_DENIED) {
            LOG_WARN(L"Access denied on volume %c:; fallback to directory traversal", driveLetter);
            IndexDriveFallbackFind(hWnd, driveLetter);
        }
        return;
    }

    DWORD br = 0;
    USN_JOURNAL_DATA_V0 journal{};
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &br, nullptr)) {
        LOG_WARN(L"FSCTL_QUERY_USN_JOURNAL failed drive=%c (err=%lu)", driveLetter, GetLastError());
        CloseHandle(hVol);
        return;
    }

    MFT_ENUM_DATA_V0 med{};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = journal.NextUsn;

    const DWORD kBufSize = 1 << 20;
    std::vector<BYTE> buffer;
    buffer.resize(kBufSize);

    while (!g_indexCancel.load()) {
        br = 0;
        if (!DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buffer.data(), (DWORD)buffer.size(), &br, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                LOG_INFO(L"FSCTL_ENUM_USN_DATA reached EOF drive=%c", driveLetter);
            } else {
                LOG_WARN(L"FSCTL_ENUM_USN_DATA failed drive=%c (err=%lu)", driveLetter, err);
            }
            break;
        }

        if (br < sizeof(USN)) {
            break;
        }

        USN* pUsn = (USN*)buffer.data();
        BYTE* p = buffer.data() + sizeof(USN);
        BYTE* end = buffer.data() + br;

        while (p + sizeof(USN_RECORD_V2) <= end) {
            if (g_indexCancel.load()) break;

            auto* rec = (USN_RECORD_V2*)p;
            if (rec->RecordLength == 0) break;

            const wchar_t* fname = (const wchar_t*)((BYTE*)rec + rec->FileNameOffset);
            const int fnChars = (int)(rec->FileNameLength / sizeof(wchar_t));
            std::wstring name(fname, fname + fnChars);

            const bool isDir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!isDir) {
                if (EndsWithInsensitive(name, L".apk") || EndsWithInsensitive(name, L".zip")) {
                    std::wstring fullPath;
                    ULONGLONG fsize = 0;
                    FILETIME ftWriteUtc{};

                    if (TryGetPathSizeTimeByFrn(hVol, (ULONGLONG)rec->FileReferenceNumber, fullPath, fsize, ftWriteUtc)) {
                        auto* row = new ResultRow();
                        row->name = name;
                        row->path = fullPath;
                        row->size = FormatSizeULongLong(fsize);
                        row->mtime = FormatFileTimeLocal(ftWriteUtc);
                        PostAddResult(hWnd, row);
                    }
                }
            }

            p += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *pUsn;
    }

    CloseHandle(hVol);

    LOG_INFO(L"IndexVolumeUsn done drive=%c", driveLetter);
}

static void IndexAllFixedDrives(HWND hWnd) {
    DWORD drives = GetLogicalDrives();

    LOG_INFO(L"IndexAllFixedDrives start mask=0x%08lX", (unsigned long)drives);
    for (int i = 0; i < 26 && !g_indexCancel.load(); ++i) {
        if ((drives & (1u << i)) == 0) continue;
        wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        IndexVolumeUsn(hWnd, (wchar_t)(L'A' + i));
    }

    LOG_INFO(L"IndexAllFixedDrives done");
}

static void StopIndexing() {
    LOG_INFO(L"StopIndexing requested");
    g_indexCancel.store(true);
    if (g_indexThread.joinable()) {
        g_indexThread.join();
    }
    g_indexRunning.store(false);
    LOG_INFO(L"StopIndexing done");
}

static void StartIndexing(HWND hWnd) {
    LOG_INFO(L"StartIndexing requested");
    StopIndexing();
    g_indexCancel.store(false);
    g_indexRunning.store(true);

    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        g_rows.clear();
    }
    RefreshList();

    g_indexThread = std::thread([hWnd]() {
        LOG_INFO(L"Index thread started");
        IndexAllFixedDrives(hWnd);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);
        LOG_INFO(L"Index thread finished");
    });
}

static LRESULT CALLBACK SearchEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND hParent = GetParent(hWnd);
        if (hParent) {
            PostMessageW(hParent, WM_COMMAND, MAKEWPARAM(IDM_SEARCH_FIND, 0), 0);
        }
        return 0;
    }
    return CallWindowProcW(g_EditOldProc, hWnd, msg, wParam, lParam);
}

static void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        LOG_INFO(L"WM_CREATE");
        EnsureCommonControls();

        g_mainHwnd = hWnd;

        SetMenu(hWnd, CreateMainMenu());

        g_hSearch = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"*apk",
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
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
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

        g_EditOldProc = (WNDPROC)SetWindowLongPtrW(g_hSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);

        RefreshList();
        LayoutChildren(hWnd);

        StartIndexing(hWnd);
        return 0;
    }
    case WM_SIZE:
        LayoutChildren(hWnd);
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        LOG_INFO(L"WM_COMMAND id=%d", id);
        switch (id) {
        case IDM_FILE_EXIT:
            DestroyWindow(hWnd);
            return 0;
        case IDM_VIEW_REFRESH:
            StartIndexing(hWnd);
            return 0;
        case IDM_VIEW_STOP:
            StopIndexing();
            return 0;
        case IDM_SEARCH_FIND:
            RefreshList();
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

        DbEnqueue(*row);

        int index = -1;
        {
            std::lock_guard<std::mutex> lk(g_rowsMutex);
            g_rows.push_back(*row);
            index = (int)g_rows.size() - 1;
        }

        delete row;

        if (index >= 0) {
            const ResultRow r = [&]() {
                std::lock_guard<std::mutex> lk(g_rowsMutex);
                return g_rows[index];
            }();

            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(r.name.c_str());
            int idx = ListView_InsertItem(g_hList, &item);
            ListView_SetItemText(g_hList, idx, 1, const_cast<LPWSTR>(r.path.c_str()));
            ListView_SetItemText(g_hList, idx, 2, const_cast<LPWSTR>(r.size.c_str()));
            ListView_SetItemText(g_hList, idx, 3, const_cast<LPWSTR>(r.mtime.c_str()));
        }

        return 0;
    }

    case WM_APP_INDEX_DONE:
        g_indexRunning.store(false);
        LOG_INFO(L"WM_APP_INDEX_DONE");
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr && hdr->hwndFrom == g_hList) {
            if (hdr->code == NM_DBLCLK) {
                MessageBoxW(hWnd, L"Double click (placeholder).", L"Info", MB_OK);
                return 0;
            }
        }
        break;
    }

    case WM_DESTROY:
        LOG_INFO(L"WM_DESTROY");
        StopIndexing();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    Logger::Init();
    LOG_INFO(L"wWinMain start");

    if (!DbInit()) {
        LOG_WARN(L"DbInit failed; continue without database");
    }

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

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOG_INFO(L"Message loop exit code=%lld", (long long)msg.wParam);
    DbShutdown();
    Logger::Shutdown();
    return (int)msg.wParam;
}
