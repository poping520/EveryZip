#include "database.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <windows.h>

#include "sqlite3.h"
#include "logger.h"

static std::wstring Utf8ToWString(const char* s)
{
    if (!s) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring w;
    w.resize(static_cast<size_t>(needed - 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), needed);
    return w;
}

Database::Database() = default;

Database::~Database()
{
    Close();
}

bool Database::Open(const std::wstring& dbPath, std::wstring* err)
{
    if (err) err->clear();

    Close();

    sqlite3* db = nullptr;
    const int rc = sqlite3_open16(dbPath.c_str(), &db);
    if (rc != SQLITE_OK || !db)
    {
        if (err)
        {
            *err = L"sqlite3_open16 failed";
        }
        if (db)
        {
            sqlite3_close(db);
        }
        return false;
    }

    db_ = db;
    return true;
}

bool Database::InsertOrUpdateEntry(const ArchiveEntry_t& e)
{
    if (!db_)
    {
        return false;
    }

    const std::wstring sql =
        L"INSERT OR REPLACE INTO entries (archive_path, entry_name, entry_path, compressed_size, uncompressed_size) "
        L"VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text16(stmt, 1, e.archivePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(stmt, 2, e.entryName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(stmt, 3, e.entryPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)e.compressed_size);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)e.uncompressed_size);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Database::InsertEntriesBatch(const std::vector<ArchiveEntry_t>& entries, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    if (!BeginTransaction())
    {
        if (err) *err = L"BeginTransaction failed";
        return false;
    }

    for (const auto& e : entries)
    {
        if (!InsertOrUpdateEntry(e))
        {
            RollbackTransaction();
            if (err)
            {
                const char* em = sqlite3_errmsg(db_);
                *err = em ? Utf8ToWString(em) : L"InsertOrUpdateEntry failed";
            }
            return false;
        }
    }

    if (!CommitTransaction())
    {
        RollbackTransaction();
        if (err) *err = L"CommitTransaction failed";
        return false;
    }

    return true;
}

bool Database::QueryEntries(const std::wstring& filter, std::vector<ArchiveEntry_t>* out, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (!out)
    {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

    sqlite3_stmt* stmt = nullptr;
    const bool hasFilter = !filter.empty();

    const std::wstring sql = hasFilter
                                 ? L"SELECT archive_path, entry_name, entry_path, compressed_size, uncompressed_size FROM entries "
                                   L"WHERE archive_path LIKE '%' || ? || '%' OR entry_name LIKE '%' || ? || '%' OR entry_path LIKE '%' || ? || '%' "
                                   L"ORDER BY id DESC;"
                                 : L"SELECT archive_path, entry_name, entry_path, compressed_size, uncompressed_size FROM entries ORDER BY id DESC;";

    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter)
    {
        sqlite3_bind_text16(stmt, 1, filter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, filter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 3, filter.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ArchiveEntry_t e;

        const void* ap16 = sqlite3_column_text16(stmt, 0);
        const void* en16 = sqlite3_column_text16(stmt, 1);
        const void* ep16 = sqlite3_column_text16(stmt, 2);
        if (ap16) e.archivePath = reinterpret_cast<const wchar_t*>(ap16);
        if (en16) e.entryName = reinterpret_cast<const wchar_t*>(en16);
        if (ep16) e.entryPath = reinterpret_cast<const wchar_t*>(ep16);

        e.compressed_size = (std::uint64_t)sqlite3_column_int64(stmt, 3);
        e.uncompressed_size = (std::uint64_t)sqlite3_column_int64(stmt, 4);

        out->push_back(std::move(e));
    }

    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::CreateEntriesTable(std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = R"(
            CREATE TABLE IF NOT EXISTS entries (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                archive_path TEXT NOT NULL,
                entry_name TEXT NOT NULL,
                entry_path TEXT NOT NULL,
                compressed_size INTEGER NOT NULL,
                uncompressed_size INTEGER NOT NULL,
                UNIQUE(archive_path, entry_path)
            );
            CREATE INDEX IF NOT EXISTS idx_entries_archive_path ON entries(archive_path);
            CREATE INDEX IF NOT EXISTS idx_entries_entry_name ON entries(entry_name);
            CREATE INDEX IF NOT EXISTS idx_entries_entry_path ON entries(entry_path);
        )";

    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create entries table failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const std::wstring sql = L"DELETE FROM entries WHERE archive_path = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text16(stmt, 1, archivePath.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        return false;
    }
    return true;
}

bool Database::QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (!out)
    {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

    sqlite3_stmt* stmt = nullptr;
    const bool hasFilter = !filter.empty();

    const std::wstring sql = hasFilter
                                 ? L"SELECT drive_letter, file_name, file_path, file_size, modify_time, usn, file_ref_number, parent_file_ref_number FROM archives "
                                 L"WHERE file_name LIKE '%' || ? || '%' OR file_path LIKE '%' || ? || '%' ORDER BY id DESC;"
                                 : L"SELECT drive_letter, file_name, file_path, file_size, modify_time, usn, file_ref_number, parent_file_ref_number FROM archives ORDER BY id DESC;";

    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter)
    {
        sqlite3_bind_text16(stmt, 1, filter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, filter.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ArchiveFile_t f;

        const void* drive16 = sqlite3_column_text16(stmt, 0);
        const void* name16 = sqlite3_column_text16(stmt, 1);
        const void* path16 = sqlite3_column_text16(stmt, 2);
        if (drive16) f.driveLetter = reinterpret_cast<const wchar_t*>(drive16);
        if (name16) f.fileName = reinterpret_cast<const wchar_t*>(name16);
        if (path16) f.filePath = reinterpret_cast<const wchar_t*>(path16);

        f.fileSize = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        f.modifyTime = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        f.usn = static_cast<USN>(sqlite3_column_int64(stmt, 5));
        f.fileRefNumber = static_cast<DWORDLONG>(sqlite3_column_int64(stmt, 6));
        f.parentFileRefNumber = static_cast<DWORDLONG>(sqlite3_column_int64(stmt, 7));

        out->push_back(std::move(f));
    }

    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

void Database::Close()
{
    if (!db_) return;
    sqlite3_close(db_);
    db_ = nullptr;
}

bool Database::ExecSql16(const std::wstring& sql, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (;;)
    {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
        {
            continue;
        }
        if (rc == SQLITE_DONE)
        {
            break;
        }

        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::CreateArchivesTable(std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = R"(
            CREATE TABLE IF NOT EXISTS archives (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                drive_letter TEXT NOT NULL,
                file_ref_number INTEGER NOT NULL,
                parent_file_ref_number INTEGER NOT NULL,
                usn INTEGER NOT NULL,
                file_name TEXT NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                modify_time INTEGER NOT NULL,
                UNIQUE(drive_letter, file_ref_number)
            );
            CREATE INDEX IF NOT EXISTS idx_drive_letter ON archives(drive_letter);
            CREATE INDEX IF NOT EXISTS idx_usn ON archives(usn);
            CREATE INDEX IF NOT EXISTS idx_file_name ON archives(file_name);
        )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create table failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

static std::string FileTimeToString(const FILETIME& ft)
{
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    std::ostringstream oss;
    oss << st.wYear << "-"
        << std::setfill('0') << std::setw(2) << st.wMonth << "-"
        << std::setfill('0') << std::setw(2) << st.wDay << " "
        << std::setfill('0') << std::setw(2) << st.wHour << ":"
        << std::setfill('0') << std::setw(2) << st.wMinute << ":"
        << std::setfill('0') << std::setw(2) << st.wSecond;
    return oss.str();
}

bool Database::InsertOrUpdateArchive(const ArchiveFile_t& af)
{
    const char* sql = R"(
            INSERT OR REPLACE INTO archives
            (drive_letter, file_ref_number, parent_file_ref_number, usn, file_name, file_path, file_size, modify_time)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Prepare failed: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_text16(stmt, 1, af.driveLetter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, af.fileRefNumber);
    sqlite3_bind_int64(stmt, 3, af.parentFileRefNumber);
    sqlite3_bind_int64(stmt, 4, af.usn);
    sqlite3_bind_text16(stmt, 5, af.fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(stmt, 6, af.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, af.fileSize);
    sqlite3_bind_int64(stmt, 8, af.modifyTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Insert/Update failed: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool Database::InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    if (!BeginTransaction())
    {
        if (err) *err = L"BeginTransaction failed";
        return false;
    }

    for (const auto& f : files)
    {
        if (!InsertOrUpdateArchive(f))
        {
            RollbackTransaction();
            if (err)
            {
                const char* em = sqlite3_errmsg(db_);
                *err = em ? Utf8ToWString(em) : L"InsertOrUpdateArchive failed";
            }
            return false;
        }
    }

    if (!CommitTransaction())
    {
        RollbackTransaction();
        if (err) *err = L"CommitTransaction failed";
        return false;
    }

    return true;
}

bool Database::GetArchiveLastUsn(wchar_t driveLetter, USN* outUsn)
{
    const char* sql = "SELECT MAX(usn) FROM archives WHERE drive_letter = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }

    std::string driveStr(1, (char)driveLetter);
    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);

    *outUsn = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        *outUsn = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::DeleteArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber)
{
    const char* sql = "DELETE FROM archives WHERE drive_letter = ? AND file_ref_number = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }

    std::string driveStr(1, (char)driveLetter);
    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileRefNumber);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Database::GetArchiveCount()
{
    const char* sql = "SELECT COUNT(*) FROM archives";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return 0;
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Database::BeginTransaction()
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Begin transaction failed: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::CommitTransaction()
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Commit failed: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::RollbackTransaction()
{
    char* errMsg = nullptr;
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return true;
}
