#include <windows.h>
#include <winioctl.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>

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

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

static std::string FileTimeToString(const FILETIME& ft) {
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

class FileDatabase {
public:
    FileDatabase() : db_(nullptr) {}

    ~FileDatabase() {
        Close();
    }

    bool Open(const char* dbPath) {
        int rc = sqlite3_open(dbPath, &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        return CreateTables();
    }

    void Close() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool BeginTransaction() {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "Begin transaction failed: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    bool CommitTransaction() {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "Commit failed: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    bool RollbackTransaction() {
        char* errMsg = nullptr;
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg);
        return true;
    }

    bool InsertOrUpdateFile(wchar_t driveLetter, uint64_t fileRefNumber, uint64_t parentFileRefNumber,
                           USN usn, const std::wstring& fileName, const std::wstring& filePath,
                           uint64_t fileSize, const FILETIME& modifyTime) {
        const char* sql = R"(
            INSERT OR REPLACE INTO files
            (drive_letter, file_ref_number, parent_file_ref_number, usn, file_name, file_path, file_size, modified_time)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Prepare failed: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }

        std::string driveStr(1, (char)driveLetter);
        std::string fileNameUtf8 = WideToUtf8(fileName);
        std::string filePathUtf8 = WideToUtf8(filePath);
        std::string modifyTimeStr = FileTimeToString(modifyTime);

        sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, fileRefNumber);
        sqlite3_bind_int64(stmt, 3, parentFileRefNumber);
        sqlite3_bind_int64(stmt, 4, usn);
        sqlite3_bind_text(stmt, 5, fileNameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, filePathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 7, fileSize);
        sqlite3_bind_text(stmt, 8, modifyTimeStr.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            std::cerr << "Insert/Update failed: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        return true;
    }

    bool GetLastUsn(wchar_t driveLetter, USN* outUsn) {
        const char* sql = "SELECT MAX(usn) FROM files WHERE drive_letter = ?";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }

        std::string driveStr(1, (char)driveLetter);
        sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);

        *outUsn = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *outUsn = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return true;
    }

    bool DeleteFileByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber) {
        const char* sql = "DELETE FROM files WHERE drive_letter = ? AND file_ref_number = ?";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }

        std::string driveStr(1, (char)driveLetter);
        sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, fileRefNumber);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    int64_t GetFileCount() {
        const char* sql = "SELECT COUNT(*) FROM files";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }

        int64_t count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

private:
    bool CreateTables() {
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS files (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                drive_letter TEXT NOT NULL,
                file_ref_number INTEGER NOT NULL,
                parent_file_ref_number INTEGER NOT NULL,
                usn INTEGER NOT NULL,
                file_name TEXT NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                modified_time TEXT NOT NULL,
                UNIQUE(drive_letter, file_ref_number)
            );
            CREATE INDEX IF NOT EXISTS idx_drive_letter ON files(drive_letter);
            CREATE INDEX IF NOT EXISTS idx_usn ON files(usn);
            CREATE INDEX IF NOT EXISTS idx_file_name ON files(file_name);
        )";

        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "Create table failed: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    sqlite3* db_;
};

struct FileInfo {
    uint64_t fileRefNumber;
    uint64_t parentFileRefNumber;
    USN usn;
    std::wstring fileName;
    std::wstring filePath;
    uint64_t fileSize;
    FILETIME modifyTime;
};

static bool GetFileInfoByRefNumber(HANDLE hVol, uint64_t fileRefNumber, FileInfo* outInfo) {
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
        outInfo->fileSize = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;
        outInfo->modifyTime = fileInfo.ftLastWriteTime;

        std::wstring fullPath;
        DWORD need = GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED);
        if (need != 0) {
            fullPath.resize(need);
            DWORD got = GetFinalPathNameByHandleW(hFile, fullPath.data(), need, FILE_NAME_NORMALIZED);
            if (got != 0) {
                while (!fullPath.empty() && fullPath.back() == L'\0') {
                    fullPath.pop_back();
                }
            } else {
                fullPath.clear();
            }
        }
        outInfo->filePath = std::move(fullPath);
    }

    CloseHandle(hFile);
    return success;
}

static bool HasTargetExt(const wchar_t* name, size_t len) {
    auto endsWithI = [&](const wchar_t* ext) -> bool {
        const size_t extLen = wcslen(ext);
        if (len < extLen) return false;
        return _wcsicmp(std::wstring(name + (len - extLen), extLen).c_str(), ext) == 0;
    };

    return endsWithI(L".zip") || endsWithI(L".7z") || endsWithI(L".rar");
}

static bool ScanDriveByUsn(wchar_t driveLetter, FileDatabase& db, bool isInitialScan,
                          uint64_t* outProcessed, uint64_t* outInserted, uint64_t* outDeleted) {
    *outProcessed = 0;
    *outInserted = 0;
    *outDeleted = 0;

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

    USN lastUsn = 0;
    if (!isInitialScan) {
        db.GetLastUsn(driveLetter, &lastUsn);
    }

    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = isInitialScan ? 0 : lastUsn;
    med.HighUsn = journal.NextUsn;

    std::vector<unsigned char> buffer;
    buffer.resize(1u << 20);

    db.BeginTransaction();

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
            db.RollbackTransaction();
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

            (*outProcessed)++;

            const wchar_t* fileName = reinterpret_cast<const wchar_t*>(
                reinterpret_cast<const unsigned char*>(rec) + rec->FileNameOffset);
            const size_t fileNameLen = static_cast<size_t>(rec->FileNameLength / sizeof(wchar_t));
            std::wstring fileNameStr(fileName, fileNameLen);

            if (HasTargetExt(fileName, fileNameLen)) {
                if (rec->Reason & USN_REASON_FILE_DELETE) {
                    db.DeleteFileByRefNumber(driveLetter, rec->FileReferenceNumber);
                    (*outDeleted)++;
                } else {
                    FileInfo info = {};
                    info.fileRefNumber = rec->FileReferenceNumber;
                    info.parentFileRefNumber = rec->ParentFileReferenceNumber;
                    info.usn = rec->Usn;
                    info.fileName = fileNameStr;

                    if (GetFileInfoByRefNumber(hVol, rec->FileReferenceNumber, &info)) {
                        if (db.InsertOrUpdateFile(driveLetter, info.fileRefNumber, info.parentFileRefNumber,
                                                 info.usn, info.fileName, info.filePath, info.fileSize,
                                                 info.modifyTime)) {
                            (*outInserted)++;
                                                 }
                    }
                }
            }

            offset += rec->RecordLength;
        }

        med.StartFileReferenceNumber = *pUsn;
    }

    db.CommitTransaction();
    CloseHandle(hVol);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    (void)argc;
    (void)argv;

    std::wcout << L"=== File Tracker with SQLite3 and USN Journal ===" << std::endl;
    std::wcout << std::endl;

    EnablePrivilege(L"SeBackupPrivilege");

    FileDatabase db;
    if (!db.Open("file_tracker.db")) {
        std::wcerr << L"Failed to open database" << std::endl;
        return 1;
    }

    int64_t initialCount = db.GetFileCount();
    bool isInitialScan = (initialCount == 0);

    if (isInitialScan) {
        std::wcout << L"Database is empty. Performing initial full scan..." << std::endl;
    } else {
        std::wcout << L"Database has " << initialCount << L" files. Performing incremental update..." << std::endl;
    }
    std::wcout << std::endl;

    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) {
        std::wcerr << L"GetLogicalDriveStringsW failed: " << GetLastError() << std::endl;
        return 1;
    }

    std::wstring drives;
    drives.resize(needed);
    if (GetLogicalDriveStringsW(needed, drives.data()) == 0) {
        std::wcerr << L"GetLogicalDriveStringsW failed: " << GetLastError() << std::endl;
        return 1;
    }

    uint64_t totalProcessed = 0;
    uint64_t totalInserted = 0;
    uint64_t totalDeleted = 0;

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
            std::wcout << root << L" => Skipped (not NTFS)" << std::endl;
            continue;
        }

        const wchar_t driveLetter = root[0];

        uint64_t processed = 0;
        uint64_t inserted = 0;
        uint64_t deleted = 0;

        std::wcout << driveLetter << L":\\ => Scanning..." << std::flush;

        if (!ScanDriveByUsn(driveLetter, db, isInitialScan, &processed, &inserted, &deleted)) {
            std::wcout << L" FAILED (err=" << GetLastError() << L")" << std::endl;
            continue;
        }

        totalProcessed += processed;
        totalInserted += inserted;
        totalDeleted += deleted;

        std::wcout << L" OK (processed=" << processed
                  << L", inserted/updated=" << inserted
                  << L", deleted=" << deleted << L")" << std::endl;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::wcout << std::endl;
    std::wcout << L"=== Summary ===" << std::endl;
    std::wcout << L"Total records processed: " << totalProcessed << std::endl;
    std::wcout << L"Total inserted/updated: " << totalInserted << std::endl;
    std::wcout << L"Total deleted: " << totalDeleted << std::endl;
    std::wcout << L"Total files in database: " << db.GetFileCount() << std::endl;
    std::wcout << L"Time elapsed: " << ms << L" ms" << std::endl;

    return 0;
}
