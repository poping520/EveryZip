#include "user_config.h"

#include <algorithm>
#include <cwctype>
#include <sstream>

#include "../logger.h"

using namespace AdvConfig;

// ============================================================================
// UserConfig 实现（基于 ConfigParser）
// ============================================================================

static const wchar_t* kKeyArchiveExtensions = L"archive_extensions";

// 默认归档扩展名
static const std::vector<std::wstring> kDefaultArchiveExtensions = {
    L".zip", L".apk", L".7z", L".rar"
};

UserConfig::UserConfig()
    : archiveExtensions_(kDefaultArchiveExtensions)
{
}

UserConfig::~UserConfig() = default;

static bool HasExtension(const std::vector<std::wstring>& exts, const std::wstring& ext)
{
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

static bool IsDefaultExtensionsBeforeRar(const std::vector<std::wstring>& exts)
{
    const bool hasZip = HasExtension(exts, L".zip");
    const bool hasApk = HasExtension(exts, L".apk");
    const bool has7z = HasExtension(exts, L".7z");
    return (exts.size() == 2 && hasZip && hasApk) ||
           (exts.size() == 3 && hasZip && hasApk && has7z);
}

const std::vector<std::wstring>& UserConfig::GetArchiveExtensions() const
{
    return archiveExtensions_;
}

void UserConfig::SetArchiveExtensions(const std::vector<std::wstring>& exts)
{
    archiveExtensions_ = exts;
}

void UserConfig::SyncFromParser()
{
    bool parsedConfig = false;
    configMigrated_ = false;

    const Value& val = parser_.Get(kKeyArchiveExtensions);
    if (val.IsList()) {
        parsedConfig = true;
        archiveExtensions_.clear();
        for (const auto& item : val.AsList()) {
            if (!item.IsString()) continue;
            std::wstring wext = item.AsString();
            if (!wext.empty() && wext[0] != L'.') {
                wext = L"." + wext;
            }
            std::transform(wext.begin(), wext.end(), wext.begin(), std::towlower);
            archiveExtensions_.push_back(std::move(wext));
        }
        if (archiveExtensions_.empty()) {
            archiveExtensions_ = kDefaultArchiveExtensions;
        }
    } else if (val.IsString()) {
        parsedConfig = true;
        // 兼容旧格式：逗号分隔
        archiveExtensions_.clear();
        std::wistringstream extStream(val.AsString());
        std::wstring ext;
        while (std::getline(extStream, ext, L',')) {
            size_t s = ext.find_first_not_of(L" \t");
            size_t e = ext.find_last_not_of(L" \t");
            if (s == std::wstring::npos) continue;
            ext = ext.substr(s, e - s + 1);
            if (ext.empty()) continue;
            if (ext[0] != L'.') {
                ext = L"." + ext;
            }
            std::transform(ext.begin(), ext.end(), ext.begin(), std::towlower);
            archiveExtensions_.push_back(std::move(ext));
        }
        if (archiveExtensions_.empty()) {
            archiveExtensions_ = kDefaultArchiveExtensions;
        }
    }

    if (parsedConfig && IsDefaultExtensionsBeforeRar(archiveExtensions_)) {
        if (!HasExtension(archiveExtensions_, L".7z")) {
            archiveExtensions_.push_back(L".7z");
        }
        archiveExtensions_.push_back(L".rar");
        SyncToParser();
        configMigrated_ = true;
    }
}

void UserConfig::SyncToParser()
{
    Value::List list;
    for (const auto& ext : archiveExtensions_) {
        list.push_back(Value(ext));
    }
    parser_.Set(kKeyArchiveExtensions, Value(std::move(list)));
}

bool UserConfig::Load(const std::wstring& configPath, std::wstring* err)
{
    if (err) err->clear();
    configPath_ = configPath;

    std::wstring parseErr;
    if (!parser_.LoadFile(configPath, &parseErr)) {
        // 文件不存在时创建默认配置
        LOG_INFO(L"Config file not found, creating default: %s", configPath.c_str());
        archiveExtensions_ = kDefaultArchiveExtensions;
        SyncToParser();
        return Save(err);
    }

    SyncFromParser();
    if (configMigrated_) {
        std::wstring saveErr;
        if (!Save(&saveErr)) {
            LOG_WARN(L"Config migration save failed: %s", saveErr.c_str());
        }
    }

    LOG_INFO(L"Config loaded: %zu archive extensions", archiveExtensions_.size());
    return true;
}

bool UserConfig::Save(std::wstring* err) const
{
    if (err) err->clear();
    if (configPath_.empty()) {
        if (err) *err = L"Config path is empty";
        return false;
    }

    std::wstring saveErr;
    if (!parser_.SaveFile(configPath_, &saveErr)) {
        if (err) *err = saveErr;
        return false;
    }

    return true;
}
