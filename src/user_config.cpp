#include "user_config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "logger.h"
#include "string_utils.h"

// ============================================================================
// ConfigValue 实现
// ============================================================================

ConfigValue::Type ConfigValue::GetType() const
{
    return static_cast<Type>(data_.index());
}

bool ConfigValue::AsBool(bool defaultVal) const
{
    if (auto* p = std::get_if<bool>(&data_)) return *p;
    return defaultVal;
}

int64_t ConfigValue::AsInt(int64_t defaultVal) const
{
    if (auto* p = std::get_if<int64_t>(&data_)) return *p;
    if (auto* p = std::get_if<double>(&data_)) return static_cast<int64_t>(*p);
    return defaultVal;
}

double ConfigValue::AsFloat(double defaultVal) const
{
    if (auto* p = std::get_if<double>(&data_)) return *p;
    if (auto* p = std::get_if<int64_t>(&data_)) return static_cast<double>(*p);
    return defaultVal;
}

static const std::string kEmptyString;
static const ConfigValue::List kEmptyList;
static const ConfigValue::Dict kEmptyDict;

const std::string& ConfigValue::AsString() const
{
    if (auto* p = std::get_if<std::string>(&data_)) return *p;
    return kEmptyString;
}

const ConfigValue::List& ConfigValue::AsList() const
{
    if (auto* p = std::get_if<List>(&data_)) return *p;
    return kEmptyList;
}

const ConfigValue::Dict& ConfigValue::AsDict() const
{
    if (auto* p = std::get_if<Dict>(&data_)) return *p;
    return kEmptyDict;
}

// ============================================================================
// ConfigParser 实现
// ============================================================================

static const ConfigValue kNullValue;

const ConfigValue& ConfigParser::Get(const std::string& key) const
{
    auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    return kNullValue;
}

void ConfigParser::Set(const std::string& key, const ConfigValue& value)
{
    if (entries_.find(key) == entries_.end()) {
        keyOrder_.push_back(key);
    }
    entries_[key] = value;
}

bool ConfigParser::Contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

bool ConfigParser::Remove(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    keyOrder_.erase(
        std::remove(keyOrder_.begin(), keyOrder_.end(), key),
        keyOrder_.end());
    return true;
}

void ConfigParser::Clear()
{
    entries_.clear();
    keyOrder_.clear();
}

// --- 解析辅助 ---

char ConfigParser::Peek(const ParseContext& ctx) const
{
    if (ctx.pos >= ctx.content->size()) return '\0';
    return (*ctx.content)[ctx.pos];
}

char ConfigParser::Advance(ParseContext& ctx)
{
    if (ctx.pos >= ctx.content->size()) return '\0';
    char c = (*ctx.content)[ctx.pos++];
    if (c == '\n') { ctx.line++; ctx.col = 1; }
    else { ctx.col++; }
    return c;
}

bool ConfigParser::IsAtEnd(const ParseContext& ctx) const
{
    return ctx.pos >= ctx.content->size();
}

void ConfigParser::SkipWhitespace(ParseContext& ctx)
{
    while (!IsAtEnd(ctx)) {
        char c = Peek(ctx);
        if (c == ' ' || c == '\t' || c == '\r') { Advance(ctx); }
        else break;
    }
}

void ConfigParser::SkipWhitespaceAndNewlines(ParseContext& ctx)
{
    while (!IsAtEnd(ctx)) {
        char c = Peek(ctx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { Advance(ctx); }
        else if (c == '#') { SkipToNextLine(ctx); }
        else break;
    }
}

void ConfigParser::SkipToNextLine(ParseContext& ctx)
{
    while (!IsAtEnd(ctx) && Peek(ctx) != '\n') { Advance(ctx); }
    if (!IsAtEnd(ctx)) Advance(ctx); // skip '\n'
}

std::string ConfigParser::FormatError(const ParseContext& ctx, const std::string& msg) const
{
    return "line " + std::to_string(ctx.line) + ", col " + std::to_string(ctx.col) + ": " + msg;
}

// --- 解析入口 ---

bool ConfigParser::Parse(const std::string& content, std::string* err)
{
    entries_.clear();
    keyOrder_.clear();

    // 跳过 UTF-8 BOM
    std::string cleaned = content;
    if (cleaned.size() >= 3 &&
        (unsigned char)cleaned[0] == 0xEF &&
        (unsigned char)cleaned[1] == 0xBB &&
        (unsigned char)cleaned[2] == 0xBF) {
        cleaned.erase(0, 3);
    }

    ParseContext ctx{ &cleaned, 0, 1, 1 };
    return ParseEntries(ctx, err);
}

bool ConfigParser::LoadFile(const std::string& filePath, std::string* err)
{
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        if (err) *err = "Cannot open file: " + filePath;
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return Parse(oss.str(), err);
}

bool ConfigParser::SaveFile(const std::string& filePath, std::string* err) const
{
    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (err) *err = "Cannot open file for writing: " + filePath;
        return false;
    }
    std::string content = Serialize();
    ofs.write(content.data(), content.size());
    ofs.close();
    if (ofs.fail()) {
        if (err) *err = "Failed to write file: " + filePath;
        return false;
    }
    return true;
}

bool ConfigParser::ParseEntries(ParseContext& ctx, std::string* err)
{
    while (true) {
        SkipWhitespaceAndNewlines(ctx);
        if (IsAtEnd(ctx)) break;

        char c = Peek(ctx);

        // 跳过注释行
        if (c == '#' || c == ';') {
            SkipToNextLine(ctx);
            continue;
        }

        // 解析 key
        size_t keyStart = ctx.pos;
        while (!IsAtEnd(ctx)) {
            char ch = Peek(ctx);
            if (ch == '=' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            Advance(ctx);
        }
        std::string key = ctx.content->substr(keyStart, ctx.pos - keyStart);
        if (key.empty()) {
            if (err) *err = FormatError(ctx, "Expected key");
            return false;
        }

        SkipWhitespace(ctx);

        if (IsAtEnd(ctx) || Peek(ctx) != '=') {
            if (err) *err = FormatError(ctx, "Expected '=' after key '" + key + "'");
            return false;
        }
        Advance(ctx); // skip '='

        SkipWhitespace(ctx);

        ConfigValue value;
        if (!ParseValue(ctx, value, err)) {
            return false;
        }

        if (entries_.find(key) == entries_.end()) {
            keyOrder_.push_back(key);
        }
        entries_[key] = std::move(value);

        // 跳过行尾剩余空白和注释
        SkipWhitespace(ctx);
        if (!IsAtEnd(ctx) && Peek(ctx) == '#') {
            SkipToNextLine(ctx);
        }
    }
    return true;
}

bool ConfigParser::ParseValue(ParseContext& ctx, ConfigValue& out, std::string* err)
{
    SkipWhitespace(ctx);
    if (IsAtEnd(ctx) || Peek(ctx) == '\n') {
        out = ConfigValue();
        return true;
    }

    char c = Peek(ctx);

    if (c == '"') {
        std::string s;
        if (!ParseString(ctx, s, err)) return false;
        out = ConfigValue(std::move(s));
        return true;
    }

    if (c == '[') {
        ConfigValue::List list;
        if (!ParseList(ctx, list, err)) return false;
        out = ConfigValue(std::move(list));
        return true;
    }

    if (c == '{') {
        ConfigValue::Dict dict;
        if (!ParseDict(ctx, dict, err)) return false;
        out = ConfigValue(std::move(dict));
        return true;
    }

    return ParseScalar(ctx, out, err);
}

bool ConfigParser::ParseString(ParseContext& ctx, std::string& out, std::string* err)
{
    if (Peek(ctx) != '"') {
        if (err) *err = FormatError(ctx, "Expected '\"'");
        return false;
    }
    Advance(ctx); // skip opening '"'

    out.clear();
    while (!IsAtEnd(ctx)) {
        char c = Peek(ctx);
        if (c == '"') {
            Advance(ctx); // skip closing '"'
            return true;
        }
        if (c == '\\') {
            Advance(ctx);
            if (IsAtEnd(ctx)) {
                if (err) *err = FormatError(ctx, "Unexpected end of string after '\\'");
                return false;
            }
            char esc = Advance(ctx);
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default: out += '\\'; out += esc; break;
            }
        } else {
            out += Advance(ctx);
        }
    }

    if (err) *err = FormatError(ctx, "Unterminated string");
    return false;
}

bool ConfigParser::ParseList(ParseContext& ctx, ConfigValue::List& out, std::string* err)
{
    if (Peek(ctx) != '[') {
        if (err) *err = FormatError(ctx, "Expected '['");
        return false;
    }
    Advance(ctx); // skip '['

    out.clear();
    SkipWhitespaceAndNewlines(ctx);

    if (!IsAtEnd(ctx) && Peek(ctx) == ']') {
        Advance(ctx);
        return true;
    }

    while (true) {
        SkipWhitespaceAndNewlines(ctx);
        if (IsAtEnd(ctx)) {
            if (err) *err = FormatError(ctx, "Unterminated list, expected ']'");
            return false;
        }

        ConfigValue elem;
        if (!ParseValue(ctx, elem, err)) return false;
        out.push_back(std::move(elem));

        SkipWhitespaceAndNewlines(ctx);
        if (IsAtEnd(ctx)) {
            if (err) *err = FormatError(ctx, "Unterminated list, expected ']'");
            return false;
        }

        char c = Peek(ctx);
        if (c == ',') {
            Advance(ctx);
            continue;
        }
        if (c == ']') {
            Advance(ctx);
            return true;
        }

        if (err) *err = FormatError(ctx, "Expected ',' or ']' in list");
        return false;
    }
}

bool ConfigParser::ParseDict(ParseContext& ctx, ConfigValue::Dict& out, std::string* err)
{
    if (Peek(ctx) != '{') {
        if (err) *err = FormatError(ctx, "Expected '{'");
        return false;
    }
    Advance(ctx); // skip '{'

    out.clear();
    SkipWhitespaceAndNewlines(ctx);

    if (!IsAtEnd(ctx) && Peek(ctx) == '}') {
        Advance(ctx);
        return true;
    }

    while (true) {
        SkipWhitespaceAndNewlines(ctx);
        if (IsAtEnd(ctx)) {
            if (err) *err = FormatError(ctx, "Unterminated dict, expected '}'");
            return false;
        }

        // 解析 key（必须是带引号的字符串）
        std::string key;
        if (Peek(ctx) != '"') {
            if (err) *err = FormatError(ctx, "Expected '\"' for dict key");
            return false;
        }
        if (!ParseString(ctx, key, err)) return false;

        SkipWhitespace(ctx);
        if (IsAtEnd(ctx) || Peek(ctx) != ':') {
            if (err) *err = FormatError(ctx, "Expected ':' after dict key");
            return false;
        }
        Advance(ctx); // skip ':'
        SkipWhitespace(ctx);

        ConfigValue value;
        if (!ParseValue(ctx, value, err)) return false;
        out[key] = std::move(value);

        SkipWhitespaceAndNewlines(ctx);
        if (IsAtEnd(ctx)) {
            if (err) *err = FormatError(ctx, "Unterminated dict, expected '}'");
            return false;
        }

        char c = Peek(ctx);
        if (c == ',') {
            Advance(ctx);
            continue;
        }
        if (c == '}') {
            Advance(ctx);
            return true;
        }

        if (err) *err = FormatError(ctx, "Expected ',' or '}' in dict");
        return false;
    }
}

bool ConfigParser::ParseScalar(ParseContext& ctx, ConfigValue& out, std::string* err)
{
    size_t start = ctx.pos;
    while (!IsAtEnd(ctx)) {
        char c = Peek(ctx);
        if (c == '\n' || c == '\r' || c == '#' || c == ',' || c == ']' || c == '}') break;
        Advance(ctx);
    }

    std::string token = ctx.content->substr(start, ctx.pos - start);
    // trim trailing whitespace
    size_t end = token.find_last_not_of(" \t");
    if (end != std::string::npos) token = token.substr(0, end + 1);

    if (token.empty()) {
        out = ConfigValue();
        return true;
    }

    // bool
    {
        std::string lower = token;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "true") { out = ConfigValue(true); return true; }
        if (lower == "false") { out = ConfigValue(false); return true; }
    }

    // int or float
    {
        bool isNumber = true;
        bool hasDot = false;
        for (size_t i = 0; i < token.size(); ++i) {
            char c = token[i];
            if (i == 0 && (c == '-' || c == '+')) continue;
            if (c == '.') {
                if (hasDot) { isNumber = false; break; }
                hasDot = true;
                continue;
            }
            if (!std::isdigit(static_cast<unsigned char>(c))) { isNumber = false; break; }
        }

        if (isNumber && !token.empty()) {
            if (hasDot) {
                try {
                    double d = std::stod(token);
                    out = ConfigValue(d);
                    return true;
                } catch (...) {}
            } else {
                try {
                    int64_t i = std::stoll(token);
                    out = ConfigValue(i);
                    return true;
                } catch (...) {}
            }
        }
    }

    // 作为无引号字符串
    out = ConfigValue(token);
    return true;
}

// --- 序列化 ---

std::string ConfigParser::Serialize() const
{
    std::ostringstream oss;
    for (const auto& key : keyOrder_) {
        auto it = entries_.find(key);
        if (it == entries_.end()) continue;
        oss << key << " = " << SerializeValue(it->second, 0) << "\n";
    }
    // 输出不在 keyOrder_ 中的键（理论上不会发生）
    for (const auto& [k, v] : entries_) {
        if (std::find(keyOrder_.begin(), keyOrder_.end(), k) == keyOrder_.end()) {
            oss << k << " = " << SerializeValue(v, 0) << "\n";
        }
    }
    return oss.str();
}

std::string ConfigParser::SerializeValue(const ConfigValue& value, int indent)
{
    switch (value.GetType()) {
        case ConfigValue::Type::Null:
            return "";
        case ConfigValue::Type::Bool:
            return value.AsBool() ? "true" : "false";
        case ConfigValue::Type::Int:
            return std::to_string(value.AsInt());
        case ConfigValue::Type::Float: {
            std::ostringstream oss;
            oss << value.AsFloat();
            return oss.str();
        }
        case ConfigValue::Type::String: {
            std::string s = "\"";
            for (char c : value.AsString()) {
                switch (c) {
                    case '"': s += "\\\""; break;
                    case '\\': s += "\\\\"; break;
                    case '\n': s += "\\n"; break;
                    case '\t': s += "\\t"; break;
                    case '\r': s += "\\r"; break;
                    default: s += c; break;
                }
            }
            s += '"';
            return s;
        }
        case ConfigValue::Type::List: {
            const auto& list = value.AsList();
            if (list.empty()) return "[]";
            std::string pad(indent + 4, ' ');
            std::string closePad(indent, ' ');
            std::string s = "[\n";
            for (size_t i = 0; i < list.size(); ++i) {
                s += pad + SerializeValue(list[i], indent + 4);
                if (i + 1 < list.size()) s += ",";
                s += "\n";
            }
            s += closePad + "]";
            return s;
        }
        case ConfigValue::Type::Dict: {
            const auto& dict = value.AsDict();
            if (dict.empty()) return "{}";
            std::string pad(indent + 4, ' ');
            std::string closePad(indent, ' ');
            std::string s = "{\n";
            size_t i = 0;
            for (const auto& [k, v] : dict) {
                s += pad + "\"" + k + "\": " + SerializeValue(v, indent + 4);
                if (i + 1 < dict.size()) s += ",";
                s += "\n";
                ++i;
            }
            s += closePad + "}";
            return s;
        }
    }
    return "";
}

// ============================================================================
// UserConfig 实现（基于 ConfigParser）
// ============================================================================

static const char* kKeyArchiveExtensions = "archive_extensions";

// 默认归档扩展名
static const std::vector<std::wstring> kDefaultArchiveExtensions = { L".zip" };

UserConfig::UserConfig()
    : archiveExtensions_(kDefaultArchiveExtensions)
{
}

UserConfig::~UserConfig() = default;

const std::vector<std::wstring>& UserConfig::GetArchiveExtensions() const
{
    return archiveExtensions_;
}

void UserConfig::SetArchiveExtensions(const std::vector<std::wstring>& exts)
{
    archiveExtensions_ = exts;
}

void UserConfig::SyncFromParser()
{
    const ConfigValue& val = parser_.Get(kKeyArchiveExtensions);
    if (val.IsList()) {
        archiveExtensions_.clear();
        for (const auto& item : val.AsList()) {
            if (!item.IsString()) continue;
            std::wstring wext = Utf8ToWString(item.AsString().c_str());
            if (!wext.empty() && wext[0] != L'.') {
                wext = L"." + wext;
            }
            std::transform(wext.begin(), wext.end(), wext.begin(), std::towlower);
            archiveExtensions_.push_back(std::move(wext));
        }
        if (archiveExtensions_.empty()) {
            archiveExtensions_ = kDefaultArchiveExtensions;
        }
    } else if (val.IsString()) {
        // 兼容旧格式：逗号分隔
        archiveExtensions_.clear();
        std::istringstream extStream(val.AsString());
        std::string ext;
        while (std::getline(extStream, ext, ',')) {
            size_t s = ext.find_first_not_of(" \t");
            size_t e = ext.find_last_not_of(" \t");
            if (s == std::string::npos) continue;
            ext = ext.substr(s, e - s + 1);
            if (ext.empty()) continue;
            std::wstring wext = Utf8ToWString(ext.c_str());
            if (!wext.empty() && wext[0] != L'.') {
                wext = L"." + wext;
            }
            std::transform(wext.begin(), wext.end(), wext.begin(), std::towlower);
            archiveExtensions_.push_back(std::move(wext));
        }
        if (archiveExtensions_.empty()) {
            archiveExtensions_ = kDefaultArchiveExtensions;
        }
    }
}

void UserConfig::SyncToParser()
{
    ConfigValue::List list;
    for (const auto& ext : archiveExtensions_) {
        list.push_back(ConfigValue(WideToUtf8(ext)));
    }
    parser_.Set(kKeyArchiveExtensions, ConfigValue(std::move(list)));
}

bool UserConfig::Load(const std::wstring& configPath, std::wstring* err)
{
    if (err) err->clear();
    configPath_ = configPath;

    std::string pathUtf8 = WideToUtf8(configPath);
    std::ifstream ifs(configPath, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_INFO(L"Config file not found, creating default: %s", configPath.c_str());
        archiveExtensions_ = kDefaultArchiveExtensions;
        SyncToParser();
        return Save(err);
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    ifs.close();

    std::string parseErr;
    if (!parser_.Parse(oss.str(), &parseErr)) {
        if (err) *err = Utf8ToWString(parseErr.c_str());
        return false;
    }

    SyncFromParser();

    LOG_INFO(L"Config loaded: %zu archive extensions", archiveExtensions_.size());
    return true;
}

bool UserConfig::Save(std::wstring* err) const
{
    if (err) err->clear();
    if (configPath_.empty()) {
        if (err) *err = L"Config path is empty";
        return false;
    }

    std::string content = parser_.Serialize();

    std::ofstream ofs(configPath_, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (err) *err = L"Cannot open config file for writing";
        return false;
    }

    ofs.write(content.data(), content.size());
    ofs.close();

    if (ofs.fail()) {
        if (err) *err = L"Failed to write config file";
        return false;
    }

    return true;
}
