#include "zip_archive_parser.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include "ioapi.h"
#include "iowin32.h"
#include "unzip.h"
}

namespace EveryArchive {

static bool EndsWithSlash(const std::string& s) {
    if (s.empty()) return false;
    const char c = s.back();
    return c == '/' || c == '\\';
}

static std::wstring ToWideBestEffort(const std::string& s) {
    if (s.empty()) return std::wstring();

    auto convert = [&](UINT codepage) -> std::wstring {
        const int needed = MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (needed <= 0) return std::wstring();
        std::wstring out;
        out.resize(needed);
        if (MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), out.data(), needed) <= 0) {
            return std::wstring();
        }
        return out;
    };

    std::wstring w = convert(CP_UTF8);
    if (!w.empty()) return w;

    w = convert(CP_ACP);
    return w;
}

ZipArchiveParser::ZipArchiveParser() = default;

ZipArchiveParser::~ZipArchiveParser() {
    Close();
}

bool ZipArchiveParser::Open(const std::wstring& archive_path, std::string* error) {
    Close();

    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }

    zlib_filefunc64_def filefunc{};
    fill_win32_filefunc64W(&filefunc);

    unzFile uf = unzOpen2_64(archive_path.c_str(), &filefunc);
    if (!uf) {
        if (error) *error = "unzOpen2_64 failed";
        return false;
    }

    handle_ = uf;
    archive_path_ = archive_path;
    return true;
}

void ZipArchiveParser::Close() {
    if (handle_) {
        unzClose((unzFile)handle_);
        handle_ = nullptr;
    }
    archive_path_.clear();
}

bool ZipArchiveParser::IsOpen() const {
    return handle_ != nullptr;
}

std::wstring ZipArchiveParser::ArchivePath() const {
    return archive_path_;
}

bool ZipArchiveParser::ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) {
    if (!out_entries) {
        if (error) *error = "out_entries is null";
        return false;
    }
    out_entries->clear();

    if (!handle_) {
        if (error) *error = "archive is not open";
        return false;
    }

    int rc = unzGoToFirstFile((unzFile)handle_);
    if (rc != UNZ_OK) {
        if (rc == UNZ_END_OF_LIST_OF_FILE) return true;
        if (error) *error = "unzGoToFirstFile failed";
        return false;
    }

    while (rc == UNZ_OK) {
        unz_file_info64 info{};
        std::vector<char> namebuf;
        namebuf.resize(4096);

        rc = unzGetCurrentFileInfo64((unzFile)handle_, &info, namebuf.data(), (uLong)namebuf.size(), nullptr, 0, nullptr, 0);
        if (rc != UNZ_OK) {
            if (error) *error = "unzGetCurrentFileInfo64 failed";
            return false;
        }

        namebuf.back() = '\0';
        std::string name(namebuf.data());

        ArchiveEntry e;
        e.name = name;
        e.name_w = ToWideBestEffort(name);
        e.is_directory = EndsWithSlash(name);
        e.compressed_size = (std::uint64_t)info.compressed_size;
        e.uncompressed_size = (std::uint64_t)info.uncompressed_size;
        e.crc32 = (std::uint32_t)info.crc;
        e.compression_method = (std::uint32_t)info.compression_method;
        e.external_attributes = (std::uint32_t)info.external_fa;

        std::tm t{};
        t.tm_sec = info.tmu_date.tm_sec;
        t.tm_min = info.tmu_date.tm_min;
        t.tm_hour = info.tmu_date.tm_hour;
        t.tm_mday = info.tmu_date.tm_mday;
        t.tm_mon = info.tmu_date.tm_mon;
        t.tm_year = info.tmu_date.tm_year - 1900;
        e.modified_time = t;

        out_entries->push_back(std::move(e));

        rc = unzGoToNextFile((unzFile)handle_);
        if (rc == UNZ_END_OF_LIST_OF_FILE) break;
        if (rc != UNZ_OK) {
            if (error) *error = "unzGoToNextFile failed";
            return false;
        }
    }

    return true;
}

} // namespace EveryArchive
