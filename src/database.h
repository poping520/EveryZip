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

    bool CreateArchivesTable(std::wstring* err);
    bool InsertOrUpdateArchive(const ArchiveFile_t& archiveFile);
    bool InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err);

    bool CreateEntriesTable(std::wstring* err);
    bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err);
    bool InsertOrUpdateEntry(const ArchiveEntry_t& entry);
    bool InsertEntriesBatch(const std::vector<ArchiveEntry_t>& entries, std::wstring* err);
    bool GetArchiveLastUsn(wchar_t driveLetter, USN* outUsn);
    bool DeleteArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber);
    bool GetArchiveCount();
    bool BeginTransaction();
    bool CommitTransaction();
    bool RollbackTransaction();


    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err);
    bool QueryEntries(const std::wstring& filter, std::vector<ArchiveEntry_t>* out, std::wstring* err);

private:
    bool ExecSql16(const std::wstring& sql, std::wstring* err);

    sqlite3* db_ = nullptr;
};
