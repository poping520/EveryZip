#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "database.h"
#include "config/user_config.h"

/** USN Journal 元数据 */
struct JournalInfo {
    int64_t journalId = 0;
    USN nextUsn = 0;
};

class FileScanner {
public:
    /** 默认构造文件扫描器对象 */
    FileScanner() = default;
    /** 默认析构文件扫描器对象 */
    ~FileScanner() = default;

    /**
     * 设置需要扫描的归档文件扩展名。
     * @param exts 允许识别为归档文件的扩展名列表。
     */
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    /**
     * 执行全量 MFT 扫描，收集所有匹配扩展名的归档文件。
     * @param out 输出扫描得到的归档文件列表。
     * @param err 可选，用于输出错误信息。
     * @param cancel 可选取消标志，置位后提前结束扫描。
     * @return 扫描成功返回 true，否则返回 false。
     */
    bool Scan(std::vector<ArchiveFile_t>* out, std::wstring* err, std::atomic_bool* cancel = nullptr);

    /**
     * 通过 FileReferenceNumber 获取文件信息（大小、修改时间、完整路径）。
     * @param hVol 卷句柄。
     * @param fileRefNumber 文件引用号。
     * @param outFileSize 输出文件大小。
     * @param outModifyTime 输出修改时间。
     * @param outFullPath 输出完整路径。
     * @return 查询成功返回 true，否则返回 false。
     */
    static bool GetFileInfoByRefNumber(HANDLE hVol, uint64_t fileRefNumber,
                                       uint64_t* outFileSize, uint64_t* outModifyTime,
                                       std::wstring* outFullPath);

    /**
     * 获取指定盘符的 USN Journal 元数据。
     * @param driveLetter 盘符。
     * @param out 输出 Journal 元数据。
     * @param err 可选，用于输出错误信息。
     * @return 获取成功返回 true，否则返回 false。
     */
    static bool QueryJournalInfo(wchar_t driveLetter, JournalInfo* out, std::wstring* err);

    /**
     * 增量读取 USN Journal，从 startUsn 开始读取归档文件变化记录。
     * @param driveLetter 盘符。
     * @param journalId 期望的 Journal 标识。
     * @param startUsn 起始 USN。
     * @param out 输出变化记录列表。
     * @param outNextUsn 输出新的结束 USN。
     * @param err 可选，用于输出错误信息。
     * @param cancel 可选取消标志。
     * @param extensions 可选扩展名列表，为空时使用默认值。
     * @return 读取成功返回 true，否则返回 false。
     */
    static bool ScanUsnJournal(wchar_t driveLetter, int64_t journalId, USN startUsn,
                               std::vector<UsnChangeRecord_t>* out, USN* outNextUsn,
                               std::wstring* err, std::atomic_bool* cancel = nullptr,
                               const std::vector<std::wstring>* extensions = nullptr);

private:
    std::vector<std::wstring> archiveExtensions_ = { L".zip" };
};
