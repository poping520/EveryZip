#pragma once

#include <string>

namespace EveryZip {

struct AppVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

struct ReleaseInfo {
    AppVersion version;
    std::wstring tagName;
    std::wstring name;
    std::wstring body;
    std::wstring htmlUrl;
    std::wstring assetName;
    std::wstring downloadUrl;
};

enum class UpdateCheckStatus {
    UpToDate,
    UpdateAvailable,
    NetworkError,
    ParseError,
    NoAsset
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::ParseError;
    ReleaseInfo release;
    std::wstring errorMessage;
};

AppVersion CurrentAppVersion();
std::wstring AppVersionToString(const AppVersion& version);
bool ParseAppVersion(const std::wstring& text, AppVersion* version);
int CompareAppVersions(const AppVersion& left, const AppVersion& right);

UpdateCheckResult CheckForUpdates();
bool DownloadUpdateExe(const std::wstring& url, const AppVersion& version,
    std::wstring* downloadedPath, std::wstring* errorMessage);
bool LaunchSelfUpdater(const std::wstring& downloadedExePath, std::wstring* errorMessage);

} // namespace EveryZip
