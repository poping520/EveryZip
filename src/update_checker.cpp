#include "update_checker.h"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app_version.h"
#include "string_utils.h"

namespace EveryZip {
namespace {

constexpr wchar_t kLatestReleaseUrl[] =
    L"https://api.github.com/repos/poping520/EveryZip/releases/latest";

std::wstring LastErrorMessage(const wchar_t* action) {
    DWORD err = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out = action ? action : L"Operation failed";
    out += L" (";
    out += std::to_wstring(err);
    out += L")";
    if (msg) {
        out += L": ";
        out += msg;
        LocalFree(msg);
    }
    return out;
}

std::wstring GetTempUpdateDir() {
    wchar_t tempPath[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = (len > 0 && len < MAX_PATH) ? std::wstring(tempPath, len) : L".\\";
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    dir += L"EveryZipUpdate";
    return dir;
}

bool EnsureDirectoryExists(const std::wstring& path, std::wstring* errorMessage) {
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    if (errorMessage) *errorMessage = LastErrorMessage(L"Create update directory failed");
    return false;
}

bool CrackUrl(const std::wstring& url, URL_COMPONENTSW* parts,
    std::wstring* host, std::wstring* path, std::wstring* errorMessage) {
    wchar_t hostBuf[512]{};
    wchar_t pathBuf[4096]{};
    ZeroMemory(parts, sizeof(*parts));
    parts->dwStructSize = sizeof(*parts);
    parts->lpszHostName = hostBuf;
    parts->dwHostNameLength = (DWORD)_countof(hostBuf);
    parts->lpszUrlPath = pathBuf;
    parts->dwUrlPathLength = (DWORD)_countof(pathBuf);
    parts->dwSchemeLength = (DWORD)-1;
    parts->dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, parts)) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Parse update URL failed");
        return false;
    }
    *host = std::wstring(parts->lpszHostName, parts->dwHostNameLength);
    *path = std::wstring(parts->lpszUrlPath, parts->dwUrlPathLength);
    if (parts->lpszExtraInfo && parts->dwExtraInfoLength > 0) {
        path->append(parts->lpszExtraInfo, parts->dwExtraInfoLength);
    }
    return true;
}

bool ReadUrlToString(const std::wstring& url, std::string* response, std::wstring* errorMessage) {
    if (!response) return false;
    response->clear();

    URL_COMPONENTSW parts{};
    std::wstring host;
    std::wstring path;
    if (!CrackUrl(url, &parts, &host, &path, errorMessage)) return false;

    HINTERNET hSession = WinHttpOpen(L"EveryZip/" EVERYZIP_VERSION_WSTRING,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Open HTTP session failed");
        return false;
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), parts.nPort, 0);
    if (!hConnect) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Connect to update server failed");
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Open update request failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    bool ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr);

    if (!ok) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Receive update response failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        if (errorMessage) {
            *errorMessage = L"Update server returned HTTP " + std::to_wstring(status);
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::vector<char> buffer(8192);
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
        response->append(buffer.data(), buffer.data() + bytesRead);
    }
    if (GetLastError() != ERROR_SUCCESS && bytesRead == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_SUCCESS && errorMessage) {
            *errorMessage = LastErrorMessage(L"Read update response failed");
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

bool DownloadUrlToFile(const std::wstring& url, const std::wstring& tempPath,
    std::wstring* errorMessage) {
    URL_COMPONENTSW parts{};
    std::wstring host;
    std::wstring path;
    if (!CrackUrl(url, &parts, &host, &path, errorMessage)) return false;

    HINTERNET hSession = WinHttpOpen(L"EveryZip/" EVERYZIP_VERSION_WSTRING,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Open download session failed");
        return false;
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), parts.nPort, 0);
    if (!hConnect) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Connect to download server failed");
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Open download request failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Receive download response failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        if (errorMessage) *errorMessage = L"Download server returned HTTP " + std::to_wstring(status);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (errorMessage) *errorMessage = LastErrorMessage(L"Create downloaded update file failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    DWORD bytesRead = 0;
    DWORD totalBytes = 0;
    while (WinHttpReadData(hRequest, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
        DWORD written = 0;
        if (!WriteFile(hFile, buffer.data(), bytesRead, &written, nullptr) || written != bytesRead) {
            if (errorMessage) *errorMessage = LastErrorMessage(L"Write downloaded update file failed");
            CloseHandle(hFile);
            DeleteFileW(tempPath.c_str());
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }
        totalBytes += bytesRead;
    }

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (totalBytes == 0) {
        DeleteFileW(tempPath.c_str());
        if (errorMessage) *errorMessage = L"Downloaded update file is empty";
        return false;
    }
    return true;
}

std::string JsonUnescapeToUtf8(const std::string& escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        char ch = escaped[i];
        if (ch != '\\' || i + 1 >= escaped.size()) {
            out.push_back(ch);
            continue;
        }
        char esc = escaped[++i];
        switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
            out.push_back('?');
            i += std::min<size_t>(4, escaped.size() - i - 1);
            break;
        default:
            out.push_back(esc);
            break;
        }
    }
    return out;
}

bool ExtractJsonString(const std::string& json, size_t start, std::string* value, size_t* endPos) {
    size_t quote = json.find('"', start);
    if (quote == std::string::npos) return false;
    std::string raw;
    bool escaped = false;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (escaped) {
            raw.push_back('\\');
            raw.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            if (value) *value = JsonUnescapeToUtf8(raw);
            if (endPos) *endPos = i + 1;
            return true;
        }
        raw.push_back(ch);
    }
    return false;
}

bool ExtractStringField(const std::string& json, const std::string& key, std::wstring* value) {
    const std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace((unsigned char)json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    std::string utf8;
    if (!ExtractJsonString(json, pos, &utf8, nullptr)) return false;
    if (value) *value = Utf8ToWString(utf8.c_str());
    return true;
}

bool ExtractAssetsArray(const std::string& json, std::string* assets) {
    size_t pos = json.find("\"assets\"");
    if (pos == std::string::npos) return false;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return false;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char ch = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '[') ++depth;
        if (ch == ']') {
            --depth;
            if (depth == 0) {
                if (assets) *assets = json.substr(pos + 1, i - pos - 1);
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> SplitTopLevelObjects(const std::string& text) {
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(text.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

bool EndsWithIcase(const std::wstring& text, const std::wstring& suffix) {
    if (suffix.size() > text.size()) return false;
    auto it = text.end() - suffix.size();
    return std::equal(it, text.end(), suffix.begin(), suffix.end(),
        [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
}

bool SameIcase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](wchar_t left, wchar_t right) { return towlower(left) == towlower(right); });
}

bool SelectReleaseAsset(const std::string& json, std::wstring* assetName, std::wstring* downloadUrl) {
    std::string assetsText;
    if (!ExtractAssetsArray(json, &assetsText)) return false;

    struct Candidate {
        std::wstring name;
        std::wstring url;
    };
    std::vector<Candidate> exeAssets;
    for (const std::string& object : SplitTopLevelObjects(assetsText)) {
        std::wstring name;
        std::wstring url;
        if (!ExtractStringField(object, "name", &name) ||
            !ExtractStringField(object, "browser_download_url", &url)) {
            continue;
        }
        if (SameIcase(name, L"EveryZip.exe")) {
            if (assetName) *assetName = name;
            if (downloadUrl) *downloadUrl = url;
            return true;
        }
        if (EndsWithIcase(name, L".exe")) {
            exeAssets.push_back({ name, url });
        }
    }

    if (!exeAssets.empty()) {
        if (assetName) *assetName = exeAssets.front().name;
        if (downloadUrl) *downloadUrl = exeAssets.front().url;
        return true;
    }
    return false;
}

std::wstring EscapePowerShellSingleQuoted(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        if (ch == L'\'') out += L"''";
        else out.push_back(ch);
    }
    return out;
}

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text, std::wstring* errorMessage) {
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file) {
        if (errorMessage) *errorMessage = L"Create self-update script failed";
        return false;
    }
    const std::string utf8 = WideToUtf8(text);
    file.write(utf8.data(), (std::streamsize)utf8.size());
    if (!file.good()) {
        if (errorMessage) *errorMessage = L"Write self-update script failed";
        return false;
    }
    return true;
}

std::wstring GetCurrentExePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    while (len == path.size()) {
        path.resize(path.size() * 2);
        len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    }
    path.resize(len);
    return path;
}

} // namespace

AppVersion CurrentAppVersion() {
    return { EVERYZIP_VERSION_MAJOR, EVERYZIP_VERSION_MINOR, EVERYZIP_VERSION_PATCH, true };
}

std::wstring AppVersionToString(const AppVersion& version) {
    return std::to_wstring(version.major) + L"." +
        std::to_wstring(version.minor) + L"." +
        std::to_wstring(version.patch);
}

bool ParseAppVersion(const std::wstring& text, AppVersion* version) {
    std::wstring s = text;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) {
        s.erase(s.begin());
    }
    int parts[3]{};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t end = (i == 2) ? s.size() : s.find(L'.', start);
        if (end == std::wstring::npos || end == start) return false;
        for (size_t j = start; j < end; ++j) {
            if (!iswdigit(s[j])) return false;
        }
        parts[i] = _wtoi(s.substr(start, end - start).c_str());
        start = end + 1;
    }
    if (start <= s.size()) {
        // Exact three-part versions only.
    }
    if (version) {
        version->major = parts[0];
        version->minor = parts[1];
        version->patch = parts[2];
        version->valid = true;
    }
    return true;
}

int CompareAppVersions(const AppVersion& left, const AppVersion& right) {
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    return 0;
}

UpdateCheckResult CheckForUpdates() {
    UpdateCheckResult result{};
    std::string json;
    std::wstring err;
    if (!ReadUrlToString(kLatestReleaseUrl, &json, &err)) {
        result.status = UpdateCheckStatus::NetworkError;
        result.errorMessage = err;
        return result;
    }

    std::wstring tagName;
    if (!ExtractStringField(json, "tag_name", &tagName) || tagName.empty()) {
        result.status = UpdateCheckStatus::ParseError;
        result.errorMessage = L"GitHub release response does not contain tag_name";
        return result;
    }

    AppVersion latest{};
    if (!ParseAppVersion(tagName, &latest)) {
        result.status = UpdateCheckStatus::ParseError;
        result.errorMessage = L"Release tag is not a valid major.minor.patch version: " + tagName;
        return result;
    }

    result.release.version = latest;
    result.release.tagName = tagName;
    ExtractStringField(json, "name", &result.release.name);
    ExtractStringField(json, "body", &result.release.body);
    ExtractStringField(json, "html_url", &result.release.htmlUrl);

    if (CompareAppVersions(latest, CurrentAppVersion()) <= 0) {
        result.status = UpdateCheckStatus::UpToDate;
        return result;
    }

    if (!SelectReleaseAsset(json, &result.release.assetName, &result.release.downloadUrl)) {
        result.status = UpdateCheckStatus::NoAsset;
        result.errorMessage = L"Found a newer release, but no downloadable .exe asset was found.";
        return result;
    }

    result.status = UpdateCheckStatus::UpdateAvailable;
    return result;
}

bool DownloadUpdateExe(const std::wstring& url, const AppVersion& version,
    std::wstring* downloadedPath, std::wstring* errorMessage) {
    const std::wstring dir = GetTempUpdateDir();
    if (!EnsureDirectoryExists(dir, errorMessage)) return false;

    const std::wstring finalPath = dir + L"\\EveryZip-" + AppVersionToString(version) + L".exe";
    const std::wstring tempPath = finalPath + L".tmp";
    DeleteFileW(tempPath.c_str());
    DeleteFileW(finalPath.c_str());

    if (!DownloadUrlToFile(url, tempPath, errorMessage)) return false;
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tempPath.c_str());
        if (errorMessage) *errorMessage = LastErrorMessage(L"Finalize downloaded update failed");
        return false;
    }
    if (downloadedPath) *downloadedPath = finalPath;
    return true;
}

bool LaunchSelfUpdater(const std::wstring& downloadedExePath, std::wstring* errorMessage) {
    const std::wstring dir = GetTempUpdateDir();
    if (!EnsureDirectoryExists(dir, errorMessage)) return false;

    const std::wstring currentExe = GetCurrentExePath();
    if (currentExe.empty()) {
        if (errorMessage) *errorMessage = L"Cannot locate current executable";
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    const std::wstring scriptPath = dir + L"\\EveryZipSelfUpdate.ps1";
    const std::wstring backupPath = currentExe + L".bak";

    std::wstringstream ps;
    ps << L"$ErrorActionPreference = 'Stop'\r\n";
    ps << L"$pidToWait = " << pid << L"\r\n";
    ps << L"$current = '" << EscapePowerShellSingleQuoted(currentExe) << L"'\r\n";
    ps << L"$downloaded = '" << EscapePowerShellSingleQuoted(downloadedExePath) << L"'\r\n";
    ps << L"$backup = '" << EscapePowerShellSingleQuoted(backupPath) << L"'\r\n";
    ps << L"$script = $MyInvocation.MyCommand.Path\r\n";
    ps << L"try {\r\n";
    ps << L"  Wait-Process -Id $pidToWait -Timeout 60 -ErrorAction SilentlyContinue\r\n";
    ps << L"  if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }\r\n";
    ps << L"  if (Test-Path -LiteralPath $current) { Move-Item -LiteralPath $current -Destination $backup -Force }\r\n";
    ps << L"  Move-Item -LiteralPath $downloaded -Destination $current -Force\r\n";
    ps << L"  Start-Process -FilePath $current\r\n";
    ps << L"  if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }\r\n";
    ps << L"} catch {\r\n";
    ps << L"  if ((Test-Path -LiteralPath $backup) -and !(Test-Path -LiteralPath $current)) { Move-Item -LiteralPath $backup -Destination $current -Force }\r\n";
    ps << L"  if (Test-Path -LiteralPath $current) { Start-Process -FilePath $current }\r\n";
    ps << L"} finally {\r\n";
    ps << L"  Start-Sleep -Seconds 1\r\n";
    ps << L"  Remove-Item -LiteralPath $script -Force -ErrorAction SilentlyContinue\r\n";
    ps << L"}\r\n";

    if (!WriteTextFileUtf8(scriptPath, ps.str(), errorMessage)) return false;

    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"";
    params += scriptPath;
    params += L"\"";
    HINSTANCE res = ShellExecuteW(nullptr, L"open", L"powershell.exe", params.c_str(), nullptr, SW_HIDE);
    if ((INT_PTR)res <= 32) {
        if (errorMessage) *errorMessage = L"Launch self-update script failed";
        return false;
    }
    return true;
}

} // namespace EveryZip
