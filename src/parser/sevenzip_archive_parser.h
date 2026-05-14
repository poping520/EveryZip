#pragma once

#include <memory>
#include <string>

#include "archive_parser.h"

namespace EveryZip {

class SevenZipArchiveParser final : public IArchiveParser {
public:
    SevenZipArchiveParser();
    ~SevenZipArchiveParser() override;

    SevenZipArchiveParser(const SevenZipArchiveParser&) = delete;
    SevenZipArchiveParser& operator=(const SevenZipArchiveParser&) = delete;

    bool Open(const std::wstring& archive_path, std::string* error) override;
    void Close() override;
    bool IsOpen() const override;
    std::wstring ArchivePath() const override;

    bool ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) override;

    bool ExtractEntry(const std::string& entry_path,
                      const std::wstring& dest_dir,
                      std::string* error) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace EveryZip
