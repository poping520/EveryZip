#pragma once

#include "advconfig.h"

// ============================================================================
// UserConfig — 应用层配置（基于 ConfigParser）
// ============================================================================

class UserConfig {
public:
    /** 构造用户配置对象并填充默认配置值。 */
    UserConfig();
    /** 析构用户配置对象。 */
    ~UserConfig();

    UserConfig(const UserConfig&) = delete;
    UserConfig& operator=(const UserConfig&) = delete;

    /**
     * 加载配置文件，如果文件不存在则创建默认配置。
     * @param configPath 配置文件路径。
     * @param err 可选，用于输出错误信息。
     * @return 加载成功返回 true，否则返回 false。
     */
    bool Load(const std::wstring& configPath, std::wstring* err = nullptr);

    /**
     * 保存当前配置到文件。
     * @param err 可选，用于输出错误信息。
     * @return 保存成功返回 true，否则返回 false。
     */
    bool Save(std::wstring* err = nullptr) const;

    /**
     * 获取归档文件扩展名列表（例如 {L".zip", L".rar"}）。
     * @return 当前归档扩展名列表的只读引用。
     */
    const std::vector<std::wstring>& GetArchiveExtensions() const;

    /**
     * 设置归档扩展名列表。
     * @param exts 新的扩展名列表。
     */
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    /**
     * 获取配置文件路径。
     * @return 当前配置文件路径的只读引用。
     */
    const std::wstring& GetConfigPath() const { return configPath_; }

private:
    /** 将解析器中的键值同步到成员变量。 */
    void SyncFromParser();

    /** 将成员变量同步回底层解析器。 */
    void SyncToParser();

    std::wstring configPath_;
    std::vector<std::wstring> archiveExtensions_;
    AdvConfig::Parser parser_;
    bool configMigrated_ = false;
};
