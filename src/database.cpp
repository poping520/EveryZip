
#include "database.h"

#include <windows.h>

#include "sqlite3.h"

static std::wstring Utf8ToWString(const char* s) {
    if (!s) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring w;
    w.resize(static_cast<size_t>(needed - 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), needed);
    return w;
}

Database::Database() = default;

Database::~Database() {
    Close();
}

bool Database::Open(const std::wstring& dbPath, std::wstring* err) {
    if (err) err->clear();

    Close();

    sqlite3* db = nullptr;
    const int rc = sqlite3_open16(dbPath.c_str(), &db);
    if (rc != SQLITE_OK || !db) {
        if (err) {
            *err = L"sqlite3_open16 failed";
        }
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    db_ = db;
    return true;
}

bool Database::QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err) {
    if (err) err->clear();
    if (!db_) {
        if (err) *err = L"db not open";
        return false;
    }
    if (!out) {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

    sqlite3_stmt* stmt = nullptr;
    const bool hasFilter = !filter.empty();

    const std::wstring sql = hasFilter
        ? L"SELECT name, path, size, modifyTimestamp, usn, frn, pfrn FROM archives "
          L"WHERE name LIKE '%' || ? || '%' OR path LIKE '%' || ? || '%' ORDER BY id DESC;"
        : L"SELECT name, path, size, modifyTimestamp, usn, frn, pfrn FROM archives ORDER BY id DESC;";

    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter) {
        sqlite3_bind_text16(stmt, 1, filter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, filter.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ArchiveFile_t f;

        const void* name16 = sqlite3_column_text16(stmt, 0);
        const void* path16 = sqlite3_column_text16(stmt, 1);
        if (name16) f.name = reinterpret_cast<const wchar_t*>(name16);
        if (path16) f.path = reinterpret_cast<const wchar_t*>(path16);

        f.size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        f.modifyTimestamp = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        f.usn = static_cast<USN>(sqlite3_column_int64(stmt, 4));
        f.frn = static_cast<DWORDLONG>(sqlite3_column_int64(stmt, 5));
        f.pfrn = static_cast<DWORDLONG>(sqlite3_column_int64(stmt, 6));

        out->push_back(std::move(f));
    }

    if (rc != SQLITE_DONE) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

void Database::Close() {
    if (!db_) return;
    sqlite3_close(db_);
    db_ = nullptr;
}

bool Database::ExecSql16(const std::wstring& sql, std::wstring* err) {
    if (err) err->clear();
    if (!db_) {
        if (err) *err = L"db not open";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare16_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (;;) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            continue;
        }
        if (rc == SQLITE_DONE) {
            break;
        }

        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::CreateArchivesTable(std::wstring* err) {
    if (err) err->clear();
    if (!db_) {
        if (err) *err = L"db not open";
        return false;
    }

    const std::wstring sql =
        L"CREATE TABLE IF NOT EXISTS archives("
        L"id INTEGER PRIMARY KEY AUTOINCREMENT,"
        L"name TEXT NOT NULL,"
        L"path TEXT NOT NULL,"
        L"size INTEGER NOT NULL,"
        L"modifyTimestamp INTEGER NOT NULL,"
        L"usn INTEGER NOT NULL,"
        L"frn INTEGER NOT NULL,"
        L"pfrn INTEGER NOT NULL"
        L");";

    if (!ExecSql16(sql, err)) return false;

    const std::wstring dedupe =
        L"DELETE FROM archives WHERE rowid NOT IN (SELECT MIN(rowid) FROM archives GROUP BY path);";
    if (!ExecSql16(dedupe, err)) return false;

    const std::wstring uniq = L"CREATE UNIQUE INDEX IF NOT EXISTS uniq_archives_path ON archives(path);";
    if (!ExecSql16(uniq, err)) return false;

    const std::wstring idx1 = L"CREATE INDEX IF NOT EXISTS idx_archives_path ON archives(path);";
    if (!ExecSql16(idx1, err)) return false;
    const std::wstring idx2 = L"CREATE INDEX IF NOT EXISTS idx_archives_frn ON archives(frn);";
    if (!ExecSql16(idx2, err)) return false;

    return true;
}

bool Database::InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err) {
    if (err) err->clear();
    if (!db_) {
        if (err) *err = L"db not open";
        return false;
    }
    if (files.empty()) return true;

    if (!ExecSql16(L"BEGIN IMMEDIATE TRANSACTION;", err)) return false;

    sqlite3_stmt* stmt = nullptr;
    const std::wstring sql =
        L"INSERT INTO archives(name, path, size, modifyTimestamp, usn, frn, pfrn) "
        L"VALUES(?, ?, ?, ?, ?, ?, ?) "
        L"ON CONFLICT(path) DO UPDATE SET "
        L"name=excluded.name, "
        L"size=excluded.size, "
        L"modifyTimestamp=excluded.modifyTimestamp, "
        L"usn=excluded.usn, "
        L"frn=excluded.frn, "
        L"pfrn=excluded.pfrn;";

    int rc = sqlite3_prepare16_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (err) *err = L"sqlite3_prepare16_v2 failed";
        ExecSql16(L"ROLLBACK;", nullptr);
        return false;
    }

    for (const auto& f : files) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_text16(stmt, 1, f.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, f.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(f.size));
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(f.modifyTimestamp));
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(f.usn));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(f.frn));
        sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(f.pfrn));

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            if (err) {
                const char* em = sqlite3_errmsg(db_);
                *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
            }
            sqlite3_finalize(stmt);
            ExecSql16(L"ROLLBACK;", nullptr);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    if (!ExecSql16(L"COMMIT;", err)) {
        ExecSql16(L"ROLLBACK;", nullptr);
        return false;
    }

    return true;
}
