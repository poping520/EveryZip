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

        /** 构造值。 */
        Value() : data_(std::monostate{}) {}
        explicit Value(bool v) : data_(v) {}
        explicit Value(int v) : data_(static_cast<int64_t>(v)) {}
        explicit Value(int64_t v) : data_(v) {}
        explicit Value(double v) : data_(v) {}
        explicit Value(const std::wstring &v) : data_(v) {}
        // 构造字符串值并移动内容。
        explicit Value(std::wstring &&v) : data_(std::move(v)) {}
        explicit Value(const wchar_t *v) : data_(std::wstring(v)) {}
        explicit Value(const List &v) : data_(v) {}
        explicit Value(List &&v) : data_(std::move(v)) {}
        explicit Value(const Dict &v) : data_(v) {}
        explicit Value(Dict &&v) : data_(std::move(v)) {}

        /**
         * 获取当前值类型。
         * @return 当前存储的数据类型。
         */
        Type GetType() const;
        bool IsNull() const { return GetType() == Type::Null; }
        bool IsBool() const { return GetType() == Type::Bool; }
        bool IsInt() const { return GetType() == Type::Int; }
        bool IsFloat() const { return GetType() == Type::Float; }
        bool IsString() const { return GetType() == Type::String; }
        bool IsList() const { return GetType() == Type::List; }
        bool IsDict() const { return GetType() == Type::Dict; }

        /**
         * 将当前值读取为布尔值。
         * @param defaultVal 类型不匹配时返回的默认值。
         * @return 当前值对应的布尔值或默认值。
         */
        bool AsBool(bool defaultVal = false) const;
        /**
         * 将当前值读取为整数值。
         * @param defaultVal 类型不匹配时返回的默认值。
         * @return 当前值对应的整数值或默认值。
         */
        int64_t AsInt(int64_t defaultVal = 0) const;
        /**
         * 将当前值读取为浮点值。
         * @param defaultVal 类型不匹配时返回的默认值。
         * @return 当前值对应的浮点值或默认值。
         */
        double AsFloat(double defaultVal = 0.0) const;
        /**
         * 将当前值读取为字符串引用。
         * @return 当前字符串值；类型不匹配时返回空字符串引用。
         */
        const std::wstring &AsString() const;
        /**
         * 将当前值读取为列表引用。
         * @return 当前列表值；类型不匹配时返回空列表引用。
         */
        const List &AsList() const;
        /**
         * 将当前值读取为字典引用。
         * @return 当前字典值；类型不匹配时返回空字典引用。
         */
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
        /**
         * 从字符串解析配置。
         * @param content 配置文本内容。
         * @param err 可选，用于输出错误信息。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool Parse(const std::wstring &content, std::wstring *err = nullptr);

        /**
         * 从文件加载配置。
         * @param filePath 配置文件路径。
         * @param err 可选，用于输出错误信息。
         * @return 加载成功返回 true，否则返回 false。
         */
        bool LoadFile(const std::wstring &filePath, std::wstring *err = nullptr);

        /**
         * 将当前配置序列化为配置文件字符串。
         * @return 序列化后的配置文本。
         */
        std::wstring Serialize() const;

        /**
         * 将当前配置保存到文件。
         * @param filePath 目标文件路径。
         * @param err 可选，用于输出错误信息。
         * @return 保存成功返回 true，否则返回 false。
         */
        bool SaveFile(const std::wstring &filePath, std::wstring *err = nullptr) const;

        /**
         * 获取全部键值对。
         * @return 内部键值对映射的只读引用。
         */
        const std::map<std::wstring, Value> &GetAll() const { return entries_; }

        /**
         * 按 key 获取值，不存在时返回 Null 值引用。
         * @param key 配置键名。
         * @return 对应的配置值引用。
         */
        const Value &Get(const std::wstring &key) const;

        /**
         * 设置键值对。
         * @param key 配置键名。
         * @param value 配置值。
         */
        void Set(const std::wstring &key, const Value &value);

        /**
         * 判断是否包含某个 key。
         * @param key 配置键名。
         * @return 包含返回 true，否则返回 false。
         */
        bool Contains(const std::wstring &key) const;

        /**
         * 移除某个 key。
         * @param key 配置键名。
         * @return 移除成功返回 true，否则返回 false。
         */
        bool Remove(const std::wstring &key);

        /** 清空所有配置。 */
        void Clear();

    private:
        struct ParseContext
        {
            const std::wstring *content;
            size_t pos;
            size_t line;
            size_t col;
        };

        /**
         * 解析全部顶层键值对。
         * @param ctx 解析上下文。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseEntries(ParseContext &ctx, std::wstring *err);
        /**
         * 解析一个配置值。
         * @param ctx 解析上下文。
         * @param out 输出解析得到的值。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseValue(ParseContext &ctx, Value &out, std::wstring *err);
        /**
         * 解析带引号字符串。
         * @param ctx 解析上下文。
         * @param out 输出字符串内容。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseString(ParseContext &ctx, std::wstring &out, std::wstring *err);
        /**
         * 解析列表值。
         * @param ctx 解析上下文。
         * @param out 输出列表内容。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseList(ParseContext &ctx, Value::List &out, std::wstring *err);
        /**
         * 解析字典值。
         * @param ctx 解析上下文。
         * @param out 输出字典内容。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseDict(ParseContext &ctx, Value::Dict &out, std::wstring *err);
        /**
         * 解析标量值。
         * @param ctx 解析上下文。
         * @param out 输出标量值。
         * @param err 可选错误输出。
         * @return 解析成功返回 true，否则返回 false。
         */
        bool ParseScalar(ParseContext &ctx, Value &out, std::wstring *err);

        /**
         * 跳过空白字符与换行。
         * @param ctx 解析上下文。
         */
        void SkipWhitespaceAndNewlines(ParseContext &ctx);
        /**
         * 跳过空白字符但保留换行。
         * @param ctx 解析上下文。
         */
        void SkipWhitespace(ParseContext &ctx);
        /**
         * 跳过当前行剩余内容。
         * @param ctx 解析上下文。
         */
        void SkipToNextLine(ParseContext &ctx);
        /**
         * 查看当前位置字符但不推进游标。
         * @param ctx 解析上下文。
         * @return 当前字符；到达末尾时返回 '\0'。
         */
        wchar_t Peek(const ParseContext &ctx) const;
        /**
         * 读取当前位置字符并推进游标。
         * @param ctx 解析上下文。
         * @return 当前位置字符；到达末尾时返回 '\0'。
         */
        wchar_t Advance(ParseContext &ctx);
        /**
         * 判断解析是否已到达文本末尾。
         * @param ctx 解析上下文。
         * @return 已结束返回 true，否则返回 false。
         */
        bool IsAtEnd(const ParseContext &ctx) const;

        /**
         * 格式化解析错误信息。
         * @param ctx 解析上下文。
         * @param msg 原始错误消息。
         * @return 带行列号的错误文本。
         */
        std::wstring FormatError(const ParseContext &ctx, const std::wstring &msg) const;

        /**
         * 将单个值序列化为配置文本片段。
         * @param value 待序列化的值。
         * @param indent 当前缩进空格数。
         * @return 序列化后的文本片段。
         */
        static std::wstring SerializeValue(const Value &value, int indent);

        std::map<std::wstring, Value> entries_;
        // 保留键的插入顺序
        std::vector<std::wstring> keyOrder_;
    };
}
