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
    /** 销毁归档解析器并释放底层资源。 */
    virtual ~IArchiveParser() = default;

    /**
     * 打开指定归档文件，准备后续读取条目。
     * @param archive_path 归档文件路径。
     * @param error 可选，用于输出错误信息。
     * @return 打开成功返回 true，否则返回 false。
     */
    virtual bool Open(const std::wstring& archive_path, std::string* error) = 0;

    /** 关闭当前已打开的归档文件。 */
    virtual void Close() = 0;

    /**
     * 判断当前解析器是否处于已打开状态。
     * @return 已打开返回 true，否则返回 false。
     */
    virtual bool IsOpen() const = 0;

    /**
     * 获取当前已打开归档文件的路径。
     * @return 归档文件路径；未打开时通常为空字符串。
     */
    virtual std::wstring ArchivePath() const = 0;

    /**
     * 枚举当前归档中的全部条目元数据。
     * @param out_entries 输出条目列表。
     * @param error 可选，用于输出错误信息。
     * @return 枚举成功返回 true，否则返回 false。
     */
    virtual bool ListEntries(std::vector<ArchiveEntry>* out_entries, std::string* error) = 0;

    /**
     * 将归档内指定条目解压到目标目录。
     * @param entry_path 归档内条目路径（与 ArchiveEntry::name 一致）。
     * @param dest_dir   解压目标目录路径（宽字符），目录须已存在或由实现自动创建。
     * @param error      可选，用于输出错误信息。
     * @return 解压成功返回 true，否则返回 false。
     */
    virtual bool ExtractEntry(const std::string& entry_path,
                              const std::wstring& dest_dir,
                              std::string* error) = 0;
};

} // namespace EveryArchive
