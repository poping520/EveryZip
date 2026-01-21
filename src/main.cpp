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
#include "parser/zip_archive_parser.h"

#include "file_scanner.h"
#include "database.h"

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

static Database g_database;
static std::wstring g_dbPath;

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

static FILETIME U64ToFileTime(uint64_t v) {
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)((v >> 32) & 0xFFFFFFFFu);
    return ft;
}

static void LoadRowsFromDbAndRefresh() {
    std::wstring err;
    if (!g_database.Open(g_dbPath, &err)) {
        LOG_WARN(L"Database::Open failed: %s", err.c_str());
        return;
    }

    std::vector<ArchiveFile_t> files;
    const std::wstring filter = GetSearchFilter();
    if (!g_database.QueryArchives(filter, &files, &err)) {
        LOG_WARN(L"QueryArchives failed: %s", err.c_str());
        return;
    }

    std::vector<ResultRow> rows;
    rows.reserve(files.size());
    for (const auto& f : files) {
        ResultRow r;
        r.name = f.name;
        r.path = f.path;
        r.size = FormatSizeULongLong((ULONGLONG)f.size);
        const FILETIME ft = U64ToFileTime(f.modifyTimestamp);
        r.mtime = FormatFileTimeLocal(ft);
        rows.push_back(std::move(r));
    }

    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        g_rows = std::move(rows);
    }

    RefreshList();
}

static void PostAddResult(HWND hWnd, ResultRow* row) {
    PostMessageW(hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)row);
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
        FileScanner scanner;
        std::vector<ArchiveFile_t> files;
        std::wstring err;
        if (!scanner.Scan(&files, &err)) {
            LOG_WARN(L"FileScanner::Scan failed: %s", err.c_str());
        }

        if (!g_indexCancel.load()) {
            if (!g_database.Open(g_dbPath, &err)) {
                LOG_WARN(L"Database::Open failed: %s", err.c_str());
            } else {
                if (!g_database.CreateArchivesTable(&err)) {
                    LOG_WARN(L"CreateArchivesTable failed: %s", err.c_str());
                } else {
                    if (!g_database.InsertArchivesBatch(files, &err)) {
                        LOG_WARN(L"InsertArchivesBatch failed: %s", err.c_str());
                    }
                }
            }
        }

        PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);
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

    case WM_APP_DB_REFRESH:
        LoadRowsFromDbAndRefresh();
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

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOG_INFO(L"Message loop exit code=%lld", (long long)msg.wParam);
    Logger::Shutdown();
    return (int)msg.wParam;
}
