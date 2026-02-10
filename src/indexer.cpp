#include "indexer.h"

#include "logger.h"
#include "resource.h"
#include "string_utils.h"
#include "parser/zip_archive_parser.h"

#include <unordered_map>

Indexer::Indexer() = default;

Indexer::~Indexer() {
    Stop();
}

void Indexer::SetDbPath(const std::wstring& dbPath) {
    dbPath_ = dbPath;
}

bool Indexer::EnsureDatabaseReady() {
    const DWORD attr = GetFileAttributesW(dbPath_.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(dbPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }

    std::wstring err;
    Database db;
    if (!db.Open(dbPath_, &err)) {
        LOG_WARN(L"Database::Open failed: %s", err.c_str());
        return false;
    }
    if (!db.CreateArchivesTable(&err)) {
        LOG_WARN(L"CreateArchivesTable failed: %s", err.c_str());
        return false;
    }
    if (!db.CreateEntriesTable(&err)) {
        LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
        return false;
    }
    return true;
}

bool Indexer::EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        CloseHandle(hToken);
        return false;
    }

    const DWORD err = GetLastError();
    CloseHandle(hToken);
    return err != ERROR_NOT_ALL_ASSIGNED;
}

void Indexer::ParseAndStoreArchive(Database& db, const ArchiveFile_t& a) {
    EveryArchive::ZipArchiveParser parser;
    std::string perr;
    if (!parser.Open(a.filePath, &perr)) {
        LOG_WARN(L"ZipArchiveParser::Open failed: %s", Utf8ToWString(perr.c_str()).c_str());
        return;
    }

    std::vector<EveryArchive::ArchiveEntry> parsed;
    if (!parser.ListEntries(&parsed, &perr)) {
        LOG_WARN(L"ZipArchiveParser::ListEntries failed: %s", Utf8ToWString(perr.c_str()).c_str());
        parser.Close();
        return;
    }
    parser.Close();

    int64_t archiveId = db.GetArchiveIdByPath(a.filePath);
    if (archiveId < 0) {
        LOG_WARN(L"GetArchiveIdByPath failed for: %s", a.filePath.c_str());
        return;
    }


    std::vector<ArchiveEntry_t> entries;
    entries.reserve(parsed.size());
    for (const auto& e : parsed) {
        if (e.is_directory) continue;

        ArchiveEntry_t out;
        out.archiveId = archiveId;
        out.entryPath = e.name_w.empty() ? Utf8ToWString(e.name.c_str()) : e.name_w;
        out.compressed_size = e.compressed_size;
        out.uncompressed_size = e.uncompressed_size;
        entries.push_back(std::move(out));
    }

    std::wstring entryErr;
    if (!db.DeleteEntriesByArchiveId(archiveId, &entryErr)) {
        LOG_WARN(L"DeleteEntriesByArchiveId failed: %s", entryErr.c_str());
    }
    if (!entries.empty()) {
        if (!db.InsertEntriesBatch(entries, &entryErr)) {
            LOG_WARN(L"InsertEntriesBatch failed: %s", entryErr.c_str());
        }
    }
}

std::vector<wchar_t> Indexer::GetMonitoredDrives() {
    std::vector<wchar_t> drives;
    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) return drives;

    std::wstring buf;
    buf.resize(needed);
    if (GetLogicalDriveStringsW(needed, buf.data()) == 0) return drives;

    const wchar_t* p = buf.c_str();
    while (*p) {
        std::wstring root = p;
        p += root.size() + 1;
        if (root.size() < 2) continue;

        UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN) continue;

        // 检查是否为 NTFS
        wchar_t fsName[MAX_PATH] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) continue;
        if (_wcsicmp(fsName, L"NTFS") != 0) continue;

        wchar_t driveLetter = root[0];

        // test: 仅监控 E 盘（与 FileScanner::Scan 保持一致）
        if (driveLetter != L'E') continue;

        drives.push_back(driveLetter);
    }
    return drives;
}

void Indexer::Stop() {
    LOG_INFO(L"StopIndexing requested");
    cancel_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
    LOG_INFO(L"StopIndexing done");
}

// 启动后台索引线程：扫描磁盘 → 增量更新数据库 → 解析归档条目 → 进入监控循环
void Indexer::Start(HWND hWnd) {
    LOG_INFO(L"StartIndexing requested");
    Stop();
    cancel_.store(false);
    running_.store(true);

    thread_ = std::thread([this, hWnd]() {
        FileScanner scanner;
        std::vector<ArchiveFile_t> scanned;
        std::wstring err;
        const bool scanOk = scanner.Scan(&scanned, &err, &cancel_);
        if (!scanOk) {
            LOG_WARN(L"FileScanner::Scan failed: %s", err.c_str());
        }

        if (!cancel_.load() && scanOk) {
            Database db;
            if (!db.Open(dbPath_, &err)) {
                LOG_WARN(L"Database::Open failed: %s", err.c_str());
            } else if (!db.CreateArchivesTable(&err)) {
                LOG_WARN(L"CreateArchivesTable failed: %s", err.c_str());
            } else if (!db.CreateEntriesTable(&err)) {
                LOG_WARN(L"CreateEntriesTable failed: %s", err.c_str());
            } else if (!db.CreateConfigsTable(&err)) {
                LOG_WARN(L"CreateConfigsTable failed: %s", err.c_str());
            } else {
                std::vector<ArchiveFile_t> old;
                if (!db.QueryArchives(L"", &old, &err)) {
                    LOG_WARN(L"QueryArchives failed: %s", err.c_str());
                } else {
                    struct OldInfo {
                        ArchiveFile_t file;
                        bool seen = false;
                    };
                    std::unordered_map<std::wstring, OldInfo> oldMap;
                    oldMap.reserve(old.size());

                    auto makeKey = [](const ArchiveFile_t& af) -> std::wstring {
                        return af.driveLetter + L":" + std::to_wstring((unsigned long long)af.fileRefNumber);
                    };

                    for (const auto& o : old) {
                        oldMap.emplace(makeKey(o), OldInfo{ o, false });
                    }

                    std::vector<ArchiveFile_t> upserts;
                    std::vector<ArchiveFile_t> toParse;

                    for (const auto& cur : scanned) {
                        if (cancel_.load()) break;

                        const std::wstring key = makeKey(cur);
                        auto it = oldMap.find(key);
                        if (it == oldMap.end()) {
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                            continue;
                        }

                        it->second.seen = true;
                        const auto& prev = it->second.file;
                        const bool changed = (cur.usn != prev.usn) || (cur.modifyTime != prev.modifyTime) || (cur.fileSize != prev.fileSize) || (cur.filePath != prev.filePath);
                        if (changed) {
                            if (!prev.filePath.empty()) {
                                std::wstring delErr;
                                if (!db.DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                    LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                                }
                            }
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                        }
                    }

                    for (const auto& kv : oldMap) {
                        if (cancel_.load()) break;
                        const auto& prev = kv.second.file;
                        if (kv.second.seen) continue;

                        if (!prev.driveLetter.empty()) {
                            db.DeleteArchiveByRefNumber(prev.driveLetter[0], (uint64_t)prev.fileRefNumber);
                        }
                        if (!prev.filePath.empty()) {
                            std::wstring delErr;
                            if (!db.DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                            }
                        }
                    }

                    if (!cancel_.load() && !upserts.empty()) {
                        if (!db.InsertArchivesBatch(upserts, &err)) {
                            LOG_WARN(L"InsertArchivesBatch failed: %s", err.c_str());
                        }
                    }

                    for (const auto& a : toParse) {
                        if (cancel_.load()) break;
                        ParseAndStoreArchive(db, a);
                    }
                }

                // 初始扫描完成后，记录每个盘符的 Journal NextUsn 作为监控起点
                if (!cancel_.load()) {
                    auto drives = GetMonitoredDrives();
                    for (wchar_t dl : drives) {
                        JournalInfo ji;
                        if (FileScanner::QueryJournalInfo(dl, &ji, &err)) {
                            db.SaveJournalUsn(dl, ji.journalId, ji.nextUsn, &err);
                            LOG_INFO(L"Saved Journal USN for drive %c: journalId=%lld, nextUsn=%lld",
                                     dl, (long long)ji.journalId, (long long)ji.nextUsn);
                        }
                    }
                }

            }
        }

        // 标记初始索引完成，通知 UI 刷新
        running_.store(false);
        PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);

        // ═══════════════════════════════════════════════════════════
        //  监控循环：定期读取 USN Journal 增量变化，实时同步数据库
        // ═══════════════════════════════════════════════════════════
        LOG_INFO(L"Entering USN Journal monitoring loop");

        while (!cancel_.load()) {
            // 每 2 秒检查一次变化
            for (int i = 0; i < 20 && !cancel_.load(); ++i) {
                Sleep(100);
            }
            if (cancel_.load()) break;

            Database db;
            std::wstring err;
            if (!db.Open(dbPath_, &err)) {
                LOG_WARN(L"Monitor: Database::Open failed: %s", err.c_str());
                continue;
            }

            auto drives = GetMonitoredDrives();
            bool anyChanged = false;

            for (wchar_t dl : drives) {
                if (cancel_.load()) break;

                // 读取上次保存的 Journal 位置
                int64_t savedJournalId = 0;
                USN savedNextUsn = 0;
                db.GetJournalUsn(dl, &savedJournalId, &savedNextUsn);

                if (savedJournalId == 0 && savedNextUsn == 0) {
                    // 没有保存过，跳过（等待下次全量扫描）
                    continue;
                }

                // 增量读取 USN Journal 变化
                std::vector<UsnChangeRecord_t> changes;
                USN newNextUsn = 0;
                std::wstring scanErr;
                if (!FileScanner::ScanUsnJournal(dl, savedJournalId, savedNextUsn,
                                                  &changes, &newNextUsn, &scanErr, &cancel_)) {
                    LOG_WARN(L"Monitor: ScanUsnJournal failed for %c: %s", dl, scanErr.c_str());
                    continue;
                }

                if (changes.empty()) {
                    // 即使没有归档文件变化，也更新 USN 位置避免重复扫描
                    if (newNextUsn > savedNextUsn) {
                        db.SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                    }
                    continue;
                }

                LOG_INFO(L"Monitor: %zu USN changes detected on drive %c", changes.size(), dl);

                // 按 fileRefNumber 去重，只保留每个文件的最后一条记录
                std::unordered_map<uint64_t, UsnChangeRecord_t> deduped;
                for (auto& cr : changes) {
                    deduped[(uint64_t)cr.fileRefNumber] = std::move(cr);
                }

                for (const auto& kv : deduped) {
                    if (cancel_.load()) break;
                    const auto& cr = kv.second;

                    bool isDelete = (cr.reason & USN_REASON_FILE_DELETE) != 0;
                    bool isRenameOld = (cr.reason & USN_REASON_RENAME_OLD_NAME) != 0;

                    if (isDelete || isRenameOld) {
                        // 文件被删除或重命名（旧名）：从数据库中移除
                        ArchiveFile_t oldAf;
                        if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            LOG_INFO(L"Monitor: Archive deleted/renamed: %s", oldAf.filePath.c_str());
                            if (!oldAf.filePath.empty()) {
                                std::wstring delErr;
                                db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                            db.DeleteArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber);
                            anyChanged = true;
                        }
                    } else {
                        // 文件新增或修改：获取最新文件信息，重新解析入库
                        // 通过 OpenFileById 获取文件的完整路径和元数据
                        wchar_t volPath[] = L"\\\\.\\X:";
                        volPath[4] = dl;
                        HANDLE hVol = CreateFileW(volPath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
                        if (hVol == INVALID_HANDLE_VALUE) continue;

                        FILE_ID_DESCRIPTOR fid{};
                        fid.dwSize = sizeof(fid);
                        fid.Type = FileIdType;
                        fid.FileId.QuadPart = (LONGLONG)cr.fileRefNumber;

                        HANDLE hFile = OpenFileById(hVol, &fid, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, 0);

                        if (hFile == INVALID_HANDLE_VALUE) {
                            CloseHandle(hVol);
                            // 文件可能已被删除，清理数据库
                            ArchiveFile_t oldAf;
                            if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                                if (!oldAf.filePath.empty()) {
                                    std::wstring delErr;
                                    db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                                }
                                db.DeleteArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber);
                                anyChanged = true;
                            }
                            continue;
                        }

                        BY_HANDLE_FILE_INFORMATION fileInfo{};
                        GetFileInformationByHandle(hFile, &fileInfo);

                        uint64_t fileSize = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;
                        ULARGE_INTEGER ui{};
                        ui.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;
                        ui.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
                        uint64_t modifyTime = ui.QuadPart;

                        // 获取完整路径
                        std::wstring fullPath;
                        DWORD need = GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                        if (need > 0) {
                            fullPath.resize(need);
                            DWORD got = GetFinalPathNameByHandleW(hFile, fullPath.data(), need, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                            if (got > 0 && got < need) {
                                fullPath.resize(got);
                                if (fullPath.size() >= 4 && fullPath[0] == L'\\' && fullPath[1] == L'\\' && fullPath[2] == L'?' && fullPath[3] == L'\\') {
                                    fullPath.erase(0, 4);
                                }
                            }
                        }
                        CloseHandle(hFile);
                        CloseHandle(hVol);

                        if (fullPath.empty()) continue;

                        // 先删除旧的条目（如果路径变了）
                        ArchiveFile_t oldAf;
                        if (db.QueryArchiveByRefNumber(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            if (!oldAf.filePath.empty() && oldAf.filePath != fullPath) {
                                std::wstring delErr;
                                db.DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                        }

                        // 更新 archives 表
                        ArchiveFile_t af;
                        af.driveLetter = std::wstring(1, dl);
                        af.fileName = cr.fileName;
                        af.filePath = fullPath;
                        af.fileSize = fileSize;
                        af.modifyTime = modifyTime;
                        af.fileRefNumber = cr.fileRefNumber;
                        af.parentFileRefNumber = cr.parentFileRefNumber;
                        af.usn = cr.usn;
                        db.InsertOrUpdateArchive(af);

                        // 重新解析归档文件内容
                        LOG_INFO(L"Monitor: Re-parsing archive: %s", fullPath.c_str());
                        ParseAndStoreArchive(db, af);
                        anyChanged = true;
                    }
                }

                // 更新 Journal USN 位置
                if (newNextUsn > savedNextUsn) {
                    db.SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                }
            }

            // 如果有变化，通知 UI 刷新
            if (anyChanged && !cancel_.load()) {
                PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
            }
        }

        LOG_INFO(L"USN Journal monitoring loop exited");
    });
}
