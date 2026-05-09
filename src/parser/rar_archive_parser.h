#pragma once

#include "archive_parser.h"

#include <memory>
#include <string>
#include <vector>

namespace EveryZip {

class RarArchiveParser final : public IArchiveParser {
public:
    RarArchiveParser();
    ~RarArchiveParser() override;

    RarArchiveParser(const RarArchiveParser&) = delete;
    RarArchiveParser& operator=(const RarArchiveParser&) = delete;

    bool Open(const std::wstring& archive_path, std::string* error) override;
    void Close() override;
    bool IsOpen() const override;
    std::wstring ArchivePath() const override;

    bool ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) override;

    bool ExtractEntry(const std::string& entry_path,
                      const std::wstring& dest_dir,
                      std::string* error) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace EveryZip
