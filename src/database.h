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
class Database
{
public:
    Database();

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /**
     * 打开指定路径的 SQLite 数据库并初始化连接参数。
     * @param dbPath 数据库文件路径
     * @param err 可选，用于输出错误信息。
     * @return 打开成功返回 true，否则返回 false。
     */
    bool Open(const std::wstring& dbPath, std::wstring* err);

    /** 关闭当前数据库连接 */
    void Close();

    /**
     * 判断数据库连接当前是否已打开。
     * @return 已打开返回 true，否则返回 false。
     */
    bool IsOpen() const { return db_ != nullptr; }

    /**
     * 设置 SQLite 忙等待超时（毫秒），防止并发读写时立即返回 SQLITE_BUSY。
     * @param ms 超时毫秒数。
     */
    void SetBusyTimeout(int ms);

    /**
     * 创建保存配置键值对的 configs 表。
     * @param err 可选，用于输出错误信息。
     * @return 创建成功返回 true，否则返回 false。
     */
    bool CreateConfigsTable(std::wstring* err);

    /**
     * 保存指定盘符的 USN Journal 位置。
     * @param driveLetter 盘符。
     * @param journalId Journal 标识。
     * @param nextUsn 下次扫描起点。
     * @param err 可选错误输出。
     * @return 保存成功返回 true，否则返回 false。
     */
    bool SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err);

    bool SaveConfigValue(const std::string& key, const std::string& value, std::wstring* err);
    bool GetConfigValue(const std::string& key, std::string* outValue);

    /**
     * 读取指定盘符已保存的 USN Journal 位置。
     * @param driveLetter 盘符。
     * @param outJournalId 输出 Journal 标识。
     * @param outNextUsn 输出 USN 起点。
     * @return 读取成功返回 true，否则返回 false。
     */
    bool GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn);


    /**
     * 创建归档文件元数据表 archives。
     * @param err 可选，用于输出错误信息。
     * @return 创建成功返回 true，否则返回 false。
     */
    bool CreateArchivesTable(std::wstring* err);

    /**
     * 插入或更新一条归档文件记录。
     * @param archiveFile 要写入的归档文件信息。
     * @return 写入成功返回 true，否则返回 false。
     */
    bool InsertOrUpdateArchive(const ArchiveFile_t& archiveFile);

    /**
     * 批量插入或更新多条归档文件记录。
     * @param files 归档文件列表。
     * @param err 可选，用于输出错误信息。
     * @return 批量写入成功返回 true，否则返回 false。
     */
    bool InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err);

    /**
     * 根据盘符和文件引用号查询单个归档文件记录。
     * @param driveLetter 盘符。
     * @param fileRefNumber 文件引用号。
     * @param out 输出归档信息。
     * @return 找到记录返回 true，否则返回 false。
     */
    bool QueryArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out);

    /**
     * 创建归档内部条目表 entries。
     * @param err 可选，用于输出错误信息。
     * @return 创建成功返回 true，否则返回 false。
     */
    bool CreateEntriesTable(std::wstring* err);

    /**
     * 根据归档路径删除其全部内部条目。
     * @param archivePath 归档文件完整路径。
     * @param err 可选错误输出。
     * @return 删除成功返回 true，否则返回 false。
     */
    bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err);

    /**
     * 根据归档主键删除其全部内部条目。
     * @param archiveId 归档记录主键。
     * @param err 可选错误输出。
     * @return 删除成功返回 true，否则返回 false。
     */
    bool DeleteEntriesByArchiveId(int64_t archiveId, std::wstring* err);

    /**
     * 插入单个归档内部条目。
     * @param entry 条目数据。
     * @return 写入成功返回 true，否则返回 false。
     */
    bool InsertOrUpdateEntry(const ArchiveEntry_t& entry);

    /**
     * 批量写入归档内部条目。
     * @param entries 条目列表。
     * @param err 可选错误输出。
     * @return 写入成功返回 true，否则返回 false。
     */
    bool InsertEntriesBatch(const std::vector<ArchiveEntry_t>& entries, std::wstring* err);

    /**
     * 查询指定盘符下已索引归档记录的最大 USN。
     * @param driveLetter 盘符。
     * @param outUsn 输出最大 USN。
     * @return 查询成功返回 true，否则返回 false。
     */
    bool GetArchiveLastUsn(wchar_t driveLetter, USN* outUsn);

    /**
     * 根据盘符和文件引用号删除归档记录。
     * @param driveLetter 盘符。
     * @param fileRefNumber 文件引用号。
     * @return 删除成功返回 true，否则返回 false。
     */
    bool DeleteArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber);

    /**
     * 统计当前归档记录总数。
     * @return archives 表中的记录数。
     */
    int64_t GetArchiveCount();

    /**
     * 根据归档路径查询归档主键。
     * @param filePath 归档文件路径。
     * @return 找到时返回归档主键，否则返回 -1。
     */
    int64_t GetArchiveIdByPath(const std::wstring& filePath);

    /**
     * 开启数据库事务。
     * @return 开启成功返回 true，否则返回 false。
     */
    bool BeginTransaction();

    /**
     * 提交当前事务。
     * @return 提交成功返回 true，否则返回 false。
     */
    bool CommitTransaction();

    /**
     * 回滚当前事务。
     * @return 始终返回 true。
     */
    bool RollbackTransaction();

    /**
     * 执行 VACUUM 以整理数据库空间。
     * @return 执行成功返回 true，否则返回 false。
     */
    bool Vacuum();


    /**
     * 查询符合关键字的归档文件列表。
     * @param filter 搜索关键字。
     * @param out 输出归档结果。
     * @param err 可选错误输出。
     * @return 查询成功返回 true，否则返回 false。
     */
    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err);

    /**
     * 查询符合关键字的归档内部条目列表。
     * @param filter 搜索关键字。
     * @param out 输出条目结果。
     * @param err 可选错误输出。
     * @return 查询成功返回 true，否则返回 false。
     */
    bool QueryEntries(const std::wstring& filter, std::vector<ArchiveEntry_t>* out, std::wstring* err);

    /**
     * 纯虚拟列表支持：只查询 rowid 列表（内存极低），按需查询单行。
     * @param filter 搜索关键字。
     * @param sortColumn 排序列索引。
     * @param sortAsc 是否升序。
     * @param outIds 输出 rowid 列表。
     * @param err 可选错误输出。
     * @return 查询成功返回 true，否则返回 false。
     */
    bool QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc,
                       std::vector<int64_t>* outIds, std::wstring* err);

    /**
     * 按 rowid 查询单条归档内部条目详情。
     * @param rowId entries 表行标识。
     * @param out 输出条目数据。
     * @return 找到记录返回 true，否则返回 false。
     */
    bool QueryEntryById(int64_t rowId, ArchiveEntry_t* out);

    /**
     * 统计符合关键字的条目数量。
     * @param filter 搜索关键字。
     * @return 匹配条目的数量。
     */
    int64_t GetEntryCount(const std::wstring& filter);

private:
    /**
     * 执行一段 UTF-16 SQL 文本。
     * @param sql SQL 语句。
     * @param err 可选错误输出。
     * @return 执行成功返回 true，否则返回 false。
     */
    bool ExecSql16(const std::wstring& sql, std::wstring* err);

    sqlite3* db_ = nullptr;
};
