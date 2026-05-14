#include "rar_archive_parser.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "../string_utils.h"

#include "dll.hpp"

namespace EveryZip {

struct RarArchiveParser::State {
    HANDLE handle = nullptr;
    std::wstring archivePath;
};

static std::string RarErrorToString(int code) {
    switch (code) {
    case ERAR_SUCCESS: return "success";
    case ERAR_END_ARCHIVE: return "end of archive";
    case ERAR_NO_MEMORY: return "out of memory";
    case ERAR_BAD_DATA: return "bad data";
    case ERAR_BAD_ARCHIVE: return "bad archive";
    case ERAR_UNKNOWN_FORMAT: return "unknown format";
    case ERAR_EOPEN: return "open error";
    case ERAR_ECREATE: return "create error";
    case ERAR_ECLOSE: return "close error";
    case ERAR_EREAD: return "read error";
    case ERAR_EWRITE: return "write error";
    case ERAR_SMALL_BUF: return "buffer too small";
    case ERAR_UNKNOWN: return "unknown error";
    case ERAR_MISSING_PASSWORD: return "missing password";
    case ERAR_EREFERENCE: return "reference error";
    case ERAR_BAD_PASSWORD: return "bad password";
    case ERAR_LARGE_DICT: return "dictionary is too large";
    default: return "unrar error: " + std::to_string(code);
    }
}

static int CALLBACK RarCallback(UINT msg, LPARAM, LPARAM, LPARAM) {
    if (msg == UCM_NEEDPASSWORD || msg == UCM_NEEDPASSWORDW ||
        msg == UCM_CHANGEVOLUME || msg == UCM_CHANGEVOLUMEW) {
        return -1;
    }
    return 0;
}

static std::wstring GetEntryNameFromPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

static std::string NormalizePathUtf8(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

static std::int64_t CombineSize(unsigned int low, unsigned int high) {
    const std::uint64_t value = (static_cast<std::uint64_t>(high) << 32) |
                                static_cast<std::uint64_t>(low);
    if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return -1;
    }
    return static_cast<std::int64_t>(value);
}

static void FillModifiedTime(unsigned int dosTime, std::tm* out) {
    if (!out || dosTime == 0) return;

    const int month = static_cast<int>((dosTime >> 21) & 0x0fu);
    out->tm_sec = static_cast<int>((dosTime & 0x1fu) * 2u);
    out->tm_min = static_cast<int>((dosTime >> 5) & 0x3fu);
    out->tm_hour = static_cast<int>((dosTime >> 11) & 0x1fu);
    out->tm_mday = static_cast<int>((dosTime >> 16) & 0x1fu);
    out->tm_mon = month - 1;
    out->tm_year = static_cast<int>(((dosTime >> 25) & 0x7fu) + 80u);
}

static bool OpenRarHandle(const std::wstring& archivePath,
                          unsigned int mode,
                          HANDLE* outHandle,
                          unsigned int* outFlags,
                          std::string* error) {
    if (outHandle) *outHandle = nullptr;
    if (outFlags) *outFlags = 0;

    RAROpenArchiveDataEx openData{};
    openData.ArcNameW = const_cast<wchar_t*>(archivePath.c_str());
    openData.OpenMode = mode;
    openData.Callback = RarCallback;

    HANDLE handle = RAROpenArchiveEx(&openData);
    if (!handle) {
        if (error) *error = "RAROpenArchiveEx failed: " + RarErrorToString(openData.OpenResult);
        return false;
    }

    if ((openData.Flags & ROADF_ENCHEADERS) != 0) {
        RARCloseArchive(handle);
        if (error) *error = "encrypted RAR headers are not supported";
        return false;
    }

    if (outFlags) *outFlags = openData.Flags;
    if (outHandle) *outHandle = handle;
    return true;
}

RarArchiveParser::RarArchiveParser()
    : state_(std::make_unique<State>())
{
}

RarArchiveParser::~RarArchiveParser() {
    Close();
}

bool RarArchiveParser::Open(const std::wstring& archive_path, std::string* error) {
    Close();

    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }

    HANDLE handle = nullptr;
    if (!OpenRarHandle(archive_path, RAR_OM_LIST, &handle, nullptr, error)) {
        return false;
    }

    state_->handle = handle;
    state_->archivePath = archive_path;
    return true;
}

void RarArchiveParser::Close() {
    if (!state_) return;
    if (state_->handle) {
        RARCloseArchive(state_->handle);
        state_->handle = nullptr;
    }
    state_->archivePath.clear();
}

bool RarArchiveParser::IsOpen() const {
    return state_ && state_->handle != nullptr;
}

std::wstring RarArchiveParser::ArchivePath() const {
    return state_ ? state_->archivePath : std::wstring();
}

bool RarArchiveParser::ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) {
    if (!out_entries) {
        if (error) *error = "out_entries is null";
        return false;
    }
    out_entries->clear();

    if (!IsOpen()) {
        if (error) *error = "archive is not open";
        return false;
    }

    for (;;) {
        RARHeaderDataEx header{};
        const int readCode = RARReadHeaderEx(state_->handle, &header);
        if (readCode == ERAR_END_ARCHIVE) {
            return true;
        }
        if (readCode != ERAR_SUCCESS) {
            if (error) *error = "RARReadHeaderEx failed: " + RarErrorToString(readCode);
            return false;
        }

        if ((header.Flags & RHDF_ENCRYPTED) != 0) {
            if (error) *error = "password protected RAR entries are not supported";
            return false;
        }

        ArchiveEntry_t entry;
        entry.entryPath = header.FileNameW;
        if (entry.entryPath.empty() && header.FileName[0] != '\0') {
            entry.entryPath = Utf8ToWString(header.FileName);
        }
        entry.entryRawPath = WideToUtf8(entry.entryPath);
        entry.isDirectory = (header.Flags & RHDF_DIRECTORY) != 0;
        entry.compressed_size = entry.isDirectory ? 0 : CombineSize(header.PackSize, header.PackSizeHigh);
        const std::int64_t uncompressed = CombineSize(header.UnpSize, header.UnpSizeHigh);
        entry.original_size = uncompressed < 0 ? 0 : static_cast<std::uint64_t>(uncompressed);
        std::tm modifiedTime{};
        FillModifiedTime(header.FileTime, &modifiedTime);
        entry.modifiedTime = LocalTmToFileTimeValue(modifiedTime);

        out_entries->push_back(std::move(entry));

        const int skipCode = RARProcessFileW(state_->handle, RAR_SKIP, nullptr, nullptr);
        if (skipCode != ERAR_SUCCESS) {
            if (error) *error = "RARProcessFileW skip failed: " + RarErrorToString(skipCode);
            return false;
        }
    }
}

bool RarArchiveParser::ExtractEntry(const std::string& entry_path,
                                    const std::wstring& dest_dir,
                                    std::string* error) {
    if (state_->archivePath.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }
    if (entry_path.empty()) {
        if (error) *error = "entry_path is empty";
        return false;
    }

    HANDLE handle = nullptr;
    if (!OpenRarHandle(state_->archivePath, RAR_OM_EXTRACT, &handle, nullptr, error)) {
        return false;
    }

    const std::string target = NormalizePathUtf8(entry_path);
    bool found = false;
    for (;;) {
        RARHeaderDataEx header{};
        const int readCode = RARReadHeaderEx(handle, &header);
        if (readCode == ERAR_END_ARCHIVE) {
            break;
        }
        if (readCode != ERAR_SUCCESS) {
            RARCloseArchive(handle);
            if (error) *error = "RARReadHeaderEx failed: " + RarErrorToString(readCode);
            return false;
        }

        std::wstring nameW = header.FileNameW;
        if (nameW.empty() && header.FileName[0] != '\0') {
            nameW = Utf8ToWString(header.FileName);
        }
        const std::string current = NormalizePathUtf8(WideToUtf8(nameW));

        if (current == target) {
            if ((header.Flags & RHDF_ENCRYPTED) != 0) {
                RARCloseArchive(handle);
                if (error) *error = "password protected RAR entries are not supported";
                return false;
            }

            std::wstring entryName = GetEntryNameFromPath(nameW);
            if (entryName.empty()) {
                RARCloseArchive(handle);
                if (error) *error = "entry has no filename: " + entry_path;
                return false;
            }

            std::wstring destPath = dest_dir;
            if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
            destPath += entryName;

            int processCode = ERAR_SUCCESS;
            if ((header.Flags & RHDF_DIRECTORY) != 0) {
                CreateDirectoryW(destPath.c_str(), nullptr);
                processCode = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
            } else {
                processCode = RARProcessFileW(handle, RAR_EXTRACT, nullptr, destPath.data());
            }
            RARCloseArchive(handle);
            if (processCode != ERAR_SUCCESS) {
                if (error) *error = "RARProcessFileW extract failed: " + RarErrorToString(processCode);
                return false;
            }
            found = true;
            break;
        }

        const int skipCode = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
        if (skipCode != ERAR_SUCCESS) {
            RARCloseArchive(handle);
            if (error) *error = "RARProcessFileW skip failed: " + RarErrorToString(skipCode);
            return false;
        }
    }

    if (!found) {
        RARCloseArchive(handle);
        if (error) *error = "entry not found: " + entry_path;
        return false;
    }
    return true;
}

} // namespace EveryZip
