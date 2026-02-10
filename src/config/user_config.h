#pragma once

#include "advconfig.h"

// ============================================================================
// UserConfig — 应用层配置（基于 ConfigParser）
// ============================================================================

class UserConfig {
public:
    UserConfig();
    ~UserConfig();

    UserConfig(const UserConfig&) = delete;
    UserConfig& operator=(const UserConfig&) = delete;

    // 加载配置文件，如果文件不存在则创建默认配置
    bool Load(const std::wstring& configPath, std::wstring* err = nullptr);

    // 保存当前配置到文件
    bool Save(std::wstring* err = nullptr) const;

    // 归档文件扩展名（例如 {L".zip", L".rar"}）
    const std::vector<std::wstring>& GetArchiveExtensions() const;
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    // 配置文件路径
    const std::wstring& GetConfigPath() const { return configPath_; }

private:
    void SyncFromParser();
    void SyncToParser();

    std::wstring configPath_;
    std::vector<std::wstring> archiveExtensions_;
    AdvConfig::Parser parser_;
};
