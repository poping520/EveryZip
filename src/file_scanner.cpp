#include "file_scanner.h"

#include <atomic>
#include <windows.h>
#include <winioctl.h>

#include <vector>

/**
 * 判断给定盘符根路径是否为 NTFS 文件系统，仅 NTFS 才支持 USN Journal。
 * @param driveRoot 盘符根路径，例如 L"C:\\"。
 * @return NTFS 盘返回 true，否则返回 false。
 */
static bool IsNtfsDriveRoot(const std::wstring& driveRoot) {
    wchar_t fsName[MAX_PATH] = {0};
    if (!GetVolumeInformationW(driveRoot.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
        return false;
    }
    return _wcsicmp(fsName, L"NTFS") == 0;
}

/**
 * 判断文件名是否匹配目标归档扩展名集合。
 * @param name 文件名起始指针。
 * @param len 文件名长度。
 * @param extensions 允许的归档扩展名列表。
 * @return 命中任一扩展名返回 true，否则返回 false。
 */
static bool HasTargetExt(const wchar_t* name, size_t len, const std::vector<std::wstring>& extensions) {
    auto endsWithI = [&](const std::wstring& ext) -> bool {
        const size_t extLen = ext.size();
        if (len < extLen) return false;
        return _wcsicmp(std::wstring(name + (len - extLen), extLen).c_str(), ext.c_str()) == 0;
    };

    for (const auto& ext : extensions) {
        if (endsWithI(ext)) return true;
    }
    return false;
}

void FileScanner::SetArchiveExtensions(const std::vector<std::wstring>& exts) {
    archiveExtensions_ = exts;
}

static const std::vector<std::wstring> kDefaultExtensions = { L".zip", L".apk", L".7z" };

bool FileScanner::GetFileInfoByRefNumber(HANDLE hVol, uint64_t fileRefNumber, uint64_t* outFileSize, uint64_t* outModifyTime, std::wstring* outFullPath) {
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
            const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
            DWORD need = GetFinalPathNameByHandleW(hFile, nullptr, 0, flags);
            if (need != 0) {
                std::wstring fullPath;
                fullPath.resize(need);
                DWORD got = GetFinalPathNameByHandleW(hFile, fullPath.data(), need, flags);
                if (got != 0 && got < need) {
                    fullPath.resize(got);
                    if (fullPath.size() >= 4 && fullPath[0] == L'\\' && fullPath[1] == L'\\' && fullPath[2] == L'?' && fullPath[3] == L'\\') {
                        fullPath.erase(0, 4);
                    }
                    *outFullPath = std::move(fullPath);
                }
            }
        }
    }

    CloseHandle(hFile);
    return success;
}

/**
 * 基于 FSCTL_ENUM_USN_DATA 对整个盘符进行 USN 扫描，收集匹配扩展名的归档文件。
 * @param driveLetter 盘符。
 * @param out 输出扫描结果。
 * @param err 可选错误输出。
 * @param cancel 可选取消标志。
 * @param extensions 目标扩展名列表。
 * @return 扫描成功返回 true，否则返回 false。
 */
static bool ScanDriveByUsn(wchar_t driveLetter, std::vector<ArchiveFile_t>* out, std::wstring* err, std::atomic_bool* cancel, const std::vector<std::wstring>& extensions) {
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

            if (HasTargetExt(fileName, fileNameLen, extensions)) {
                ArchiveFile_t af;
                af.driveLetter = std::wstring(1, driveLetter);
                af.fileName.assign(fileName, fileNameLen);
                af.fileRefNumber = rec->FileReferenceNumber;
                af.parentFileRefNumber = rec->ParentFileReferenceNumber;
                af.usn = rec->Usn;

                uint64_t fileSize = 0;
                uint64_t modifyTime = 0;
                std::wstring fullPath;
                if (FileScanner::GetFileInfoByRefNumber(hVol, rec->FileReferenceNumber, &fileSize, &modifyTime, &fullPath)) {
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

bool FileScanner::QueryJournalInfo(wchar_t driveLetter, JournalInfo* out, std::wstring* err) {
    if (err) err->clear();
    if (!out) { if (err) *err = L"out is null"; return false; }

    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;

    HANDLE hVol = CreateFileW(volumePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        if (err) *err = L"CreateFileW volume failed";
        return false;
    }

    USN_JOURNAL_DATA_V0 journal{};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journal, sizeof(journal), &bytes, nullptr)) {
        if (err) *err = L"FSCTL_QUERY_USN_JOURNAL failed";
        CloseHandle(hVol);
        return false;
    }

    out->journalId = (int64_t)journal.UsnJournalID;
    out->nextUsn = journal.NextUsn;
    CloseHandle(hVol);
    return true;
}

bool FileScanner::ScanUsnJournal(wchar_t driveLetter, int64_t journalId, USN startUsn,
                                 std::vector<UsnChangeRecord_t>* out, USN* outNextUsn,
                                 std::wstring* err, std::atomic_bool* cancel,
                                 const std::vector<std::wstring>* extensions) {
    const std::vector<std::wstring>& exts = extensions ? *extensions : kDefaultExtensions;
    if (err) err->clear();
    if (!out) {
        if (err) *err = L"out is null";
        return false;
    }

    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;

    HANDLE hVol = CreateFileW(volumePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        if (err) *err = L"CreateFileW volume failed";
        return false;
    }

    // 查询当前 Journal 状态获取 NextUsn
    USN_JOURNAL_DATA_V0 journal{};
    DWORD bytes = 0;
    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journal, sizeof(journal), &bytes, nullptr)) {
        if (err) *err = L"FSCTL_QUERY_USN_JOURNAL failed";
        CloseHandle(hVol);
        return false;
    }

    if (outNextUsn) *outNextUsn = journal.NextUsn;

    // 如果 Journal ID 变了，说明 Journal 被重建，需要全量重扫
    if ((int64_t)journal.UsnJournalID != journalId) {
        if (err) *err = L"Journal ID changed, need full rescan";
        CloseHandle(hVol);
        return false;
    }

    // 没有新的变化
    if (startUsn >= journal.NextUsn) {
        CloseHandle(hVol);
        return true;
    }

    // 使用 FSCTL_READ_USN_JOURNAL 读取增量变化
    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = startUsn;
    readData.ReasonMask = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND |
                          USN_REASON_DATA_TRUNCATION | USN_REASON_NAMED_DATA_OVERWRITE |
                          USN_REASON_NAMED_DATA_EXTEND | USN_REASON_NAMED_DATA_TRUNCATION |
                          USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                          USN_REASON_RENAME_NEW_NAME | USN_REASON_RENAME_OLD_NAME |
                          USN_REASON_CLOSE;
    readData.ReturnOnlyOnClose = TRUE;  // 只返回 CLOSE 记录，减少噪音
    readData.UsnJournalID = (DWORDLONG)journalId;

    std::vector<unsigned char> buffer(1u << 16);  // 64KB 缓冲区

    for (;;) {
        if (cancel && cancel->load()) break;

        bytes = 0;
        BOOL ok = DeviceIoControl(hVol, FSCTL_READ_USN_JOURNAL,
            &readData, sizeof(readData),
            buffer.data(), (DWORD)buffer.size(),
            &bytes, nullptr);

        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF || e == ERROR_JOURNAL_ENTRY_DELETED) {
                break;
            }
            if (err) *err = L"FSCTL_READ_USN_JOURNAL failed (err=" + std::to_wstring(e) + L")";
            CloseHandle(hVol);
            return false;
        }

        if (bytes <= sizeof(USN)) break;

        // 第一个 USN 是下次读取的起始位置
        USN nextStartUsn = *(USN*)buffer.data();
        DWORD offset = sizeof(USN);

        while (offset + sizeof(USN_RECORD) <= bytes) {
            const USN_RECORD* rec = (const USN_RECORD*)(buffer.data() + offset);
            if (rec->RecordLength == 0 || offset + rec->RecordLength > bytes) break;

            const wchar_t* fileName = (const wchar_t*)((const unsigned char*)rec + rec->FileNameOffset);
            const size_t fileNameLen = rec->FileNameLength / sizeof(wchar_t);

            // 只关注归档文件（.zip/.apk/.7z 等）
            if (HasTargetExt(fileName, fileNameLen, exts)) {
                UsnChangeRecord_t cr;
                cr.driveLetter = driveLetter;
                cr.fileRefNumber = rec->FileReferenceNumber;
                cr.parentFileRefNumber = rec->ParentFileReferenceNumber;
                cr.reason = rec->Reason;
                cr.fileName.assign(fileName, fileNameLen);
                cr.usn = rec->Usn;
                out->push_back(std::move(cr));
            }

            offset += rec->RecordLength;
        }

        readData.StartUsn = nextStartUsn;
    }

    CloseHandle(hVol);
    return true;
}

/**
 * 扫描所有 NTFS 盘符，收集匹配扩展名的归档文件列表，作为初始全量索引输入。
 * @param out 输出归档文件列表。
 * @param err 可选错误输出。
 * @param cancel 可选取消标志。
 * @return 扫描成功返回 true，否则返回 false。
 */
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

        if (cancel && cancel->load()) return true;

        std::wstring scanErr;
        if (!ScanDriveByUsn(driveLetter, out, &scanErr, cancel, archiveExtensions_)) {
            if (err) {
                *err = scanErr.empty() ? L"ScanDriveByUsn failed" : scanErr;
            }
            return false;
        }
    }

    return true;
}
