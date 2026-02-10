#include "icon_cache.h"

#include <cwctype>

int IconCache::GetFileIconIndex(const std::wstring& fileName) {
    // 提取扩展名（包含点，如 ".xml"）并转小写
    std::wstring ext;
    size_t dotPos = fileName.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        ext = fileName.substr(dotPos);
        for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    }

    // 查找缓存
    auto it = cache_.find(ext);
    if (it != cache_.end()) {
        return it->second;
    }

    // 使用 SHGetFileInfo 按扩展名获取系统图标索引（SHGFI_USEFILEATTRIBUTES 无需文件实际存在）
    SHFILEINFOW sfi{};
    std::wstring fakeName = L"file" + ext;
    DWORD_PTR ret = SHGetFileInfoW(
        fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

    int idx = (ret != 0) ? sfi.iIcon : 0;
    cache_[ext] = idx;
    return idx;
}

HIMAGELIST IconCache::InitSysImageList() {
    SHFILEINFOW sfi{};
    hSysSmallIcons_ = (HIMAGELIST)SHGetFileInfoW(
        L"", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    return hSysSmallIcons_;
}
