
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

    std::wstring testFile = L"C:\\Users\\123\\Desktop\\apk\\com.guomilive.xtkj.apk";

    EveryArchive::ZipArchiveParser parser;

    std::string err;
    parser.Open(testFile, &err);

    std::vector<EveryArchive::ArchiveEntry> entries;
    parser.ListEntries(&entries, &err);

    for (const auto& entry : entries) {
        std::cerr << entry.name << "\n";
    }

    parser.Close();

    return 0;
}
