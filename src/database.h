#pragma once

#include <string>
#include <vector>

#include "types.h"

struct sqlite3;

/**
 * table: configs
 * ┌──────┬───────┐
 * │ key  │ value │
 * └──────┴───────┘
 *
 * table: archives
 * ┌──────────┬────────┬────────┬────────┐
 * │ xxxx     │ xxxx   │ xxxx   │ xxxx   │
 * └──────────┴────────┴────────┴────────┘
 *
 * table: files
 *
 */
class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool Open(const std::wstring& dbPath, std::wstring* err);
    void Close();

    bool CreateConfigsTable(std::wstring* err);
    bool SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err);
    bool GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn);

    bool CreateArchivesTable(std::wstring* err);
    bool InsertOrUpdateArchive(const ArchiveFile_t& archiveFile);
    bool InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err);
    bool QueryArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out);

    bool CreateEntriesTable(std::wstring* err);
    bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err);
    bool InsertOrUpdateEntry(const ArchiveEntry_t& entry);
    bool InsertEntriesBatch(const std::vector<ArchiveEntry_t>& entries, std::wstring* err);
    bool GetArchiveLastUsn(wchar_t driveLetter, USN* outUsn);
    bool DeleteArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber);
    int64_t GetArchiveCount();
    bool BeginTransaction();
    bool CommitTransaction();
    bool RollbackTransaction();


    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err);
    bool QueryEntries(const std::wstring& filter, std::vector<ArchiveEntry_t>* out, std::wstring* err);

    // 纯虚拟列表支持：只查询 rowid 列表（内存极低），按需查询单行
    bool QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc,
                       std::vector<int64_t>* outIds, std::wstring* err);
    bool QueryEntryById(int64_t rowId, ArchiveEntry_t* out);
    int64_t GetEntryCount(const std::wstring& filter);

private:
    bool ExecSql16(const std::wstring& sql, std::wstring* err);

    sqlite3* db_ = nullptr;
};
