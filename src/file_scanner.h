#pragma once

#include <string>
 #include <vector>

#include "database.h"

class FileScanner {
public:
    FileScanner() = default;
    ~FileScanner() = default;

    bool Scan(std::vector<ArchiveFile_t>* out, std::wstring* err);
};
