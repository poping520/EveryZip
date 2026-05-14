#pragma once

#include "advconfig.h"

// ============================================================================
// UserConfig — 应用层配置（基于 ConfigParser）
// ============================================================================

class UserConfig {
public:
    enum class LanguageMode {
        System,
        ZhCN,
        EnUS
    };

    struct ArchiveFormatRule {
        std::wstring extension;
        std::wstring parser;
        bool enabled = false;
        std::wstring group;
    };

    struct WindowPlacementConfig {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        bool maximized = false;
    };

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

    const std::vector<ArchiveFormatRule>& GetArchiveFormatRules() const;

    std::wstring GetParserForExtension(const std::wstring& extension) const;

    std::wstring GetParserForPath(const std::wstring& path) const;

    /**
     * 获取限定扫描的盘符列表（例如 {L'G'}）。为空时扫描所有 NTFS 盘。
     * @return 当前扫描盘符列表的只读引用。
     */
    const std::vector<wchar_t>& GetScanDriveLetters() const;

    /**
     * 获取主列表“归档文件”列是否显示完整路径。
     * @return 显示完整路径返回 true；仅显示文件名返回 false。
     */
    bool GetShowArchiveFullPath() const;

    bool GetRememberUiState() const;

    bool GetStartupScanConfirmed() const;

    LanguageMode GetLanguageMode() const;

    std::wstring GetLanguageConfigValue() const;

    const WindowPlacementConfig& GetWindowPlacement() const;

    const std::vector<int>& GetListColumnWidths() const;

    static const std::vector<int>& GetDefaultListColumnWidths();

    void SetArchiveFormatRules(const std::vector<ArchiveFormatRule>& rules);

    static std::wstring NormalizeArchiveExtension(const std::wstring& ext);

    static bool IsValidCustomArchiveExtension(const std::wstring& ext);

    /**
     * 设置限定扫描的盘符列表。为空时扫描所有 NTFS 盘。
     * @param drives 新的扫描盘符列表。
     */
    void SetScanDriveLetters(const std::vector<wchar_t>& drives);

    /**
     * 设置主列表“归档文件”列是否显示完整路径。
     * @param showFullPath true 显示完整路径，false 仅显示文件名。
     */
    void SetShowArchiveFullPath(bool showFullPath);

    void SetRememberUiState(bool remember);

    void SetStartupScanConfirmed(bool confirmed);

    void SetLanguageMode(LanguageMode mode);

    void SetWindowPlacement(const WindowPlacementConfig& placement);

    void SetListColumnWidths(const std::vector<int>& widths);

    void ResetListColumnWidths();

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
    std::vector<ArchiveFormatRule> archiveFormatRules_;
    std::vector<wchar_t> scanDriveLetters_;
    bool showArchiveFullPath_ = false;
    bool rememberUiState_ = true;
    bool startupScanConfirmed_ = false;
    LanguageMode languageMode_ = LanguageMode::System;
    WindowPlacementConfig windowPlacement_;
    std::vector<int> listColumnWidths_;
    AdvConfig::Parser parser_;
    bool configMigrated_ = false;
};
