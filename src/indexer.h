#pragma once

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "file_scanner.h"

class Indexer {
public:
    Indexer();
    ~Indexer();

    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    // 设置数据库路径
    void SetDbPath(const std::wstring& dbPath);

    // 设置归档文件扩展名（从 UserConfig 加载后传入）
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    // 确保数据库文件存在并创建所需的表
    bool EnsureDatabaseReady();

    // 启动后台索引线程：扫描磁盘 → 增量更新数据库 → 解析归档条目 → 进入监控循环
    void Start(HWND hWnd);

    // 停止后台索引线程（设置取消标志并等待线程结束）
    void Stop();

    // 运行状态
    bool IsRunning() const { return running_.load(); }

    // 提升进程权限（用于 USN Journal 扫描需要的 SeManageVolume / SeBackup）
    static bool EnablePrivilege(const wchar_t* privilegeName);

private:
    // 解析单个归档文件并将条目写入数据库（先删除旧条目再插入新条目）
    static void ParseAndStoreArchive(Database& db, const ArchiveFile_t& a);

    // 获取所有需要监控的 NTFS 盘符列表
    static std::vector<wchar_t> GetMonitoredDrives();

    std::wstring dbPath_;
    std::vector<std::wstring> archiveExtensions_ = { L".zip" };
    std::atomic_bool cancel_{ false };
    std::atomic_bool running_{ false };
    std::thread thread_;
};
