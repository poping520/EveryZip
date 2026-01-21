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
    bool InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err);

    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err);

private:
    bool ExecSql16(const std::wstring& sql, std::wstring* err);

    sqlite3* db_ = nullptr;
};
