#pragma once

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <string>
#include <unordered_map>

class IconCache {
public:
    // 根据文件名获取系统图标索引（按扩展名缓存，避免重复查询）
    int GetFileIconIndex(const std::wstring& fileName);

    // 初始化系统小图标 ImageList 并关联到 ListView
    HIMAGELIST InitSysImageList();

    HIMAGELIST GetSysSmallIcons() const { return hSysSmallIcons_; }

private:
    HIMAGELIST hSysSmallIcons_ = nullptr;
    std::unordered_map<std::wstring, int> cache_;
};
