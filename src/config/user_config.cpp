#include "user_config.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <sstream>

#include "../logger.h"

using namespace AdvConfig;

// ============================================================================
// UserConfig 实现（基于 ConfigParser）
// ============================================================================

static const wchar_t* kKeyArchiveFormats = L"archive_formats";
static const wchar_t* kKeyScanDrives = L"scan_drives";
static const wchar_t* kKeyParseThreads = L"parse_threads";
static const wchar_t* kKeyShowArchiveFullPath = L"show_archive_full_path";
static const wchar_t* kKeyRememberUiState = L"remember_ui_state";
static const wchar_t* kKeyStartupScanConfirmed = L"startup_scan_confirmed";
static const wchar_t* kKeyAutoUpdateCheckEnabled = L"auto_update_check_enabled";
static const wchar_t* kKeyLastAutoUpdateCheckAt = L"last_auto_update_check_at";
static const wchar_t* kKeyLanguage = L"language";
static const wchar_t* kKeyWindowRect = L"window_rect";
static const wchar_t* kKeyWindowMaximized = L"window_maximized";
static const wchar_t* kKeyListColumnWidths = L"list_column_widths";

static const std::vector<UserConfig::ArchiveFormatRule> kBuiltInArchiveFormatRules = {
    { L".zip", L"zip", true,  L"default" },
    { L".rar", L"rar", true,  L"default" },
    { L".7z",  L"7z",  true,  L"default" },
    { L".apk", L"zip", false, L"known_aliases" },
    { L".ipa", L"zip", false, L"known_aliases" },
    { L".jar", L"zip", false, L"known_aliases" },
    { L".war", L"zip", false, L"known_aliases" }
};

// 测试阶段默认只扫描 G 盘；配置为空列表时扫描所有 NTFS 盘。
static const std::vector<wchar_t> kDefaultScanDriveLetters = {
    // L'G'
};

static const std::vector<int> kDefaultListColumnWidths = {
    210,
    210,
    280,
    100,
    100,
    140
};

static uint32_t NormalizeParseThreadCount(int64_t value)
{
    if (value <= 0) return 0;
    if (value > 16) return 16;
    return static_cast<uint32_t>(value);
}

UserConfig::UserConfig()
    : archiveFormatRules_(kBuiltInArchiveFormatRules),
      scanDriveLetters_(kDefaultScanDriveLetters),
      listColumnWidths_(kDefaultListColumnWidths)
{
}

UserConfig::~UserConfig() = default;

static bool IsValidParserName(const std::wstring& parser)
{
    return parser == L"zip" || parser == L"rar" || parser == L"7z";
}

static bool IsBuiltInArchiveExtension(const std::wstring& ext)
{
    for (const auto& rule : kBuiltInArchiveFormatRules) {
        if (rule.extension == ext) return true;
    }
    return false;
}

static wchar_t NormalizeDriveLetter(const std::wstring& drive)
{
    if (drive.empty()) return L'\0';
    wchar_t ch = drive[0];
    if (ch >= L'a' && ch <= L'z') {
        ch = (wchar_t)(ch - L'a' + L'A');
    }
    if (ch < L'A' || ch > L'Z') return L'\0';
    return ch;
}

static bool HasDriveLetter(const std::vector<wchar_t>& drives, wchar_t drive)
{
    return std::find(drives.begin(), drives.end(), drive) != drives.end();
}

static UserConfig::LanguageMode ParseLanguageMode(const std::wstring& value)
{
    std::wstring normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), std::towlower);
    if (normalized == L"zh-cn") return UserConfig::LanguageMode::ZhCN;
    if (normalized == L"en-us") return UserConfig::LanguageMode::EnUS;
    return UserConfig::LanguageMode::System;
}

static std::wstring LanguageModeToConfigValue(UserConfig::LanguageMode mode)
{
    switch (mode) {
    case UserConfig::LanguageMode::ZhCN:
        return L"zh-CN";
    case UserConfig::LanguageMode::EnUS:
        return L"en-US";
    case UserConfig::LanguageMode::System:
    default:
        return L"system";
    }
}

const std::vector<UserConfig::ArchiveFormatRule>& UserConfig::GetArchiveFormatRules() const
{
    return archiveFormatRules_;
}

std::wstring UserConfig::GetParserForExtension(const std::wstring& extension) const
{
    const std::wstring normalized = NormalizeArchiveExtension(extension);
    if (normalized.empty()) return {};

    for (const auto& rule : archiveFormatRules_) {
        if (rule.enabled && rule.extension == normalized) {
            return rule.parser;
        }
    }
    return {};
}

std::wstring UserConfig::GetParserForPath(const std::wstring& path) const
{
    const size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};
    return GetParserForExtension(path.substr(dotPos));
}

const std::vector<wchar_t>& UserConfig::GetScanDriveLetters() const
{
    return scanDriveLetters_;
}

uint32_t UserConfig::GetParseThreadCount() const
{
    return parseThreadCount_;
}

bool UserConfig::GetShowArchiveFullPath() const
{
    return showArchiveFullPath_;
}

bool UserConfig::GetRememberUiState() const
{
    return rememberUiState_;
}

bool UserConfig::GetStartupScanConfirmed() const
{
    return startupScanConfirmed_;
}

bool UserConfig::GetAutoUpdateCheckEnabled() const
{
    return autoUpdateCheckEnabled_;
}

int64_t UserConfig::GetLastAutoUpdateCheckAt() const
{
    return lastAutoUpdateCheckAt_;
}

UserConfig::LanguageMode UserConfig::GetLanguageMode() const
{
    return languageMode_;
}

std::wstring UserConfig::GetLanguageConfigValue() const
{
    return LanguageModeToConfigValue(languageMode_);
}

const UserConfig::WindowPlacementConfig& UserConfig::GetWindowPlacement() const
{
    return windowPlacement_;
}

const std::vector<int>& UserConfig::GetListColumnWidths() const
{
    return listColumnWidths_;
}

const std::vector<int>& UserConfig::GetDefaultListColumnWidths()
{
    return kDefaultListColumnWidths;
}

void UserConfig::SetArchiveFormatRules(const std::vector<ArchiveFormatRule>& rules)
{
    archiveFormatRules_.clear();

    for (const auto& builtIn : kBuiltInArchiveFormatRules) {
        ArchiveFormatRule rule = builtIn;
        for (const auto& candidate : rules) {
            const std::wstring ext = NormalizeArchiveExtension(candidate.extension);
            if (ext == builtIn.extension) {
                rule.enabled = candidate.enabled;
                break;
            }
        }
        archiveFormatRules_.push_back(std::move(rule));
    }

    for (const auto& candidate : rules) {
        const std::wstring ext = NormalizeArchiveExtension(candidate.extension);
        if (!IsValidCustomArchiveExtension(ext) || IsBuiltInArchiveExtension(ext)) {
            continue;
        }

        std::wstring parser = candidate.parser;
        std::transform(parser.begin(), parser.end(), parser.begin(), std::towlower);
        if (!IsValidParserName(parser)) {
            parser = L"zip";
        }

        bool exists = false;
        for (const auto& rule : archiveFormatRules_) {
            if (rule.extension == ext) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            archiveFormatRules_.push_back({ ext, parser, candidate.enabled, L"custom" });
        }
    }

    SyncToParser();
}

std::wstring UserConfig::NormalizeArchiveExtension(const std::wstring& ext)
{
    std::wstring out = ext;
    const size_t first = out.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = out.find_last_not_of(L" \t\r\n");
    out = out.substr(first, last - first + 1);

    if (!out.empty() && out[0] != L'.') {
        out.insert(out.begin(), L'.');
    }
    std::transform(out.begin(), out.end(), out.begin(), std::towlower);
    return out;
}

bool UserConfig::IsValidCustomArchiveExtension(const std::wstring& ext)
{
    const std::wstring normalized = NormalizeArchiveExtension(ext);
    if (normalized.size() < 2) return false;

    static constexpr const wchar_t* kInvalidChars = L"<>:\"/\\|?*";
    for (wchar_t ch : normalized) {
        if (wcschr(kInvalidChars, ch)) return false;
        if (ch < 32) return false;
    }
    return normalized.find(L'.', 1) == std::wstring::npos;
}

void UserConfig::SetScanDriveLetters(const std::vector<wchar_t>& drives)
{
    scanDriveLetters_.clear();
    for (wchar_t drive : drives) {
        std::wstring s(1, drive);
        wchar_t normalized = NormalizeDriveLetter(s);
        if (normalized && !HasDriveLetter(scanDriveLetters_, normalized)) {
            scanDriveLetters_.push_back(normalized);
        }
    }
    SyncToParser();
}

void UserConfig::SetParseThreadCount(uint32_t threads)
{
    parseThreadCount_ = NormalizeParseThreadCount(threads);
    SyncToParser();
}

void UserConfig::SetShowArchiveFullPath(bool showFullPath)
{
    showArchiveFullPath_ = showFullPath;
    SyncToParser();
}

void UserConfig::SetRememberUiState(bool remember)
{
    rememberUiState_ = remember;
    SyncToParser();
}

void UserConfig::SetStartupScanConfirmed(bool confirmed)
{
    startupScanConfirmed_ = confirmed;
    SyncToParser();
}

void UserConfig::SetAutoUpdateCheckEnabled(bool enabled)
{
    autoUpdateCheckEnabled_ = enabled;
    SyncToParser();
}

void UserConfig::SetLastAutoUpdateCheckAt(int64_t timestamp)
{
    lastAutoUpdateCheckAt_ = std::max<int64_t>(0, timestamp);
    SyncToParser();
}

void UserConfig::SetLanguageMode(LanguageMode mode)
{
    languageMode_ = mode;
    SyncToParser();
}

void UserConfig::SetWindowPlacement(const WindowPlacementConfig& placement)
{
    windowPlacement_ = placement;
    SyncToParser();
}

void UserConfig::SetListColumnWidths(const std::vector<int>& widths)
{
    if (widths.size() == kDefaultListColumnWidths.size() &&
        std::all_of(widths.begin(), widths.end(), [](int width) { return width > 0; })) {
        listColumnWidths_ = widths;
    } else {
        listColumnWidths_ = kDefaultListColumnWidths;
    }
    SyncToParser();
}

void UserConfig::ResetListColumnWidths()
{
    listColumnWidths_ = kDefaultListColumnWidths;
    SyncToParser();
}

static void ApplyConfiguredRule(const AdvConfig::Value& value,
                                const std::wstring& group,
                                std::vector<UserConfig::ArchiveFormatRule>* rules)
{
    if (!value.IsDict() || !rules) return;

    const auto& dict = value.AsDict();
    auto extIt = dict.find(L"extension");
    auto parserIt = dict.find(L"parser");
    auto enabledIt = dict.find(L"enabled");
    if (extIt == dict.end() || !extIt->second.IsString()) return;

    const std::wstring ext = UserConfig::NormalizeArchiveExtension(extIt->second.AsString());
    if (ext.empty()) return;

    std::wstring parser = L"zip";
    if (parserIt != dict.end() && parserIt->second.IsString()) {
        parser = parserIt->second.AsString();
        std::transform(parser.begin(), parser.end(), parser.begin(), std::towlower);
    }
    if (!IsValidParserName(parser)) {
        parser = L"zip";
    }

    bool enabled = false;
    if (enabledIt != dict.end() && enabledIt->second.IsBool()) {
        enabled = enabledIt->second.AsBool(false);
    }

    for (auto& rule : *rules) {
        if (rule.extension == ext) {
            rule.enabled = enabled;
            if (rule.group == L"custom") {
                rule.parser = parser;
            }
            return;
        }
    }

    if (group == L"custom" && UserConfig::IsValidCustomArchiveExtension(ext) && !IsBuiltInArchiveExtension(ext)) {
        rules->push_back({ ext, parser, enabled, L"custom" });
    }
}

void UserConfig::SyncFromParser()
{
    configMigrated_ = false;

    archiveFormatRules_ = kBuiltInArchiveFormatRules;
    parseThreadCount_ = 0;
    const Value& formats = parser_.Get(kKeyArchiveFormats);
    if (formats.IsDict()) {
        const auto& groups = formats.AsDict();
        for (const auto& groupName : { L"default", L"known_aliases", L"custom" }) {
            auto it = groups.find(groupName);
            if (it == groups.end() || !it->second.IsList()) continue;
            for (const auto& item : it->second.AsList()) {
                ApplyConfiguredRule(item, groupName, &archiveFormatRules_);
            }
        }
    } else {
        configMigrated_ = true;
    }
    const Value& scanDrives = parser_.Get(kKeyScanDrives);
    if (scanDrives.IsList()) {
        scanDriveLetters_.clear();
        for (const auto& item : scanDrives.AsList()) {
            if (!item.IsString()) continue;
            wchar_t drive = NormalizeDriveLetter(item.AsString());
            if (drive && !HasDriveLetter(scanDriveLetters_, drive)) {
                scanDriveLetters_.push_back(drive);
            }
        }
    } else if (scanDrives.IsString()) {
        scanDriveLetters_.clear();
        std::wistringstream driveStream(scanDrives.AsString());
        std::wstring drive;
        while (std::getline(driveStream, drive, L',')) {
            size_t s = drive.find_first_not_of(L" \t");
            size_t e = drive.find_last_not_of(L" \t");
            if (s == std::wstring::npos) continue;
            drive = drive.substr(s, e - s + 1);
            wchar_t normalized = NormalizeDriveLetter(drive);
            if (normalized && !HasDriveLetter(scanDriveLetters_, normalized)) {
                scanDriveLetters_.push_back(normalized);
            }
        }
        SyncToParser();
        configMigrated_ = true;
    } else if (!parser_.Contains(kKeyScanDrives)) {
        scanDriveLetters_ = kDefaultScanDriveLetters;
        SyncToParser();
        configMigrated_ = true;
    }

    const Value& parseThreads = parser_.Get(kKeyParseThreads);
    if (parseThreads.IsInt()) {
        const uint32_t normalized = NormalizeParseThreadCount(parseThreads.AsInt());
        parseThreadCount_ = normalized;
        if (parseThreads.AsInt() != static_cast<int64_t>(normalized)) {
            configMigrated_ = true;
        }
    } else if (!parser_.Contains(kKeyParseThreads)) {
        parseThreadCount_ = 0;
        SyncToParser();
        configMigrated_ = true;
    } else {
        parseThreadCount_ = 0;
        configMigrated_ = true;
    }

    const Value& showFullPath = parser_.Get(kKeyShowArchiveFullPath);
    if (showFullPath.IsBool()) {
        showArchiveFullPath_ = showFullPath.AsBool(false);
    } else if (!parser_.Contains(kKeyShowArchiveFullPath)) {
        showArchiveFullPath_ = false;
        SyncToParser();
        configMigrated_ = true;
    }

    const Value& rememberUiState = parser_.Get(kKeyRememberUiState);
    if (rememberUiState.IsBool()) {
        rememberUiState_ = rememberUiState.AsBool(true);
    } else {
        rememberUiState_ = true;
        configMigrated_ = true;
    }

    const Value& startupScanConfirmed = parser_.Get(kKeyStartupScanConfirmed);
    if (startupScanConfirmed.IsBool()) {
        startupScanConfirmed_ = startupScanConfirmed.AsBool(false);
    } else {
        startupScanConfirmed_ = false;
        configMigrated_ = true;
    }

    const Value& autoUpdateCheckEnabled = parser_.Get(kKeyAutoUpdateCheckEnabled);
    if (autoUpdateCheckEnabled.IsBool()) {
        autoUpdateCheckEnabled_ = autoUpdateCheckEnabled.AsBool(true);
    } else {
        autoUpdateCheckEnabled_ = true;
        configMigrated_ = true;
    }

    const Value& lastAutoUpdateCheckAt = parser_.Get(kKeyLastAutoUpdateCheckAt);
    if (lastAutoUpdateCheckAt.IsInt()) {
        lastAutoUpdateCheckAt_ = std::max<int64_t>(0, lastAutoUpdateCheckAt.AsInt());
    } else {
        lastAutoUpdateCheckAt_ = 0;
        configMigrated_ = true;
    }

    const Value& language = parser_.Get(kKeyLanguage);
    if (language.IsString()) {
        const LanguageMode parsed = ParseLanguageMode(language.AsString());
        languageMode_ = parsed;
        if (language.AsString() != LanguageModeToConfigValue(parsed)) {
            configMigrated_ = true;
        }
    } else {
        languageMode_ = LanguageMode::System;
        configMigrated_ = true;
    }

    const Value& windowRect = parser_.Get(kKeyWindowRect);
    if (windowRect.IsDict()) {
        const auto& dict = windowRect.AsDict();
        auto leftIt = dict.find(L"left");
        auto topIt = dict.find(L"top");
        auto rightIt = dict.find(L"right");
        auto bottomIt = dict.find(L"bottom");
        if (leftIt != dict.end() && leftIt->second.IsInt() &&
            topIt != dict.end() && topIt->second.IsInt() &&
            rightIt != dict.end() && rightIt->second.IsInt() &&
            bottomIt != dict.end() && bottomIt->second.IsInt()) {
            windowPlacement_.left = static_cast<int>(leftIt->second.AsInt());
            windowPlacement_.top = static_cast<int>(topIt->second.AsInt());
            windowPlacement_.right = static_cast<int>(rightIt->second.AsInt());
            windowPlacement_.bottom = static_cast<int>(bottomIt->second.AsInt());
        } else {
            windowPlacement_ = WindowPlacementConfig{};
            configMigrated_ = true;
        }
    } else {
        windowPlacement_ = WindowPlacementConfig{};
        configMigrated_ = true;
    }

    const Value& windowMaximized = parser_.Get(kKeyWindowMaximized);
    if (windowMaximized.IsBool()) {
        windowPlacement_.maximized = windowMaximized.AsBool(false);
    } else {
        windowPlacement_.maximized = false;
        configMigrated_ = true;
    }

    const Value& listColumnWidths = parser_.Get(kKeyListColumnWidths);
    bool validColumnWidths = false;
    if (listColumnWidths.IsList() && listColumnWidths.AsList().size() == kDefaultListColumnWidths.size()) {
        std::vector<int> widths;
        widths.reserve(kDefaultListColumnWidths.size());
        validColumnWidths = true;
        for (const auto& item : listColumnWidths.AsList()) {
            if (!item.IsInt() || item.AsInt() <= 0) {
                validColumnWidths = false;
                break;
            }
            widths.push_back(static_cast<int>(item.AsInt()));
        }
        if (validColumnWidths) {
            listColumnWidths_ = std::move(widths);
        }
    }
    if (!validColumnWidths) {
        listColumnWidths_ = kDefaultListColumnWidths;
        configMigrated_ = true;
    }

    if (configMigrated_) {
        SyncToParser();
    }
}

static AdvConfig::Value RuleToConfigValue(const UserConfig::ArchiveFormatRule& rule)
{
    Value::Dict dict;
    dict[L"enabled"] = Value(rule.enabled);
    dict[L"extension"] = Value(rule.extension);
    dict[L"parser"] = Value(rule.parser);
    return Value(std::move(dict));
}

void UserConfig::SyncToParser()
{
    Value::Dict formatGroups;
    Value::List defaultRules;
    Value::List knownRules;
    Value::List customRules;

    for (const auto& rule : archiveFormatRules_) {
        if (rule.group == L"default") {
            defaultRules.push_back(RuleToConfigValue(rule));
        } else if (rule.group == L"known_aliases") {
            knownRules.push_back(RuleToConfigValue(rule));
        } else if (rule.group == L"custom") {
            customRules.push_back(RuleToConfigValue(rule));
        }
    }

    formatGroups[L"default"] = Value(std::move(defaultRules));
    formatGroups[L"known_aliases"] = Value(std::move(knownRules));
    formatGroups[L"custom"] = Value(std::move(customRules));
    parser_.Set(kKeyArchiveFormats, Value(std::move(formatGroups)));

    Value::List driveList;
    for (wchar_t drive : scanDriveLetters_) {
        driveList.push_back(Value(std::wstring(1, drive)));
    }
    parser_.Set(kKeyScanDrives, Value(std::move(driveList)));

    parser_.Set(kKeyParseThreads, Value(static_cast<int64_t>(parseThreadCount_)));

    parser_.Set(kKeyShowArchiveFullPath, Value(showArchiveFullPath_));

    parser_.Set(kKeyRememberUiState, Value(rememberUiState_));
    parser_.Set(kKeyStartupScanConfirmed, Value(startupScanConfirmed_));
    parser_.Set(kKeyAutoUpdateCheckEnabled, Value(autoUpdateCheckEnabled_));
    parser_.Set(kKeyLastAutoUpdateCheckAt, Value(lastAutoUpdateCheckAt_));
    parser_.Set(kKeyLanguage, Value(LanguageModeToConfigValue(languageMode_)));
    parser_.Remove(L"window_left");
    parser_.Remove(L"window_top");
    parser_.Remove(L"window_right");
    parser_.Remove(L"window_bottom");

    Value::Dict windowRect;
    windowRect[L"left"] = Value(static_cast<int64_t>(windowPlacement_.left));
    windowRect[L"top"] = Value(static_cast<int64_t>(windowPlacement_.top));
    windowRect[L"right"] = Value(static_cast<int64_t>(windowPlacement_.right));
    windowRect[L"bottom"] = Value(static_cast<int64_t>(windowPlacement_.bottom));
    parser_.Set(kKeyWindowRect, Value(std::move(windowRect)));

    parser_.Set(kKeyWindowMaximized, Value(windowPlacement_.maximized));

    Value::List columnWidths;
    for (int width : listColumnWidths_) {
        columnWidths.push_back(Value(static_cast<int64_t>(width)));
    }
    parser_.Set(kKeyListColumnWidths, Value(std::move(columnWidths)));
}

bool UserConfig::Load(const std::wstring& configPath, std::wstring* err)
{
    if (err) err->clear();
    configPath_ = configPath;

    std::wstring parseErr;
    if (!parser_.LoadFile(configPath, &parseErr)) {
        // 文件不存在时创建默认配置
        LOG_INFO(L"Config file not found, creating default: %s", configPath.c_str());
        archiveFormatRules_ = kBuiltInArchiveFormatRules;
        scanDriveLetters_ = kDefaultScanDriveLetters;
        parseThreadCount_ = 0;
        showArchiveFullPath_ = false;
        rememberUiState_ = true;
        startupScanConfirmed_ = false;
        autoUpdateCheckEnabled_ = true;
        lastAutoUpdateCheckAt_ = 0;
        languageMode_ = LanguageMode::System;
        windowPlacement_ = WindowPlacementConfig{};
        listColumnWidths_ = kDefaultListColumnWidths;
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

    LOG_INFO(L"Config loaded: %zu archive format rules, %zu scan drives",
             archiveFormatRules_.size(), scanDriveLetters_.size());
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
