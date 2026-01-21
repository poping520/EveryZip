
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

static bool EnablePrivilege(const wchar_t* privName) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, privName, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);

    return ok && err == ERROR_SUCCESS;
}

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
    //return true;
}

static bool ScanDriveByUsn(const wchar_t driveLetter, uint64_t* outCount, uint64_t* outSizeBytes) {
    *outCount = 0;
    *outSizeBytes = 0;

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
        return false;
    }

    USN_JOURNAL_DATA_V0 journal = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &bytes, nullptr)) {
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
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                break;
            }
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
                (*outCount)++;

                FILE_ID_DESCRIPTOR fid = {};
                fid.dwSize = sizeof(fid);
                fid.Type = FileIdType;
                fid.FileId.QuadPart = static_cast<LONGLONG>(rec->FileReferenceNumber);

                HANDLE hFile = OpenFileById(
                    hVol,
                    &fid,
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    0);

                if (hFile != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER li = {};
                    if (GetFileSizeEx(hFile, &li) && li.QuadPart > 0) {
                        *outSizeBytes += static_cast<uint64_t>(li.QuadPart);
                    }
                    CloseHandle(hFile);
                }
            }

            offset += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *pUsn;
    }

    CloseHandle(hVol);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    (void)argc;
    (void)argv;

    EnablePrivilege(L"SeBackupPrivilege");

    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) {
        std::wcerr << L"GetLogicalDriveStringsW failed: " << GetLastError() << L"\n";
        return 1;
    }

    std::wstring drives;
    drives.resize(needed);
    if (GetLogicalDriveStringsW(needed, drives.data()) == 0) {
        std::wcerr << L"GetLogicalDriveStringsW failed: " << GetLastError() << L"\n";
        return 1;
    }

    uint64_t totalCount = 0;
    uint64_t totalSizeBytes = 0;

    const auto t0 = std::chrono::steady_clock::now();

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

        uint64_t count = 0;
        uint64_t sizeBytes = 0;
        if (!ScanDriveByUsn(driveLetter, &count, &sizeBytes)) {
            std::wcerr << L"Scan failed on " << driveLetter << L":\\ (err=" << GetLastError() << L")\n";
            continue;
        }

        totalCount += count;
        totalSizeBytes += sizeBytes;
        std::wcout << driveLetter << L":\\ => COUNT=" << count << L" SIZE_BYTES=" << sizeBytes << L"\n";
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::wcout << L"TOTAL => COUNT=" << totalCount << L" SIZE_BYTES=" << totalSizeBytes << L"\n";
    std::wcout << L"TIME_MS => " << ms << L"\n";

    return 0;
}
