#include <windows.h>
#include <commctrl.h>

#include "logger.h"
#include "main_window.h"
#include "string_utils.h"
#include "indexer.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

// 启用 ComCtl32 v6 视觉样式（PBS_MARQUEE 进度条需要）
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


static constexpr wchar_t kAppClassName[] = L"EveryArchiveMainWindow";
static constexpr wchar_t kAppTitle[] = L"EveryArchive";

static void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
}

// ══════════════════════════════════════════════════════════════
//  程序入口
// ══════════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    Logger::Init();
    LOG_INFO(L"wWinMain start");

    std::wstring dbPath = GetExeDir() + L"\\everyarchive.db";

    const bool p1 = Indexer::EnablePrivilege(SE_MANAGE_VOLUME_NAME);
    const bool p2 = Indexer::EnablePrivilege(SE_BACKUP_NAME);
    LOG_INFO(L"EnablePrivilege SeManageVolume=%d SeBackup=%d", (int)p1, (int)p2);

    EnsureCommonControls();

    // 创建主窗口状态（通过 CREATESTRUCT::lpCreateParams 传递给 WndProc）
    MainWindowState state;
    state.hInstance = hInstance;
    state.dbPath = dbPath;
    state.indexer.SetDbPath(dbPath);
    state.rowCache.SetDbPath(dbPath);
    state.rowCache.SetIconCache(&state.iconCache);

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
        &state);  // 传递 state 指针

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
