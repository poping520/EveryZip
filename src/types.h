#pragma once

#include <cstdint>
#include <string>

#include <Windows.h>

typedef struct ArchiveFile {
    // FileReferenceNumber
    DWORDLONG frn;
    //
    USN usn;
    // ParentFileReferenceNumber
    DWORDLONG pfrn;
    // 文件名称
    std::wstring name;
    // 文件路径
    std::wstring path;
    // 文件大小
    uint64_t size;
    // 文件修改时间
    uint64_t modifyTimestamp;

} ArchiveFile_t;