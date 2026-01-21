#pragma once

#include <string>

#include "archive_parser.h"

namespace EveryArchive {

class ZipArchiveParser final : public IArchiveParser {
public:
    ZipArchiveParser();
    ~ZipArchiveParser() override;

    bool Open(const std::wstring& archive_path, std::string* error) override;
    void Close() override;

    bool IsOpen() const override;
    std::wstring ArchivePath() const override;

    bool ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) override;

private:
    void* handle_ = nullptr; // unzFile
    std::wstring archive_path_;
};

} // namespace EveryArchive
