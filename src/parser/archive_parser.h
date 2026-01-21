#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace EveryArchive {

struct ArchiveEntry {
    std::string name;
    std::wstring name_w;

    bool is_directory = false;

    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;

    std::uint32_t compression_method = 0;
    std::uint32_t external_attributes = 0;

    std::tm modified_time{};
};

class IArchiveParser {
public:
    virtual ~IArchiveParser() = default;

    virtual bool Open(const std::wstring& archive_path, std::string* error) = 0;
    virtual void Close() = 0;

    virtual bool IsOpen() const = 0;
    virtual std::wstring ArchivePath() const = 0;

    virtual bool ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) = 0;
};

} // namespace EveryArchive
