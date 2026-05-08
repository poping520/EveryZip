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
    uint64_t modifyTime = 0;
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
    std::wstring entryPath;
    std::string entryRawPath;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
};
