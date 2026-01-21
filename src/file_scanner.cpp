#include "file_scanner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>

#include <windows.h>

static bool IsNtfsDriveRoot(const std::wstring& driveRoot) {
    wchar_t fsName[MAX_PATH] = {0};
    if (!GetVolumeInformationW(driveRoot.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
        return false;
    }
    return _wcsicmp(fsName, L"NTFS") == 0;
}

static bool HasTargetExt(const wchar_t* name, size_t len) {
    auto endsWithI = [&](const wchar_t* ext) -> bool {
        const size_t extLen = wcslen(ext);
        if (len < extLen) return false;
        return _wcsicmp(std::wstring(name + (len - extLen), extLen).c_str(), ext) == 0;
    };
    return endsWithI(L".zip") || endsWithI(L".apk");
}

static uint64_t FileTimeToU64(const LARGE_INTEGER& li) {
    return static_cast<uint64_t>(li.QuadPart);
}

struct DirNode {
    DWORDLONG parent = 0;
    std::wstring name;
};

struct MatchedFile {
    DWORDLONG frn = 0;
    DWORDLONG pfrn = 0;
    USN usn = 0;
    std::wstring name;
    uint64_t modifyTs = 0;
};

static std::wstring BuildDirPath(const std::wstring& driveRoot,
                                DWORDLONG pfrn,
                                const std::unordered_map<DWORDLONG, DirNode>& dirs) {
    std::vector<std::wstring> parts;
    parts.reserve(64);

    DWORDLONG cur = pfrn;
    for (int i = 0; i < 1024; ++i) {
        auto it = dirs.find(cur);
        if (it == dirs.end()) break;
        const DirNode& n = it->second;
        if (!n.name.empty()) {
            parts.push_back(n.name);
        }
        if (n.parent == 0 || n.parent == cur) break;
        cur = n.parent;
    }

    std::wstring path = driveRoot;
    for (auto rit = parts.rbegin(); rit != parts.rend(); ++rit) {
        path.append(*rit);
        path.push_back(L'\\');
    }
    return path;
}

static bool TryGetFileSizeById(HANDLE hVol, DWORDLONG frn, uint64_t* outSize) {
    *outSize = 0;

    FILE_ID_DESCRIPTOR fid = {};
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    fid.FileId.QuadPart = static_cast<LONGLONG>(frn);

    HANDLE hFile = OpenFileById(
        hVol,
        &fid,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        0);

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER li = {};
    const bool ok = !!GetFileSizeEx(hFile, &li);
    if (ok && li.QuadPart >= 0) {
        *outSize = static_cast<uint64_t>(li.QuadPart);
    }
    CloseHandle(hFile);
    return ok;
}

static bool ScanOneNtfsVolumeByUsn(const std::wstring& driveRoot, std::vector<ArchiveFile_t>* out, std::wstring* err) {
    if (driveRoot.size() < 2) return true;
    const wchar_t driveLetter = driveRoot[0];

    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;

    HANDLE hVol = CreateFileW(
        volumePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        if (err) *err = L"CreateFileW(volume) failed";
        return false;
    }

    USN_JOURNAL_DATA_V0 journal = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &bytes, nullptr)) {
        CloseHandle(hVol);
        if (err) *err = L"FSCTL_QUERY_USN_JOURNAL failed";
        return false;
    }

    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = journal.NextUsn;

    std::vector<unsigned char> buffer;
    buffer.resize(1u << 20);

    std::unordered_map<DWORDLONG, DirNode> dirs;
    dirs.reserve(1u << 20);

    std::vector<MatchedFile> matches;
    matches.reserve(1u << 16);

    for (;;) {
        bytes = 0;
        if (!DeviceIoControl(
                hVol,
                FSCTL_ENUM_USN_DATA,
                &med,
                sizeof(med),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes,
                nullptr)) {
            const DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) {
                break;
            }
            CloseHandle(hVol);
            if (err) *err = L"FSCTL_ENUM_USN_DATA failed";
            return false;
        }

        if (bytes < sizeof(USN)) {
            break;
        }

        const USN* pUsn = reinterpret_cast<const USN*>(buffer.data());
        DWORD offset = sizeof(USN);

        while (offset + sizeof(USN_RECORD) <= bytes) {
            const USN_RECORD* rec = reinterpret_cast<const USN_RECORD*>(buffer.data() + offset);
            if (rec->RecordLength == 0 || offset + rec->RecordLength > bytes) {
                break;
            }

            const wchar_t* fileName = reinterpret_cast<const wchar_t*>(
                reinterpret_cast<const unsigned char*>(rec) + rec->FileNameOffset);
            const size_t fileNameLen = static_cast<size_t>(rec->FileNameLength / sizeof(wchar_t));
            const bool isDir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (isDir) {
                DirNode node;
                node.parent = static_cast<DWORDLONG>(rec->ParentFileReferenceNumber);
                node.name.assign(fileName, fileName + fileNameLen);
                dirs[static_cast<DWORDLONG>(rec->FileReferenceNumber)] = std::move(node);
            } else {
                if (HasTargetExt(fileName, fileNameLen)) {
                    MatchedFile mf;
                    mf.frn = static_cast<DWORDLONG>(rec->FileReferenceNumber);
                    mf.pfrn = static_cast<DWORDLONG>(rec->ParentFileReferenceNumber);
                    mf.usn = rec->Usn;
                    mf.name.assign(fileName, fileName + fileNameLen);
                    mf.modifyTs = FileTimeToU64(rec->TimeStamp);
                    matches.push_back(std::move(mf));
                }
            }

            offset += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *pUsn;
    }

    for (const auto& m : matches) {
        ArchiveFile_t af;
        af.name = m.name;
        af.frn = m.frn;
        af.pfrn = m.pfrn;
        af.usn = m.usn;
        af.modifyTimestamp = m.modifyTs;

        const std::wstring dirPath = BuildDirPath(driveRoot, m.pfrn, dirs);
        af.path = dirPath + m.name;

        uint64_t sz = 0;
        if (TryGetFileSizeById(hVol, m.frn, &sz)) {
            af.size = sz;
        } else {
            af.size = 0;
        }

        out->push_back(std::move(af));
    }

    CloseHandle(hVol);
    return true;
}

bool FileScanner::Scan(std::vector<ArchiveFile_t>* out, std::wstring* err) {
    if (!out) {
        if (err) *err = L"out is null";
        return false;
    }
    out->clear();
    if (err) err->clear();

    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) {
        if (err) *err = L"GetLogicalDriveStringsW failed";
        return false;
    }

    std::wstring drives;
    drives.resize(needed);
    if (GetLogicalDriveStringsW(needed, drives.data()) == 0) {
        if (err) *err = L"GetLogicalDriveStringsW failed";
        return false;
    }

    const wchar_t* p = drives.c_str();
    while (*p) {
        std::wstring root = p;
        p += root.size() + 1;

        const UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN) {
            continue;
        }
        if (!IsNtfsDriveRoot(root)) {
            continue;
        }

        std::wstring perr;
        if (!ScanOneNtfsVolumeByUsn(root, out, &perr)) {
            if (err && err->empty()) {
                *err = perr;
            }
            continue;
        }
    }

    return true;
}