#include "libarchive_parser.h"

#include <windows.h>
#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "../string_utils.h"

namespace EveryZip {

// 将宽字符串转换为 UTF-8 窄字符串
static std::string WStringToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// 判断路径是否以目录分隔符结尾
static bool EndsWithSlash(const char* s) {
    if (!s || !*s) return false;
    const char c = s[strlen(s) - 1];
    return c == '/' || c == '\\';
}

LibArchiveParser::LibArchiveParser() = default;

LibArchiveParser::~LibArchiveParser() {
    Close();
}

bool LibArchiveParser::Open(const std::wstring& archive_path, std::string* error) {
    Close();
    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }
    // 仅记录路径，实际读取在 ListEntries/ExtractEntry 中按需打开
    archive_path_ = archive_path;
    return true;
}

void LibArchiveParser::Close() {
    archive_path_.clear();
}

bool LibArchiveParser::IsOpen() const {
    return !archive_path_.empty();
}

std::wstring LibArchiveParser::ArchivePath() const {
    return archive_path_;
}

bool LibArchiveParser::ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) {
    if (!out_entries) {
        if (error) *error = "out_entries is null";
        return false;
    }
    out_entries->clear();
    return ForEachEntry([&](const ArchiveEntry_t& entry) {
        out_entries->push_back(entry);
        return true;
    }, error);
}

bool LibArchiveParser::ForEachEntry(const std::function<bool(const ArchiveEntry_t&)>& visitor,
                                    std::string* error) {
    if (!visitor) {
        if (error) *error = "visitor is null";
        return false;
    }
    if (archive_path_.empty()) {
        if (error) *error = "archive is not open";
        return false;
    }

    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    // libarchive 在 Windows 上使用宽字符路径
    int r = archive_read_open_filename_w(a, archive_path_.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : "archive_read_open failed";
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* pathUtf8 = archive_entry_pathname_utf8(entry);
        if (!pathUtf8) pathUtf8 = archive_entry_pathname(entry);
        if (!pathUtf8) {
            archive_read_data_skip(a);
            continue;
        }

        ArchiveEntry_t e;
        e.entryPathUtf8 = pathUtf8;
        e.isDirectory = (archive_entry_filetype(entry) == AE_IFDIR) || EndsWithSlash(pathUtf8);

        e.originalSize = (std::uint64_t)archive_entry_size(entry);
        e.compressedSize = -1; // libarchive list 模式不提供压缩后大小

        const struct stat* st = archive_entry_stat(entry);
        if (st) {
            std::time_t mt = st->st_mtime;
            const std::tm* tm_ptr = std::localtime(&mt);
            if (tm_ptr) e.modifiedTime = LocalTmToFileTimeValue(*tm_ptr);
        }

        if (!visitor(e)) {
            if (error) *error = "entry visitor stopped";
            archive_read_free(a);
            return false;
        }
        archive_read_data_skip(a);
    }

    if (r != ARCHIVE_EOF) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : "archive read error";
        archive_read_free(a);
        return false;
    }

    archive_read_free(a);
    return true;
}

bool LibArchiveParser::ExtractEntry(const std::string& entry_path,
                                    const std::wstring& dest_dir,
                                    std::string* error) {
    if (archive_path_.empty()) {
        if (error) *error = "archive is not open";
        return false;
    }
    if (entry_path.empty()) {
        if (error) *error = "entry_path is empty";
        return false;
    }

    // 路径分隔符统一化（用于比较）
    auto normPath = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    const std::string targetNorm = normPath(entry_path);

    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    int r = archive_read_open_filename_w(a, archive_path_.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : "archive_read_open failed";
        archive_read_free(a);
        return false;
    }

    bool found = false;
    bool ok    = false;
    struct archive_entry* entry;

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* pathUtf8 = archive_entry_pathname_utf8(entry);
        if (!pathUtf8) pathUtf8 = archive_entry_pathname(entry);
        if (!pathUtf8) { archive_read_data_skip(a); continue; }

        if (normPath(std::string(pathUtf8)) != targetNorm) {
            archive_read_data_skip(a);
            continue;
        }

        found = true;

        // 构造目标文件路径：dest_dir\文件名
        std::string nameStr(pathUtf8);
        const size_t slash = nameStr.find_last_of("/\\");
        const std::string basename = (slash != std::string::npos) ? nameStr.substr(slash + 1) : nameStr;
        std::wstring destPath = dest_dir;
        if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
        destPath += Utf8ToWString(basename.c_str());

        // 用写入磁盘 writer 解压单个条目
        struct archive* wr = archive_write_disk_new();
        archive_write_disk_set_options(wr, ARCHIVE_EXTRACT_TIME);

        // 修改条目目标路径
        const std::string destUtf8 = WStringToUtf8(destPath);
        archive_entry_set_pathname(entry, destUtf8.c_str());

        r = archive_write_header(wr, entry);
        if (r == ARCHIVE_OK) {
            const void* buf;
            size_t size;
            la_int64_t offset;
            while ((r = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK) {
                if (archive_write_data_block(wr, buf, size, offset) != ARCHIVE_OK) {
                    if (error) *error = archive_error_string(wr) ? archive_error_string(wr) : "write error";
                    break;
                }
            }
            if (r == ARCHIVE_EOF) {
                archive_write_finish_entry(wr);
                ok = true;
            } else if (!ok && error) {
                *error = archive_error_string(a) ? archive_error_string(a) : "read data error";
            }
        } else {
            if (error) *error = archive_error_string(wr) ? archive_error_string(wr) : "write header error";
        }

        archive_write_free(wr);
        break;
    }

    archive_read_free(a);

    if (!found) {
        if (error) *error = "entry not found: " + entry_path;
        return false;
    }
    return ok;
}

} // namespace EveryZip
