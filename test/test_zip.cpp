
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/parser/zip_archive_parser.h"



int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::wstring testFile = L"C:\\Users\\123\\Desktop\\zip\\sample.zip";

    EveryZip::ZipArchiveParser parser;

    std::string err;
    parser.Open(testFile, &err);

    std::vector<ArchiveEntry_t> entries;
    parser.ListEntries(&entries, &err);

    for (const auto& entry : entries) {
        std::cerr << entry.entryRawPath << "\n";
    }

    parser.Close();

    return 0;
}
