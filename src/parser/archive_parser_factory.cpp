#include "archive_parser_factory.h"

#include <algorithm>
#include <cwctype>

#include "rar_archive_parser.h"
#include "sevenzip_archive_parser.h"
#include "zip_archive_parser.h"

namespace EveryZip {

static std::wstring GetLowerExtension(const std::wstring& path) {
    const size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};
    std::wstring ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return (wchar_t)towlower(ch);
    });
    return ext;
}

std::unique_ptr<IArchiveParser> CreateArchiveParserForPath(const std::wstring& archive_path) {
    const std::wstring ext = GetLowerExtension(archive_path);
    if (ext == L".7z") {
        return std::make_unique<SevenZipArchiveParser>();
    }
    if (ext == L".rar") {
        return std::make_unique<RarArchiveParser>();
    }
    if (ext == L".zip") {
        return std::make_unique<ZipArchiveParser>();
    }
    return nullptr;
}

std::unique_ptr<IArchiveParser> CreateArchiveParserByType(const std::wstring& parser_type) {
    std::wstring type = parser_type;
    std::transform(type.begin(), type.end(), type.begin(), [](wchar_t ch) {
        return (wchar_t)towlower(ch);
    });

    if (type == L"7z") {
        return std::make_unique<SevenZipArchiveParser>();
    }
    if (type == L"rar") {
        return std::make_unique<RarArchiveParser>();
    }
    if (type == L"zip") {
        return std::make_unique<ZipArchiveParser>();
    }
    return nullptr;
}

} // namespace EveryZip
