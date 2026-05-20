#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../database.h"
#include "../config/user_config.h"
#include "../icon_cache.h"
#include "../indexer.h"
#include "../row_cache.h"

LANGID GetEffectiveLanguageId(const UserConfig& config);

// 从 STRINGTABLE 资源加载本地化字符串（拷贝到 std::wstring）。
std::wstring LS(HINSTANCE hInstance, UINT id);
std::wstring LS(HINSTANCE hInstance, UINT id, LANGID languageId);

// ── 异步加载结果（后台线程只查询 rowid 列表，传递到 UI 线程）──
struct AsyncLoadResult {
    uint64_t generation = 0;
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
    UserConfig userConfig;
    bool showArchiveFullPath = false;

    // UI 控件句柄
    HWND hSearch = nullptr;
    HWND hList = nullptr;
    HWND hStatusBar = nullptr;
    HWND hProgress = nullptr;
    HWND hArchiveTooltip = nullptr;
    WNDPROC editOldProc = nullptr;
    WNDPROC listOldProc = nullptr;
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
    std::atomic<int64_t> parseDoneCount{ 0 };
    std::atomic<int64_t> parseTotalCount{ 0 };
    std::atomic<int> pendingRowLoads{ 0 };
    std::atomic_bool updateCheckInProgress{ false };
    std::atomic_bool updateDownloadInProgress{ false };
    std::atomic_bool updatePromptVisible{ false };

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
    int listRetryAttempts = 0;
    bool listRetrySuppressedLogged = false;
    int archiveTooltipItem = -1;
    int archiveTooltipSubItem = -1;
    bool archiveTooltipTracking = false;
    std::wstring archiveTooltipText;

    std::atomic_bool shuttingDown{ false };
    std::atomic_uint64_t loadGeneration{ 0 };
    std::mutex asyncThreadsMutex;
    std::vector<std::thread> asyncThreads;
};

// 主窗口过程
 // 参数：hWnd - 当前窗口句柄；msg - 消息编号；wParam - 消息参数；lParam - 消息参数。
 // 返回值：消息处理结果。
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
