#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "../src/parser/sevenzip_archive_parser.h"
#include "../src/string_utils.h"

static std::wstring ArgToWide(const char* arg) {
    if (!arg) return {};
    return Utf8ToWString(arg);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: TestSevenZip <archive.7z> [entry-name]\n";
        return 0;
    }

    EveryZip::SevenZipArchiveParser parser;
    std::string err;
    if (!parser.Open(ArgToWide(argv[1]), &err)) {
        std::cerr << "Open failed: " << err << "\n";
        return 1;
    }

    std::vector<ArchiveEntry_t> entries;
    if (!parser.ListEntries(&entries, &err)) {
        std::cerr << "ListEntries failed: " << err << "\n";
        return 1;
    }

    bool sawFile = false;
    for (const auto& entry : entries) {
        std::cerr << entry.entryPathUtf8
                  << " compressed=" << entry.compressedSize
                  << " uncompressed=" << entry.originalSize
                  << "\n";
        if (!entry.isDirectory) {
            sawFile = true;
            if (entry.entryPathUtf8.empty()) {
                std::cerr << "Entry name conversion failed\n";
                return 1;
            }
        }
    }

    if (!sawFile) {
        std::cerr << "No file entries found\n";
        return 1;
    }

    if (argc >= 3) {
        wchar_t tempPath[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, tempPath)) {
            std::cerr << "GetTempPathW failed\n";
            return 1;
        }
        std::wstring destDir = tempPath;
        destDir += L"EveryZipTestSevenZip";
        CreateDirectoryW(destDir.c_str(), nullptr);

        if (!parser.ExtractEntry(argv[2], destDir, &err)) {
            std::cerr << "ExtractEntry failed: " << err << "\n";
            return 1;
        }
    }

    parser.Close();
    return 0;
}
