#pragma once

#include <cstdint>
#include <string>

#include <Windows.h>

// 归档文件
typedef struct ArchiveFile
{
    // 盘符
    std::wstring driveLetter;
    // 文件名称
    std::wstring fileName;
    // 文件路径
    std::wstring filePath;
    // 文件大小
    uint64_t fileSize;
    // 文件修改时间
    uint64_t modifyTime;
    // FileReferenceNumber
    DWORDLONG fileRefNumber;
    // ParentFileReferenceNumber
    DWORDLONG parentFileRefNumber;
    //
    USN usn;
} ArchiveFile_t;

// 归档文件内容
typedef struct ArchiveEntry
{
    std::wstring archivePath;
    std::wstring entryName;
    std::wstring entryPath;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
} ArchiveEntry_t;
