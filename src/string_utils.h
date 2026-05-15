#pragma once

#include <windows.h>
#include <shlwapi.h>

#include <cstdint>
#include <ctime>
#include <cwctype>
#include <string>

/**
 * 将宽字符串中的英文字母转换为小写形式。
 * @param s 待转换的宽字符串，会按值传入并在函数内部修改副本。
 * @return 转换完成后的新宽字符串。
 */
std::wstring ToLower(std::wstring s);
/**
 * 将 UTF-16 宽字符串编码为 UTF-8 字节串。
 * @param w 待转换的宽字符串内容。
 * @return 转换后的 UTF-8 字符串；转换失败或输入为空时返回空字符串。
 */
std::string WideToUtf8(const std::wstring& w);
/**
 * 使用指定 Windows 代码页将字节串解码为宽字符串。
 * @param s 待解码的字节串，可包含非 UTF-8 编码内容。
 * @param codepage Windows 代码页，例如 CP_UTF8 或 CP_ACP。
 * @param flags 传给 MultiByteToWideChar 的标志，例如 MB_ERR_INVALID_CHARS。
 * @return 转换后的宽字符串；转换失败或输入为空时返回空字符串。
 */
std::wstring MultiByteToWString(const std::string& s, UINT codepage, DWORD flags = 0);
/**
 * 将 UTF-16 代码单元序列转换为宽字符串。
 * @param src UTF-16 代码单元指针，可以为 nullptr。
 * @param lenWithNull 代码单元数量，可以包含末尾 null。
 * @return 转换后的宽字符串；输入为空时返回空字符串。
 */
std::wstring Utf16UnitsToWString(const uint16_t* src, size_t lenWithNull);
/**
 * 将 UTF-8 C 字符串解码为宽字符串。
 * @param s 指向 UTF-8 字符串的指针，可以为 nullptr。
 * @return 转换后的宽字符串；输入为空指针或转换失败时返回空字符串。
 */
std::wstring Utf8ToWString(const char* s);
/**
 * 获取当前可执行文件所在目录。
 * @return 可执行文件所在目录路径；获取失败时返回当前目录 "."。
 */
std::wstring GetExeDir();
/**
 * 从完整路径中提取末尾的文件名或条目名。
 * @param path 原始路径，可以包含 '/' 或 '\\' 分隔符。
 * @return 路径最后一段名称；若路径以分隔符结尾则返回空字符串。
 */
std::wstring GetEntryNameFromPath(const std::wstring& path);
/**
 * 为数字字符串添加千位分隔符，便于界面展示。
 * @param num 仅包含数字字符的字符串。
 * @return 插入逗号后的格式化字符串。
 */
std::wstring AddThousandsSeparator(const std::wstring& num);
/**
 * 将字节大小格式化为适合显示的 KB 文本。
 * @param v 原始字节数。
 * @return 格式化后的大小字符串。
 */
std::wstring FormatSizeULongLong(ULONGLONG v);
uint64_t LocalTmToFileTimeValue(const std::tm& t);
std::wstring FormatFileTimeValueLocal(uint64_t v);
/**
 * 将 UTC FILETIME 转换为本地时间文本。
 * @param ftUtc UTC 时区下的 FILETIME 时间值。
 * @return 本地时间格式化字符串；转换失败时返回空字符串。
 */
std::wstring FormatFileTimeLocal(const FILETIME& ftUtc);
/**
 * 将 64 位整数形式的 FILETIME 还原为 FILETIME 结构。
 * @param v 以 uint64_t 表示的 FILETIME 数值。
 * @return 拆分后的 FILETIME 结构。
 */
FILETIME U64ToFileTime(uint64_t v);
