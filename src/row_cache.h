#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "database.h"
#include "icon_cache.h"

// ListView 行缓存项（按需从数据库加载，缓存可见行数据）
struct CachedRow {
    std::wstring name;
    std::wstring archivePath;
    std::wstring entryPath;
    std::wstring sizeStr;       // 格式化后的压缩大小
    std::wstring origSizeStr;   // 格式化后的原始大小
    int iconIndex = 0;
};

class RowCache {
public:
    explicit RowCache(size_t maxSize = 2000);

    // 设置数据库路径（延迟打开连接）
    void SetDbPath(const std::wstring& dbPath);

    // 设置图标缓存引用（用于获取文件图标索引）
    void SetIconCache(IconCache* iconCache);

    // 按 rowid 从 LRU 缓存中获取行数据，缓存未命中时从数据库查询
    const CachedRow* Get(int64_t rowId);

    // 清空行数据缓存（排序或搜索条件变化时调用）
    void Clear();

    // 关闭数据库连接
    void Close();

private:
    void EnsureDbOpen();

    size_t maxSize_;
    std::wstring dbPath_;
    Database cacheDb_;
    bool dbOpen_ = false;
    IconCache* iconCache_ = nullptr;

    std::unordered_map<int64_t, CachedRow> cache_;
    std::deque<int64_t> lru_;
};
