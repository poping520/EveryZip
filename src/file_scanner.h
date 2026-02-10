#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "database.h"

// USN Journal 元数据
struct JournalInfo {
    int64_t journalId = 0;
    USN nextUsn = 0;
};

class FileScanner {
public:
    FileScanner() = default;
    ~FileScanner() = default;

    // 全量 MFT 扫描
    bool Scan(std::vector<ArchiveFile_t>* out, std::wstring* err, std::atomic_bool* cancel = nullptr);

    // 通过 FileReferenceNumber 获取文件信息（大小、修改时间、完整路径）
    static bool GetFileInfoByRefNumber(HANDLE hVol, uint64_t fileRefNumber,
                                       uint64_t* outFileSize, uint64_t* outModifyTime,
                                       std::wstring* outFullPath);

    // 获取指定盘符的 USN Journal 元数据（journalId, nextUsn）
    static bool QueryJournalInfo(wchar_t driveLetter, JournalInfo* out, std::wstring* err);

    // 增量读取 USN Journal：从 startUsn 开始读取归档文件的变化记录
    static bool ScanUsnJournal(wchar_t driveLetter, int64_t journalId, USN startUsn,
                               std::vector<UsnChangeRecord_t>* out, USN* outNextUsn,
                               std::wstring* err, std::atomic_bool* cancel = nullptr);
};
