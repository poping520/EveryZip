#pragma once

#include <cstdint>
#include <string>

#include <Windows.h>

// 归档文件
struct ArchiveFile_t
{
    // 盘符
    std::wstring driveLetter;
    // 文件名称
    std::wstring fileName;
    // 文件路径
    std::wstring filePath;
    // 文件大小
    uint64_t fileSize = 0;
    // 文件修改时间
    uint64_t modifiedTime = 0;
    // FileReferenceNumber
    DWORDLONG fileRefNumber = 0;
    // ParentFileReferenceNumber
    DWORDLONG parentFileRefNumber = 0;
    //
    USN usn = 0;
};

// USN Journal 增量变化记录
struct UsnChangeRecord_t
{
    wchar_t driveLetter = 0;
    DWORDLONG fileRefNumber = 0;
    DWORDLONG parentFileRefNumber = 0;
    DWORD reason = 0;           // USN_REASON_* 标志
    std::wstring fileName;
    USN usn = 0;
};

// 归档文件内容
struct ArchiveEntry_t
{
    int64_t archiveId = 0;          // archives 表的 id（插入时使用）
    std::wstring archivePath;       // archives.file_path（查询时由 JOIN 填充）
    std::string entryPathUtf8;      // 规范化后的 UTF-8 条目路径（entries.entry_path）
    std::string entryRawPath;
    bool isDirectory = false;       // 解析归档时使用；目录项不写入 entries 表
    std::int64_t compressedSize = 0;   // -1 表示该格式无法提供可靠的逐文件压缩大小
    std::uint64_t originalSize = 0;
    std::uint64_t modifiedTime = 0;
};
