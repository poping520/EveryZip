#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

// Advanced Configuration
namespace AdvConfig
{

    // ============================================================================
    // AdvConfig::Value — 配置文件中一个值的变体类型
    // ============================================================================

    class Value
    {
    public:
        using List = std::vector<Value>;
        using Dict = std::map<std::wstring, Value>;
        using Variant = std::variant<std::monostate, bool, int64_t, double, std::wstring, List, Dict>;

        enum class Type
        {
            Null,
            Bool,
            Int,
            Float,
            String,
            List,
            Dict
        };

        Value() : data_(std::monostate{}) {}
        explicit Value(bool v) : data_(v) {}
        explicit Value(int v) : data_(static_cast<int64_t>(v)) {}
        explicit Value(int64_t v) : data_(v) {}
        explicit Value(double v) : data_(v) {}
        explicit Value(const std::wstring &v) : data_(v) {}
        explicit Value(std::wstring &&v) : data_(std::move(v)) {}
        explicit Value(const wchar_t *v) : data_(std::wstring(v)) {}
        explicit Value(const List &v) : data_(v) {}
        explicit Value(List &&v) : data_(std::move(v)) {}
        explicit Value(const Dict &v) : data_(v) {}
        explicit Value(Dict &&v) : data_(std::move(v)) {}

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
        const std::wstring &AsString() const;
        const List &AsList() const;
        const Dict &AsDict() const;

        bool operator==(const Value &other) const { return data_ == other.data_; }
        bool operator!=(const Value &other) const { return !(*this == other); }

    private:
        Variant data_;
    };

    // ============================================================================
    // AdvConfig::Parser — 通用配置文件解析器
    // ============================================================================

    class Parser
    {
    public:
        // 从字符串解析配置
        bool Parse(const std::wstring &content, std::wstring *err = nullptr);

        // 从文件加载配置
        bool LoadFile(const std::wstring &filePath, std::wstring *err = nullptr);

        // 序列化为配置文件字符串
        std::wstring Serialize() const;

        // 保存到文件
        bool SaveFile(const std::wstring &filePath, std::wstring *err = nullptr) const;

        // 获取所有键值对
        const std::map<std::wstring, Value> &GetAll() const { return entries_; }

        // 按 key 获取值，不存在返回 Null
        const Value &Get(const std::wstring &key) const;

        // 设置键值对
        void Set(const std::wstring &key, const Value &value);

        // 是否包含某个 key
        bool Contains(const std::wstring &key) const;

        // 移除某个 key
        bool Remove(const std::wstring &key);

        // 清空所有配置
        void Clear();

    private:
        struct ParseContext
        {
            const std::wstring *content;
            size_t pos;
            size_t line;
            size_t col;
        };

        bool ParseEntries(ParseContext &ctx, std::wstring *err);
        bool ParseValue(ParseContext &ctx, Value &out, std::wstring *err);
        bool ParseString(ParseContext &ctx, std::wstring &out, std::wstring *err);
        bool ParseList(ParseContext &ctx, Value::List &out, std::wstring *err);
        bool ParseDict(ParseContext &ctx, Value::Dict &out, std::wstring *err);
        bool ParseScalar(ParseContext &ctx, Value &out, std::wstring *err);

        void SkipWhitespaceAndNewlines(ParseContext &ctx);
        void SkipWhitespace(ParseContext &ctx);
        void SkipToNextLine(ParseContext &ctx);
        wchar_t Peek(const ParseContext &ctx) const;
        wchar_t Advance(ParseContext &ctx);
        bool IsAtEnd(const ParseContext &ctx) const;

        std::wstring FormatError(const ParseContext &ctx, const std::wstring &msg) const;

        static std::wstring SerializeValue(const Value &value, int indent);

        std::map<std::wstring, Value> entries_;
        // 保留键的插入顺序
        std::vector<std::wstring> keyOrder_;
    };
}
