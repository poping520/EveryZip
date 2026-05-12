#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "database.h"
#include "icon_cache.h"

/** ListView 行缓存项（按需从数据库加载，缓存可见行数据）。 */
struct CachedRow {
    std::wstring name;
    std::wstring archivePath;
    std::wstring entryPath;
    std::string entryRawPath;
    std::wstring sizeStr;       // 格式化后的压缩大小
    std::wstring origSizeStr;   // 格式化后的原始大小
    std::wstring modifiedTimeStr;
    int iconIndex = 0;
};

class RowCache {
public:
    /**
     * 创建行缓存对象并指定最大缓存容量。
     * @param maxSize LRU 缓存最多保留的行数。
     */
    explicit RowCache(size_t maxSize = 2000);

    /**
     * 设置数据库路径（延迟打开连接）。
     * @param dbPath 行数据查询所使用的 SQLite 数据库文件路径。
     */
    void SetDbPath(const std::wstring& dbPath);

    /**
     * 设置图标缓存引用（用于获取文件图标索引）。
     * @param iconCache 图标缓存对象指针，可为空。
     */
    void SetIconCache(IconCache* iconCache);

    /**
     * 按 rowid 从 LRU 缓存中获取行数据，缓存未命中时从数据库查询。
     * @param rowId entries 表中的行标识。
     * @return 缓存项指针；查询失败时返回 nullptr。
     */
    const CachedRow* Get(int64_t rowId);

    /** 清空行数据缓存（排序或搜索条件变化时调用）。 */
    void Clear();

    /** 关闭数据库连接。 */
    void Close();

private:
    /** 确保缓存内部数据库连接已经打开。 */
    void EnsureDbOpen();

    size_t maxSize_;
    std::wstring dbPath_;
    Database cacheDb_;
    bool dbOpen_ = false;
    IconCache* iconCache_ = nullptr;

    std::unordered_map<int64_t, CachedRow> cache_;
    std::deque<int64_t> lru_;
};
