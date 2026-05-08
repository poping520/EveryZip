#pragma once

#include "archive_parser.h"

#include <string>
#include <vector>

namespace EveryZip {

/**
 * 基于 libarchive 的统一归档解析器，支持 zip、rar、7z、tar 等格式。
 */
class LibArchiveParser : public IArchiveParser {
public:
    LibArchiveParser();
    ~LibArchiveParser() override;

    bool Open(const std::wstring& archive_path, std::string* error) override;
    void Close() override;
    bool IsOpen() const override;
    std::wstring ArchivePath() const override;

    bool ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) override;

    bool ExtractEntry(const std::string& entry_path,
                      const std::wstring& dest_dir,
                      std::string* error) override;

private:
    std::wstring archive_path_;
};

} // namespace EveryZip
