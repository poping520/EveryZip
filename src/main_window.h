#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "database.h"
#include "icon_cache.h"
#include "indexer.h"
#include "row_cache.h"

// 从 STRINGTABLE 资源加载本地化字符串（只读指针，不以 '\0' 结尾）
const wchar_t* S(HINSTANCE hInstance, UINT id);
// 从 STRINGTABLE 资源加载本地化字符串（拷贝到 std::wstring）
std::wstring LS(HINSTANCE hInstance, UINT id);

// ── 异步加载结果（后台线程只查询 rowid 列表，传递到 UI 线程）──
struct AsyncLoadResult {
    std::vector<int64_t> rowIds;
    int64_t archiveCount = 0;
    int64_t entryCount = 0;
};

// 主窗口状态（通过 GWLP_USERDATA 附加到窗口）
struct MainWindowState {
    HINSTANCE hInstance = nullptr;
    std::wstring dbPath;

    // UI 控件句柄
    HWND hSearch = nullptr;
    HWND hList = nullptr;
    HWND hStatusBar = nullptr;
    HWND hProgress = nullptr;
    WNDPROC editOldProc = nullptr;
    HFONT hSearchFont = nullptr;
    HFONT hNormalFont = nullptr;
    HFONT hBoldFont = nullptr;

    // ListView 数据源
    std::vector<int64_t> rowIds;
    std::mutex rowsMutex;
    int64_t totalEntryCount = 0;

    // 列头排序状态
    int sortColumn = -1;
    bool sortAscending = true;

    // 缓存的归档文件数量
    std::atomic<int64_t> cachedArchiveCount{ 0 };

    // 模块
    IconCache iconCache;
    RowCache rowCache;
    Indexer indexer;

    // 托盘/退出
    bool forceQuit = false;
};

// 主窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
