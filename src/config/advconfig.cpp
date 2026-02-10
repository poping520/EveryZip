
#include "advconfig.h"

#include <cwctype>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <windows.h>

namespace AdvConfig
{

    // ============================================================================
    // ConfigValue 实现
    // ============================================================================

    Value::Type Value::GetType() const
    {
        return static_cast<Type>(data_.index());
    }

    bool Value::AsBool(bool defaultVal) const
    {
        if (auto *p = std::get_if<bool>(&data_))
            return *p;
        return defaultVal;
    }

    int64_t Value::AsInt(int64_t defaultVal) const
    {
        if (auto *p = std::get_if<int64_t>(&data_))
            return *p;
        if (auto *p = std::get_if<double>(&data_))
            return static_cast<int64_t>(*p);
        return defaultVal;
    }

    double Value::AsFloat(double defaultVal) const
    {
        if (auto *p = std::get_if<double>(&data_))
            return *p;
        if (auto *p = std::get_if<int64_t>(&data_))
            return static_cast<double>(*p);
        return defaultVal;
    }

    static const std::wstring kEmptyString;
    static const Value::List kEmptyList;
    static const Value::Dict kEmptyDict;

    const std::wstring &Value::AsString() const
    {
        if (auto *p = std::get_if<std::wstring>(&data_))
            return *p;
        return kEmptyString;
    }

    const Value::List &Value::AsList() const
    {
        if (auto *p = std::get_if<List>(&data_))
            return *p;
        return kEmptyList;
    }

    const Value::Dict &Value::AsDict() const
    {
        if (auto *p = std::get_if<Dict>(&data_))
            return *p;
        return kEmptyDict;
    }

    // ============================================================================
    // ConfigParser 实现
    // ============================================================================

    static const Value kNullValue;

    const Value &Parser::Get(const std::wstring &key) const
    {
        auto it = entries_.find(key);
        if (it != entries_.end())
            return it->second;
        return kNullValue;
    }

    void Parser::Set(const std::wstring &key, const Value &value)
    {
        if (entries_.find(key) == entries_.end())
        {
            keyOrder_.push_back(key);
        }
        entries_[key] = value;
    }

    bool Parser::Contains(const std::wstring &key) const
    {
        return entries_.find(key) != entries_.end();
    }

    bool Parser::Remove(const std::wstring &key)
    {
        auto it = entries_.find(key);
        if (it == entries_.end())
            return false;
        entries_.erase(it);
        keyOrder_.erase(
            std::remove(keyOrder_.begin(), keyOrder_.end(), key),
            keyOrder_.end());
        return true;
    }

    void Parser::Clear()
    {
        entries_.clear();
        keyOrder_.clear();
    }

    // --- 解析辅助 ---

    wchar_t Parser::Peek(const ParseContext &ctx) const
    {
        if (ctx.pos >= ctx.content->size())
            return L'\0';
        return (*ctx.content)[ctx.pos];
    }

    wchar_t Parser::Advance(ParseContext &ctx)
    {
        if (ctx.pos >= ctx.content->size())
            return L'\0';
        wchar_t c = (*ctx.content)[ctx.pos++];
        if (c == L'\n')
        {
            ctx.line++;
            ctx.col = 1;
        }
        else
        {
            ctx.col++;
        }
        return c;
    }

    bool Parser::IsAtEnd(const ParseContext &ctx) const
    {
        return ctx.pos >= ctx.content->size();
    }

    void Parser::SkipWhitespace(ParseContext &ctx)
    {
        while (!IsAtEnd(ctx))
        {
            wchar_t c = Peek(ctx);
            if (c == L' ' || c == L'\t' || c == L'\r')
            {
                Advance(ctx);
            }
            else
                break;
        }
    }

    void Parser::SkipWhitespaceAndNewlines(ParseContext &ctx)
    {
        while (!IsAtEnd(ctx))
        {
            wchar_t c = Peek(ctx);
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
            {
                Advance(ctx);
            }
            else if (c == L'#')
            {
                SkipToNextLine(ctx);
            }
            else
                break;
        }
    }

    void Parser::SkipToNextLine(ParseContext &ctx)
    {
        while (!IsAtEnd(ctx) && Peek(ctx) != L'\n')
        {
            Advance(ctx);
        }
        if (!IsAtEnd(ctx))
            Advance(ctx); // skip '\n'
    }

    std::wstring Parser::FormatError(const ParseContext &ctx, const std::wstring &msg) const
    {
        return L"line " + std::to_wstring(ctx.line) + L", col " + std::to_wstring(ctx.col) + L": " + msg;
    }

    // --- 解析入口 ---

    bool Parser::Parse(const std::wstring &content, std::wstring *err)
    {
        entries_.clear();
        keyOrder_.clear();

        // 跳过 BOM (U+FEFF)
        std::wstring cleaned = content;
        if (!cleaned.empty() && cleaned[0] == L'\xFEFF')
        {
            cleaned.erase(0, 1);
        }

        ParseContext ctx{&cleaned, 0, 1, 1};
        return ParseEntries(ctx, err);
    }

    bool Parser::LoadFile(const std::wstring &filePath, std::wstring *err)
    {
        FILE *fp = _wfopen(filePath.c_str(), L"rb");
        if (!fp)
        {
            if (err)
                *err = L"Cannot open file: " + filePath;
            return false;
        }
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        std::string bytes(fileSize, '\0');
        fread(&bytes[0], 1, fileSize, fp);
        fclose(fp);

        std::wstring wContent;
        if (bytes.size() >= 2)
        {
            auto u = [](char c) -> unsigned char { return static_cast<unsigned char>(c); };

            if (bytes.size() >= 3 && u(bytes[0]) == 0xEF && u(bytes[1]) == 0xBB && u(bytes[2]) == 0xBF)
            {
                // UTF-8 BOM
                const char *data = bytes.data() + 3;
                int dataLen = (int)bytes.size() - 3;
                int len = MultiByteToWideChar(CP_UTF8, 0, data, dataLen, nullptr, 0);
                if (len > 0)
                {
                    wContent.resize(len);
                    MultiByteToWideChar(CP_UTF8, 0, data, dataLen, &wContent[0], len);
                }
            }
            else if (u(bytes[0]) == 0xFF && u(bytes[1]) == 0xFE)
            {
                // UTF-16 LE BOM
                const wchar_t *data = reinterpret_cast<const wchar_t *>(bytes.data() + 2);
                size_t wLen = (bytes.size() - 2) / sizeof(wchar_t);
                wContent.assign(data, wLen);
            }
            else if (u(bytes[0]) == 0xFE && u(bytes[1]) == 0xFF)
            {
                // UTF-16 BE BOM — swap bytes to LE
                size_t wLen = (bytes.size() - 2) / sizeof(wchar_t);
                wContent.resize(wLen);
                const unsigned char *src = reinterpret_cast<const unsigned char *>(bytes.data() + 2);
                for (size_t i = 0; i < wLen; ++i)
                {
                    wContent[i] = static_cast<wchar_t>((src[i * 2] << 8) | src[i * 2 + 1]);
                }
            }
            else
            {
                // No BOM — assume UTF-8
                int len = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
                if (len > 0)
                {
                    wContent.resize(len);
                    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &wContent[0], len);
                }
            }
        }
        else if (!bytes.empty())
        {
            // 1 byte, treat as UTF-8
            int len = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
            if (len > 0)
            {
                wContent.resize(len);
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &wContent[0], len);
            }
        }
        return Parse(wContent, err);
    }

    bool Parser::SaveFile(const std::wstring &filePath, std::wstring *err) const
    {
        std::wstring wContent = Serialize();

        // wstring -> UTF-8 bytes
        std::string bytes;
        if (!wContent.empty())
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, wContent.data(), (int)wContent.size(), nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                bytes.resize(len);
                WideCharToMultiByte(CP_UTF8, 0, wContent.data(), (int)wContent.size(), &bytes[0], len, nullptr, nullptr);
            }
        }

        FILE *fp = _wfopen(filePath.c_str(), L"wb");
        if (!fp)
        {
            if (err)
                *err = L"Cannot open file for writing: " + filePath;
            return false;
        }
        // UTF-8 BOM
        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        fwrite(bom, 1, sizeof(bom), fp);
        if (!bytes.empty())
        {
            fwrite(bytes.data(), 1, bytes.size(), fp);
        }
        fclose(fp);
        return true;
    }

    bool Parser::ParseEntries(ParseContext &ctx, std::wstring *err)
    {
        while (true)
        {
            SkipWhitespaceAndNewlines(ctx);
            if (IsAtEnd(ctx))
                break;

            wchar_t c = Peek(ctx);

            // 跳过注释行
            if (c == L'#' || c == L';')
            {
                SkipToNextLine(ctx);
                continue;
            }

            // 解析 key
            size_t keyStart = ctx.pos;
            while (!IsAtEnd(ctx))
            {
                wchar_t ch = Peek(ctx);
                if (ch == L'=' || ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r')
                    break;
                Advance(ctx);
            }
            std::wstring key = ctx.content->substr(keyStart, ctx.pos - keyStart);
            if (key.empty())
            {
                if (err)
                    *err = FormatError(ctx, L"Expected key");
                return false;
            }

            SkipWhitespace(ctx);

            if (IsAtEnd(ctx) || Peek(ctx) != L'=')
            {
                if (err)
                    *err = FormatError(ctx, L"Expected '=' after key '" + key + L"'");
                return false;
            }
            Advance(ctx); // skip '='

            SkipWhitespace(ctx);

            Value value;
            if (!ParseValue(ctx, value, err))
            {
                return false;
            }

            if (entries_.find(key) == entries_.end())
            {
                keyOrder_.push_back(key);
            }
            entries_[key] = std::move(value);

            // 跳过行尾剩余空白和注释
            SkipWhitespace(ctx);
            if (!IsAtEnd(ctx) && Peek(ctx) == L'#')
            {
                SkipToNextLine(ctx);
            }
        }
        return true;
    }

    bool Parser::ParseValue(ParseContext &ctx, Value &out, std::wstring *err)
    {
        SkipWhitespace(ctx);
        if (IsAtEnd(ctx) || Peek(ctx) == L'\n')
        {
            out = Value();
            return true;
        }

        wchar_t c = Peek(ctx);

        if (c == L'"')
        {
            std::wstring s;
            if (!ParseString(ctx, s, err))
                return false;
            out = Value(std::move(s));
            return true;
        }

        if (c == L'[')
        {
            Value::List list;
            if (!ParseList(ctx, list, err))
                return false;
            out = Value(std::move(list));
            return true;
        }

        if (c == L'{')
        {
            Value::Dict dict;
            if (!ParseDict(ctx, dict, err))
                return false;
            out = Value(std::move(dict));
            return true;
        }

        return ParseScalar(ctx, out, err);
    }

    bool Parser::ParseString(ParseContext &ctx, std::wstring &out, std::wstring *err)
    {
        if (Peek(ctx) != L'"')
        {
            if (err)
                *err = FormatError(ctx, L"Expected '\"'");
            return false;
        }
        Advance(ctx); // skip opening '"'

        out.clear();
        while (!IsAtEnd(ctx))
        {
            wchar_t c = Peek(ctx);
            if (c == L'"')
            {
                Advance(ctx); // skip closing '"'
                return true;
            }
            if (c == L'\\')
            {
                Advance(ctx);
                if (IsAtEnd(ctx))
                {
                    if (err)
                        *err = FormatError(ctx, L"Unexpected end of string after '\\'");
                    return false;
                }
                wchar_t esc = Advance(ctx);
                switch (esc)
                {
                case L'"':
                    out += L'"';
                    break;
                case L'\\':
                    out += L'\\';
                    break;
                case L'n':
                    out += L'\n';
                    break;
                case L't':
                    out += L'\t';
                    break;
                case L'r':
                    out += L'\r';
                    break;
                default:
                    out += L'\\';
                    out += esc;
                    break;
                }
            }
            else
            {
                out += Advance(ctx);
            }
        }

        if (err)
            *err = FormatError(ctx, L"Unterminated string");
        return false;
    }

    bool Parser::ParseList(ParseContext &ctx, Value::List &out, std::wstring *err)
    {
        if (Peek(ctx) != L'[')
        {
            if (err)
                *err = FormatError(ctx, L"Expected '['");
            return false;
        }
        Advance(ctx); // skip '['

        out.clear();
        SkipWhitespaceAndNewlines(ctx);

        if (!IsAtEnd(ctx) && Peek(ctx) == L']')
        {
            Advance(ctx);
            return true;
        }

        while (true)
        {
            SkipWhitespaceAndNewlines(ctx);
            if (IsAtEnd(ctx))
            {
                if (err)
                    *err = FormatError(ctx, L"Unterminated list, expected ']'");
                return false;
            }

            Value elem;
            if (!ParseValue(ctx, elem, err))
                return false;
            out.push_back(std::move(elem));

            SkipWhitespaceAndNewlines(ctx);
            if (IsAtEnd(ctx))
            {
                if (err)
                    *err = FormatError(ctx, L"Unterminated list, expected ']'");
                return false;
            }

            wchar_t c = Peek(ctx);
            if (c == L',')
            {
                Advance(ctx);
                continue;
            }
            if (c == L']')
            {
                Advance(ctx);
                return true;
            }

            if (err)
                *err = FormatError(ctx, L"Expected ',' or ']' in list");
            return false;
        }
    }

    bool Parser::ParseDict(ParseContext &ctx, Value::Dict &out, std::wstring *err)
    {
        if (Peek(ctx) != L'{')
        {
            if (err)
                *err = FormatError(ctx, L"Expected '{'");
            return false;
        }
        Advance(ctx); // skip '{'

        out.clear();
        SkipWhitespaceAndNewlines(ctx);

        if (!IsAtEnd(ctx) && Peek(ctx) == L'}')
        {
            Advance(ctx);
            return true;
        }

        while (true)
        {
            SkipWhitespaceAndNewlines(ctx);
            if (IsAtEnd(ctx))
            {
                if (err)
                    *err = FormatError(ctx, L"Unterminated dict, expected '}'");
                return false;
            }

            // 解析 key（必须是带引号的字符串）
            std::wstring key;
            if (Peek(ctx) != L'"')
            {
                if (err)
                    *err = FormatError(ctx, L"Expected '\"' for dict key");
                return false;
            }
            if (!ParseString(ctx, key, err))
                return false;

            SkipWhitespace(ctx);
            if (IsAtEnd(ctx) || Peek(ctx) != L':')
            {
                if (err)
                    *err = FormatError(ctx, L"Expected ':' after dict key");
                return false;
            }
            Advance(ctx); // skip ':'
            SkipWhitespace(ctx);

            Value value;
            if (!ParseValue(ctx, value, err))
                return false;
            out[key] = std::move(value);

            SkipWhitespaceAndNewlines(ctx);
            if (IsAtEnd(ctx))
            {
                if (err)
                    *err = FormatError(ctx, L"Unterminated dict, expected '}'");
                return false;
            }

            wchar_t c = Peek(ctx);
            if (c == L',')
            {
                Advance(ctx);
                continue;
            }
            if (c == L'}')
            {
                Advance(ctx);
                return true;
            }

            if (err)
                *err = FormatError(ctx, L"Expected ',' or '}' in dict");
            return false;
        }
    }

    bool Parser::ParseScalar(ParseContext &ctx, Value &out, std::wstring *err)
    {
        size_t start = ctx.pos;
        while (!IsAtEnd(ctx))
        {
            wchar_t c = Peek(ctx);
            if (c == L'\n' || c == L'\r' || c == L'#' || c == L',' || c == L']' || c == L'}')
                break;
            Advance(ctx);
        }

        std::wstring token = ctx.content->substr(start, ctx.pos - start);
        // trim trailing whitespace
        size_t end = token.find_last_not_of(L" \t");
        if (end != std::wstring::npos)
            token = token.substr(0, end + 1);

        if (token.empty())
        {
            out = Value();
            return true;
        }

        // bool
        {
            std::wstring lower = token;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t c)
                           { return std::towlower(c); });
            if (lower == L"true")
            {
                out = Value(true);
                return true;
            }
            if (lower == L"false")
            {
                out = Value(false);
                return true;
            }
        }

        // int or float
        {
            bool isNumber = true;
            bool hasDot = false;
            for (size_t i = 0; i < token.size(); ++i)
            {
                wchar_t c = token[i];
                if (i == 0 && (c == L'-' || c == L'+'))
                    continue;
                if (c == L'.')
                {
                    if (hasDot)
                    {
                        isNumber = false;
                        break;
                    }
                    hasDot = true;
                    continue;
                }
                if (!std::iswdigit(c))
                {
                    isNumber = false;
                    break;
                }
            }

            if (isNumber && !token.empty())
            {
                if (hasDot)
                {
                    try
                    {
                        double d = std::stod(token);
                        out = Value(d);
                        return true;
                    }
                    catch (...)
                    {
                    }
                }
                else
                {
                    try
                    {
                        int64_t i = std::stoll(token);
                        out = Value(i);
                        return true;
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        // 作为无引号字符串
        out = Value(token);
        return true;
    }

    // --- 序列化 ---

    std::wstring Parser::Serialize() const
    {
        std::wostringstream oss;
        for (const auto &key : keyOrder_)
        {
            auto it = entries_.find(key);
            if (it == entries_.end())
                continue;
            oss << key << L" = " << SerializeValue(it->second, 0) << L"\n";
        }
        // 输出不在 keyOrder_ 中的键（理论上不会发生）
        for (const auto &[k, v] : entries_)
        {
            if (std::find(keyOrder_.begin(), keyOrder_.end(), k) == keyOrder_.end())
            {
                oss << k << L" = " << SerializeValue(v, 0) << L"\n";
            }
        }
        return oss.str();
    }

    std::wstring Parser::SerializeValue(const Value &value, int indent)
    {
        switch (value.GetType())
        {
        case Value::Type::Null:
            return L"";
        case Value::Type::Bool:
            return value.AsBool() ? L"true" : L"false";
        case Value::Type::Int:
            return std::to_wstring(value.AsInt());
        case Value::Type::Float:
        {
            std::wostringstream oss;
            oss << value.AsFloat();
            return oss.str();
        }
        case Value::Type::String:
        {
            std::wstring s = L"\"";
            for (wchar_t c : value.AsString())
            {
                switch (c)
                {
                case L'"':
                    s += L"\\\"";
                    break;
                case L'\\':
                    s += L"\\\\";
                    break;
                case L'\n':
                    s += L"\\n";
                    break;
                case L'\t':
                    s += L"\\t";
                    break;
                case L'\r':
                    s += L"\\r";
                    break;
                default:
                    s += c;
                    break;
                }
            }
            s += L'"';
            return s;
        }
        case Value::Type::List:
        {
            const auto &list = value.AsList();
            if (list.empty())
                return L"[]";
            std::wstring pad(indent + 4, L' ');
            std::wstring closePad(indent, L' ');
            std::wstring s = L"[\n";
            for (size_t i = 0; i < list.size(); ++i)
            {
                s += pad + SerializeValue(list[i], indent + 4);
                if (i + 1 < list.size())
                    s += L",";
                s += L"\n";
            }
            s += closePad + L"]";
            return s;
        }
        case Value::Type::Dict:
        {
            const auto &dict = value.AsDict();
            if (dict.empty())
                return L"{}";
            std::wstring pad(indent + 4, L' ');
            std::wstring closePad(indent, L' ');
            std::wstring s = L"{\n";
            size_t i = 0;
            for (const auto &[k, v] : dict)
            {
                s += pad + L"\"" + k + L"\": " + SerializeValue(v, indent + 4);
                if (i + 1 < dict.size())
                    s += L",";
                s += L"\n";
                ++i;
            }
            s += closePad + L"}";
            return s;
        }
        }
        return L"";
    }
}
