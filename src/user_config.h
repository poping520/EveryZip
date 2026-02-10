#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// ConfigValue — 配置文件中一个值的变体类型
// ============================================================================

class ConfigValue {
public:
    using List = std::vector<ConfigValue>;
    using Dict = std::map<std::string, ConfigValue>;
    using Variant = std::variant<std::monostate, bool, int64_t, double, std::string, List, Dict>;

    enum class Type { Null, Bool, Int, Float, String, List, Dict };

    ConfigValue() : data_(std::monostate{}) {}
    explicit ConfigValue(bool v) : data_(v) {}
    explicit ConfigValue(int v) : data_(static_cast<int64_t>(v)) {}
    explicit ConfigValue(int64_t v) : data_(v) {}
    explicit ConfigValue(double v) : data_(v) {}
    explicit ConfigValue(const std::string& v) : data_(v) {}
    explicit ConfigValue(std::string&& v) : data_(std::move(v)) {}
    explicit ConfigValue(const char* v) : data_(std::string(v)) {}
    explicit ConfigValue(const List& v) : data_(v) {}
    explicit ConfigValue(List&& v) : data_(std::move(v)) {}
    explicit ConfigValue(const Dict& v) : data_(v) {}
    explicit ConfigValue(Dict&& v) : data_(std::move(v)) {}

    Type GetType() const;
    bool IsNull() const { return GetType() == Type::Null; }
    bool IsBool() const { return GetType() == Type::Bool; }
    bool IsInt() const { return GetType() == Type::Int; }
    bool IsFloat() const { return GetType() == Type::Float; }
    bool IsString() const { return GetType() == Type::String; }
    bool IsList() const { return GetType() == Type::List; }
    bool IsDict() const { return GetType() == Type::Dict; }

    bool AsBool(bool defaultVal = false) const;
    int64_t AsInt(int64_t defaultVal = 0) const;
    double AsFloat(double defaultVal = 0.0) const;
    const std::string& AsString() const;
    const List& AsList() const;
    const Dict& AsDict() const;

    bool operator==(const ConfigValue& other) const { return data_ == other.data_; }
    bool operator!=(const ConfigValue& other) const { return !(*this == other); }

private:
    Variant data_;
};

// ============================================================================
// ConfigParser — 通用配置文件解析器
// ============================================================================

class ConfigParser {
public:
    // 从字符串解析配置
    bool Parse(const std::string& content, std::string* err = nullptr);

    // 从文件加载配置
    bool LoadFile(const std::string& filePath, std::string* err = nullptr);

    // 序列化为配置文件字符串
    std::string Serialize() const;

    // 保存到文件
    bool SaveFile(const std::string& filePath, std::string* err = nullptr) const;

    // 获取所有键值对
    const std::map<std::string, ConfigValue>& GetAll() const { return entries_; }

    // 按 key 获取值，不存在返回 Null
    const ConfigValue& Get(const std::string& key) const;

    // 设置键值对
    void Set(const std::string& key, const ConfigValue& value);

    // 是否包含某个 key
    bool Contains(const std::string& key) const;

    // 移除某个 key
    bool Remove(const std::string& key);

    // 清空所有配置
    void Clear();

private:
    struct ParseContext {
        const std::string* content;
        size_t pos;
        size_t line;
        size_t col;
    };

    bool ParseEntries(ParseContext& ctx, std::string* err);
    bool ParseValue(ParseContext& ctx, ConfigValue& out, std::string* err);
    bool ParseString(ParseContext& ctx, std::string& out, std::string* err);
    bool ParseList(ParseContext& ctx, ConfigValue::List& out, std::string* err);
    bool ParseDict(ParseContext& ctx, ConfigValue::Dict& out, std::string* err);
    bool ParseScalar(ParseContext& ctx, ConfigValue& out, std::string* err);

    void SkipWhitespaceAndNewlines(ParseContext& ctx);
    void SkipWhitespace(ParseContext& ctx);
    void SkipToNextLine(ParseContext& ctx);
    char Peek(const ParseContext& ctx) const;
    char Advance(ParseContext& ctx);
    bool IsAtEnd(const ParseContext& ctx) const;

    std::string FormatError(const ParseContext& ctx, const std::string& msg) const;

    static std::string SerializeValue(const ConfigValue& value, int indent);

    std::map<std::string, ConfigValue> entries_;
    // 保留键的插入顺序
    std::vector<std::string> keyOrder_;
};

// ============================================================================
// UserConfig — 应用层配置（基于 ConfigParser）
// ============================================================================

class UserConfig {
public:
    UserConfig();
    ~UserConfig();

    UserConfig(const UserConfig&) = delete;
    UserConfig& operator=(const UserConfig&) = delete;

    // 加载配置文件，如果文件不存在则创建默认配置
    bool Load(const std::wstring& configPath, std::wstring* err = nullptr);

    // 保存当前配置到文件
    bool Save(std::wstring* err = nullptr) const;

    // 归档文件扩展名（例如 {L".zip", L".rar"}）
    const std::vector<std::wstring>& GetArchiveExtensions() const;
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    // 配置文件路径
    const std::wstring& GetConfigPath() const { return configPath_; }

private:
    void SyncFromParser();
    void SyncToParser();

    std::wstring configPath_;
    std::vector<std::wstring> archiveExtensions_;
    ConfigParser parser_;
};
