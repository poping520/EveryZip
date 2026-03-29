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
 // 参数：hInstance - 模块实例句柄；id - 字符串资源编号。
 // 返回值：指向资源字符串的只读指针；加载失败时返回空字符串常量。
const wchar_t* S(HINSTANCE hInstance, UINT id);
// 从 STRINGTABLE 资源加载本地化字符串（拷贝到 std::wstring）
 // 参数：hInstance - 模块实例句柄；id - 字符串资源编号。
 // 返回值：拷贝后的本地化字符串；加载失败时返回空字符串。
std::wstring LS(HINSTANCE hInstance, UINT id);

// ── 异步加载结果（后台线程只查询 rowid 列表，传递到 UI 线程）──
struct AsyncLoadResult {
    std::vector<int64_t> rowIds;
    int64_t archiveCount = 0;
    int64_t entryCount = 0;
};

// ── 异步解压结果（后台线程解压完成后传递到 UI 线程）──
struct ExtractResult {
    bool success = false;
    std::wstring destDir;
    std::string errorMsg;
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

    // 转圈动画角度（0~359，每帧递增）
    int spinnerAngle = 0;

    // 标记是否有行数据加载失败，需要延迟重试刷新列表
    bool needsListRetry = false;
};

// 主窗口过程
 // 参数：hWnd - 当前窗口句柄；msg - 消息编号；wParam - 消息参数；lParam - 消息参数。
 // 返回值：消息处理结果。
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
