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

 // 判断归档条目名是否以目录分隔符结尾，以识别目录项。
 // 参数：s - 归档内原始条目名称。
 // 返回值：目录项返回 true，否则返回 false。
static bool EndsWithSlash(const std::string& s) {
    if (s.empty()) return false;
    const char c = s.back();
    return c == '/' || c == '\\';
}

 // 将归档条目名称转换为宽字符串。
 // isUtf8=true 时严格按 UTF-8 解码；为 false 时按系统代码页（CP_ACP/GBK）解码。
 // 参数：s - 原始窄字符串名称；isUtf8 - ZIP 条目的 UTF-8 标志位（flag bit 11）是否置位。
 // 返回値：转换后的宽字符串；失败时返回空字符串。
static std::wstring ToWideBestEffort(const std::string& s, bool isUtf8) {
    if (s.empty()) return std::wstring();

    auto convert = [&](UINT codepage, DWORD flags) -> std::wstring {
        const int needed = MultiByteToWideChar(codepage, flags, s.c_str(), (int)s.size(), nullptr, 0);
        if (needed <= 0) return std::wstring();
        std::wstring out;
        out.resize(needed);
        if (MultiByteToWideChar(codepage, flags, s.c_str(), (int)s.size(), out.data(), needed) <= 0) {
            return std::wstring();
        }
        return out;
    };

    if (isUtf8) {
        // 标志位明确指定 UTF-8，直接用 UTF-8 解码
        std::wstring w = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!w.empty()) return w;
        // UTF-8 解码失败则回退 CP_ACP
        return convert(CP_ACP, 0);
    } else {
        // 无 UTF-8 标志位：直接使用 CP_ACP（GBK/对应系统编码）
        std::wstring w = convert(CP_ACP, 0);
        if (!w.empty()) return w;
        return convert(CP_UTF8, 0);
    }
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

        const bool isUtf8 = (info.flag & 0x800) != 0;
        ArchiveEntry e;
        e.name = name;
        e.name_w = ToWideBestEffort(name, isUtf8);
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

bool ZipArchiveParser::ExtractEntry(const std::string& entry_path,
                                    const std::wstring& dest_dir,
                                    std::string* error) {
    if (!handle_) {
        if (error) *error = "archive is not open";
        return false;
    }
    if (entry_path.empty()) {
        if (error) *error = "entry_path is empty";
        return false;
    }

    // 定位到目标条目（不区分大小写）
    int rc = unzLocateFile((unzFile)handle_, entry_path.c_str(), 2);
    if (rc != UNZ_OK) {
        if (error) *error = "entry not found: " + entry_path;
        return false;
    }

    // 获取条目信息（用于判断是否目录）
    unz_file_info64 info{};
    char namebuf[4096]{};
    rc = unzGetCurrentFileInfo64((unzFile)handle_, &info, namebuf, sizeof(namebuf) - 1, nullptr, 0, nullptr, 0);
    if (rc != UNZ_OK) {
        if (error) *error = "unzGetCurrentFileInfo64 failed";
        return false;
    }

    std::string name(namebuf);
    if (EndsWithSlash(name)) {
        // 纯目录条目，直接创建目录即可
        const bool isUtf8dir = (info.flag & 0x800) != 0;
        std::wstring dirPath = dest_dir + L"\\" + ToWideBestEffort(name, isUtf8dir);
        // 替换正斜杠为反斜杠
        std::replace(dirPath.begin(), dirPath.end(), L'/', L'\\');
        // 移除末尾斜杠
        while (!dirPath.empty() && (dirPath.back() == L'\\' || dirPath.back() == L'/'))
            dirPath.pop_back();
        CreateDirectoryW(dirPath.c_str(), nullptr);
        return true;
    }

    // 只取文件名部分（去掉归档内路径结构）
    std::string basename = name;
    size_t slashPos = basename.find_last_of("/\\");
    if (slashPos != std::string::npos)
        basename = basename.substr(slashPos + 1);
    if (basename.empty()) {
        if (error) *error = "entry has no filename: " + entry_path;
        return false;
    }

    std::wstring destPath = dest_dir;
    if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
    const bool isUtf8file = (info.flag & 0x800) != 0;
    destPath += ToWideBestEffort(basename, isUtf8file);

    // 打开当前条目准备读取
    rc = unzOpenCurrentFile((unzFile)handle_);
    if (rc != UNZ_OK) {
        if (error) *error = "unzOpenCurrentFile failed";
        return false;
    }

    // 创建目标文件
    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        unzCloseCurrentFile((unzFile)handle_);
        if (error) *error = "CreateFileW failed for: " + entry_path;
        return false;
    }

    // 循环读取并写入
    bool ok = true;
    std::vector<char> buf(65536);
    for (;;) {
        int bytes = unzReadCurrentFile((unzFile)handle_, buf.data(), (unsigned)buf.size());
        if (bytes < 0) {
            if (error) *error = "unzReadCurrentFile failed";
            ok = false;
            break;
        }
        if (bytes == 0) break;
        DWORD written = 0;
        if (!WriteFile(hFile, buf.data(), (DWORD)bytes, &written, nullptr) || written != (DWORD)bytes) {
            if (error) *error = "WriteFile failed";
            ok = false;
            break;
        }
    }

    CloseHandle(hFile);
    int crcRc = unzCloseCurrentFile((unzFile)handle_);
    if (ok && crcRc == UNZ_CRCERROR) {
        if (error) *error = "CRC mismatch for: " + entry_path;
        return false;
    }
    return ok;
}

} // namespace EveryArchive
