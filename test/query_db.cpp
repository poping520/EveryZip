#include <sqlite3.h>
#include <iostream>
#include <iomanip>

int main() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open("file_tracker.db", &db);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    const char* sql = "SELECT COUNT(*) as total, drive_letter FROM files GROUP BY drive_letter";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Prepare failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return 1;
    }

    std::cout << "=== Database Statistics ===" << std::endl;
    int64_t totalFiles = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t count = sqlite3_column_int64(stmt, 0);
        const char* drive = (const char*)sqlite3_column_text(stmt, 1);
        std::cout << "Drive " << drive << ": " << count << " files" << std::endl;
        totalFiles += count;
    }
    sqlite3_finalize(stmt);

    std::cout << "Total files in database: " << totalFiles << std::endl;
    std::cout << std::endl;

    if (totalFiles > 0) {
        const char* sql2 = "SELECT drive_letter, file_name, file_size FROM files LIMIT 10";
        rc = sqlite3_prepare_v2(db, sql2, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            std::cout << "=== Sample Files (first 10) ===" << std::endl;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* drive = (const char*)sqlite3_column_text(stmt, 0);
                const char* name = (const char*)sqlite3_column_text(stmt, 1);
                int64_t size = sqlite3_column_int64(stmt, 2);
                std::cout << drive << ":\\ " << name << " (" << size << " bytes)" << std::endl;
            }
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    return 0;
}
