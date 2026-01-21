#pragma once

#include "types.h"

#include <string>
#include <vector>

class FileScanner {
public:
    FileScanner() = default;
    ~FileScanner() = default;

    bool Scan(std::vector<ArchiveFile_t>* out, std::wstring* err);
};
