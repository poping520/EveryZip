#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstdint>
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

#pragma pack(push, 1)
struct FileRecordHeader {
    uint32_t signature;
    uint16_t fixupOffset;
    uint16_t fixupCount;
    uint64_t lsn;
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;
    uint32_t bytesInUse;
    uint32_t bytesAllocated;
    uint64_t baseFileRecord;
    uint16_t nextAttributeId;
    uint16_t align;
    uint32_t recordNumber;
};

struct AttributeRecordHeader {
    uint32_t type;
    uint32_t length;
    uint8_t nonResident;
    uint8_t nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t instance;
    union {
        struct {
            uint32_t valueLength;
            uint16_t valueOffset;
            uint8_t residentFlags;
            uint8_t reserved;
        } resident;
        struct {
            uint64_t lowestVCN;
            uint64_t highestVCN;
            uint16_t mappingPairsOffset;
            uint8_t compressionUnit;
            uint8_t reserved1[5];
            uint64_t allocatedSize;
            uint64_t dataSize;
            uint64_t initializedSize;
            uint64_t compressedSize;
        } nonres;
    } data;
};

struct FileNameAttribute {
    uint64_t parentDirectory;
    uint64_t creationTime;
    uint64_t changeTime;
    uint64_t lastWriteTime;
    uint64_t lastAccessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t flags;
    uint32_t reparse;
    uint8_t nameLength;
    uint8_t nameType;
    wchar_t name[1];
};
#pragma pack(pop)

static bool IsValidFileRecordSignature(const void* rec) {
    const auto* hdr = reinterpret_cast<const FileRecordHeader*>(rec);
    return hdr->signature == 0x454C4946; // 'FILE'
}

static bool IsDirectoryRecord(const void* rec) {
    const auto* hdr = reinterpret_cast<const FileRecordHeader*>(rec);
    return (hdr->flags & 0x0002) != 0;
}

static bool ExtractBestFileNameAndSize(
    const void* fileRecord,
    uint32_t recordBytes,
    std::wstring* outName,
    uint64_t* outSize) {
    *outName = L"";
    *outSize = 0;

    if (recordBytes < sizeof(FileRecordHeader)) return false;
    const auto* fr = reinterpret_cast<const FileRecordHeader*>(fileRecord);
    if (fr->firstAttributeOffset >= recordBytes) return false;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(fileRecord);
    uint32_t off = fr->firstAttributeOffset;

    std::wstring bestName;
    uint64_t bestSize = 0;
    int bestRank = -1;

    while (off + sizeof(AttributeRecordHeader) <= recordBytes) {
        const auto* attr = reinterpret_cast<const AttributeRecordHeader*>(base + off);
        if (attr->type == 0xFFFFFFFF) break;
        if (attr->length == 0) break;
        if (off + attr->length > recordBytes) break;

        if (attr->type == 0x30 && attr->nonResident == 0) {
            const uint32_t vlen = attr->data.resident.valueLength;
            const uint16_t voff = attr->data.resident.valueOffset;
            if (voff + vlen <= attr->length && voff + sizeof(FileNameAttribute) <= attr->length) {
                const auto* fna = reinterpret_cast<const FileNameAttribute*>(
                    reinterpret_cast<const uint8_t*>(attr) + voff);
                const size_t nlen = static_cast<size_t>(fna->nameLength);
                const size_t bytesNeeded = sizeof(FileNameAttribute) - sizeof(wchar_t) + nlen * sizeof(wchar_t);
                if (bytesNeeded <= vlen) {
                    std::wstring name(fna->name, fna->name + nlen);
                    const uint64_t size = fna->realSize;
                    const int rank = (fna->nameType == 1 || fna->nameType == 3) ? 2 : 1;
                    if (rank > bestRank) {
                        bestRank = rank;
                        bestName = std::move(name);
                        bestSize = size;
                    }
                }
            }
        }

        off += attr->length;
    }

    if (bestRank < 0) return false;

    *outName = std::move(bestName);
    *outSize = bestSize;
    return true;
}

static bool ScanDriveByMft(const wchar_t driveLetter, uint64_t* outCount, uint64_t* outSizeBytes) {
    *outCount = 0;
    *outSizeBytes = 0;

    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;

    HANDLE hVol = CreateFileW(
        volumePath,
        FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hVol == INVALID_HANDLE_VALUE) {
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER vol = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vol, sizeof(vol), &bytes, nullptr)) {
        CloseHandle(hVol);
        return false;
    }

    const uint64_t recordSize = static_cast<uint64_t>(vol.BytesPerFileRecordSegment);
    const uint64_t mftValid = static_cast<uint64_t>(vol.MftValidDataLength.QuadPart);
    if (recordSize == 0 || mftValid < recordSize) {
        CloseHandle(hVol);
        return false;
    }

    const uint64_t maxRecords = mftValid / recordSize;

    std::vector<uint8_t> outBuf;
    outBuf.resize(static_cast<size_t>(recordSize + 4096));

    NTFS_FILE_RECORD_INPUT_BUFFER in = {};

    for (uint64_t frn = 0; frn < maxRecords; ++frn) {
        in.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);
        bytes = 0;

        if (!DeviceIoControl(
                hVol,
                FSCTL_GET_NTFS_FILE_RECORD,
                &in,
                sizeof(in),
                outBuf.data(),
                static_cast<DWORD>(outBuf.size()),
                &bytes,
                nullptr)) {
            continue;
        }

        if (bytes < sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER)) {
            continue;
        }

        const auto* out = reinterpret_cast<const NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outBuf.data());
        const void* rec = out->FileRecordBuffer;

        if (!IsValidFileRecordSignature(rec)) {
            continue;
        }
        if (IsDirectoryRecord(rec)) {
            continue;
        }

        std::wstring name;
        uint64_t size = 0;
        if (!ExtractBestFileNameAndSize(rec, static_cast<uint32_t>(recordSize), &name, &size)) {
            continue;
        }

        if (!name.empty() && HasTargetExt(name.c_str(), name.size())) {
            (*outCount)++;
            *outSizeBytes += size;
        }
    }

    CloseHandle(hVol);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    (void)argc;
    (void)argv;

    if (!EnablePrivilege(L"SeBackupPrivilege")) {
        std::wcerr << L"EnablePrivilege(SeBackupPrivilege) failed: " << GetLastError() << L"\n";
    }
    if (!EnablePrivilege(L"SeManageVolumePrivilege")) {
        std::wcerr << L"EnablePrivilege(SeManageVolumePrivilege) failed: " << GetLastError() << L"\n";
    }

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
        if (!ScanDriveByMft(driveLetter, &count, &sizeBytes)) {
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