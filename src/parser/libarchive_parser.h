#pragma once

#include "archive_parser.h"

#include <string>
#include <vector>

namespace EveryZip {

/**
 * 历史草稿：libarchive 在 list 模式下无法可靠提供逐条目压缩后大小，
 * 因此不作为 EveryZip 的主线解析器。
 */
class LibArchiveParser : public IArchiveParser {
public:
    LibArchiveParser();
    ~LibArchiveParser() override;

    bool Open(const std::wstring& archive_path, std::string* error) override;
    void Close() override;
    bool IsOpen() const override;
    std::wstring ArchivePath() const override;

    bool ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) override;

    bool ExtractEntry(const std::string& entry_path,
                      const std::wstring& dest_dir,
                      std::string* error) override;

private:
    std::wstring archive_path_;
};

} // namespace EveryZip
