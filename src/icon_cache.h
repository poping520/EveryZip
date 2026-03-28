#pragma once

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <string>
#include <unordered_map>

class IconCache {
public:
    /**
     * 根据文件名获取系统图标索引（按扩展名缓存，避免重复查询）。
     * @param fileName 需要显示图标的文件名或条目名。
     * @return 系统小图标列表中的图标索引。
     */
    int GetFileIconIndex(const std::wstring& fileName);

    /**
     * 初始化系统小图标 ImageList 并关联到 ListView。
     * @return 系统提供的小图标 ImageList 句柄。
     */
    HIMAGELIST InitSysImageList();

    /**
     * 获取当前缓存的系统小图标列表句柄。
     * @return 系统小图标列表句柄；尚未初始化时可能为 nullptr。
     */
    HIMAGELIST GetSysSmallIcons() const { return hSysSmallIcons_; }

private:
    HIMAGELIST hSysSmallIcons_ = nullptr;
    std::unordered_map<std::wstring, int> cache_;
};
