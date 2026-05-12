#include "row_cache.h"

#include <algorithm>

#include "string_utils.h"

RowCache::RowCache(size_t maxSize) : maxSize_(maxSize) {}

void RowCache::SetDbPath(const std::wstring& dbPath) {
    dbPath_ = dbPath;
}

void RowCache::SetIconCache(IconCache* iconCache) {
    iconCache_ = iconCache;
}

void RowCache::EnsureDbOpen() {
    if (!dbOpen_) {
        std::wstring err;
        dbOpen_ = cacheDb_.Open(dbPath_, &err);
        if (dbOpen_) {
            cacheDb_.SetBusyTimeout(500);
        }
    }
}

const CachedRow* RowCache::Get(int64_t rowId) {
    // 缓存命中：提升到 LRU 队列头部
    auto it = cache_.find(rowId);
    if (it != cache_.end()) {
        auto lruIt = std::find(lru_.begin(), lru_.end(), rowId);
        if (lruIt != lru_.end()) {
            lru_.erase(lruIt);
        }
        lru_.push_front(rowId);
        return &it->second;
    }

    // 缓存未命中：从数据库查询
    EnsureDbOpen();
    if (!dbOpen_) return nullptr;

    ArchiveEntry_t entry;
    if (!cacheDb_.QueryEntryById(rowId, &entry)) {
        return nullptr;
    }

    // 构建缓存项
    CachedRow cr;
    cr.name = GetEntryNameFromPath(entry.entryPath);
    cr.archivePath = entry.archivePath;
    cr.entryPath = entry.entryPath;
    cr.entryRawPath = entry.entryRawPath;
    cr.sizeStr = entry.compressed_size < 0 ? L"-" : FormatSizeULongLong((ULONGLONG)entry.compressed_size);
    cr.origSizeStr = FormatSizeULongLong((ULONGLONG)entry.original_size);
    cr.modifiedTimeStr = FormatFileTimeValueLocal(entry.modifiedTime);
    cr.iconIndex = iconCache_ ? iconCache_->GetFileIconIndex(cr.name) : 0;

    // 插入缓存并维护 LRU
    auto [inserted, _] = cache_.emplace(rowId, std::move(cr));
    lru_.push_front(rowId);

    // 超出容量时淘汰最旧的
    while (cache_.size() > maxSize_ && !lru_.empty()) {
        int64_t oldest = lru_.back();
        lru_.pop_back();
        cache_.erase(oldest);
    }

    return &inserted->second;
}

void RowCache::Clear() {
    cache_.clear();
    lru_.clear();
}

void RowCache::Close() {
    cacheDb_.Close();
    dbOpen_ = false;
}
