#include "file_scanner.h"

#include <atomic>
#include <windows.h>
#include <winioctl.h>

#include <vector>

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

    return endsWithI(L".zip");
}

static bool GetFileInfoByRefNumber(HANDLE hVol, uint64_t fileRefNumber, uint64_t* outFileSize, uint64_t* outModifyTime, std::wstring* outFullPath) {
    if (outFileSize) *outFileSize = 0;
    if (outModifyTime) *outModifyTime = 0;
    if (outFullPath) outFullPath->clear();

    FILE_ID_DESCRIPTOR fid = {};
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    fid.FileId.QuadPart = static_cast<LONGLONG>(fileRefNumber);

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

    BY_HANDLE_FILE_INFORMATION fileInfo = {};
    bool success = GetFileInformationByHandle(hFile, &fileInfo) != 0;
    if (success) {
        if (outFileSize) {
            *outFileSize = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;
        }
        if (outModifyTime) {
            ULARGE_INTEGER ui{};
            ui.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;
            ui.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
            *outModifyTime = ui.QuadPart;
        }

        if (outFullPath) {
            DWORD need = GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED);
            if (need != 0) {
                std::wstring fullPath;
                fullPath.resize(need);
                DWORD got = GetFinalPathNameByHandleW(hFile, fullPath.data(), need, FILE_NAME_NORMALIZED);
                if (got != 0) {
                    while (!fullPath.empty() && fullPath.back() == L'\0') {
                        fullPath.pop_back();
                    }
                    *outFullPath = std::move(fullPath);
                }
            }
        }
    }

    CloseHandle(hFile);
    return success;
}

static bool ScanDriveByUsn(wchar_t driveLetter, std::vector<ArchiveFile_t>* out, std::wstring* err, std::atomic_bool* cancel) {
    if (err) err->clear();
    if (!out) {
        if (err) *err = L"out is null";
        return false;
    }

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
        if (err) *err = L"CreateFileW volume failed";
        return false;
    }

    USN_JOURNAL_DATA_V0 journal = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &bytes, nullptr)) {
        if (err) *err = L"FSCTL_QUERY_USN_JOURNAL failed";
        CloseHandle(hVol);
        return false;
    }

    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = journal.NextUsn;

    std::vector<unsigned char> buffer;
    buffer.resize(1u << 20);

    for (;;) {
        if (cancel && cancel->load()) {
            CloseHandle(hVol);
            return true;
        }
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
            if (err) *err = L"FSCTL_ENUM_USN_DATA failed";
            CloseHandle(hVol);
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

            if (HasTargetExt(fileName, fileNameLen)) {
                ArchiveFile_t af;
                af.driveLetter = std::wstring(1, driveLetter);
                af.fileName.assign(fileName, fileNameLen);
                af.fileRefNumber = rec->FileReferenceNumber;
                af.parentFileRefNumber = rec->ParentFileReferenceNumber;
                af.usn = rec->Usn;

                uint64_t fileSize = 0;
                uint64_t modifyTime = 0;
                std::wstring fullPath;
                if (GetFileInfoByRefNumber(hVol, rec->FileReferenceNumber, &fileSize, &modifyTime, &fullPath)) {
                    af.fileSize = fileSize;
                    af.modifyTime = modifyTime;
                    af.filePath = std::move(fullPath);
                }

                out->push_back(std::move(af));
            }

            offset += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *pUsn;
    }

    CloseHandle(hVol);
    return true;
}

bool FileScanner::Scan(std::vector<ArchiveFile_t>* out, std::wstring* err, std::atomic_bool* cancel) {
    if (err) err->clear();
    if (!out) {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

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

        if (root.size() < 2) continue;

        const UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN) {
            continue;
        }

        if (!IsNtfsDriveRoot(root)) {
            continue;
        }

        const wchar_t driveLetter = root[0];

        // test
        if (driveLetter != L'E') {
            continue;
        }

        if (cancel && cancel->load()) return true;

        std::wstring scanErr;
        if (!ScanDriveByUsn(driveLetter, out, &scanErr, cancel)) {
            if (err) {
                *err = scanErr.empty() ? L"ScanDriveByUsn failed" : scanErr;
            }
            return false;
        }
    }

    return true;
}
