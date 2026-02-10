#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/user_config.h"

#define TEST(name) static void name()
#define RUN_TEST(name) do { printf("  [RUN ] %s\n", #name); name(); printf("  [PASS] %s\n", #name); } while(0)
#define ASSERT_TRUE(expr)  do { if (!(expr)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); assert(false); } } while(0)
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)   ASSERT_TRUE((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT_TRUE(std::fabs((a) - (b)) < (eps))

// ============================================================================
// ConfigValue 测试
// ============================================================================

TEST(TestConfigValueNull)
{
    ConfigValue v;
    ASSERT_TRUE(v.IsNull());
    ASSERT_EQ(v.GetType(), ConfigValue::Type::Null);
}

TEST(TestConfigValueBool)
{
    ConfigValue t(true);
    ConfigValue f(false);
    ASSERT_TRUE(t.IsBool());
    ASSERT_TRUE(f.IsBool());
    ASSERT_EQ(t.AsBool(), true);
    ASSERT_EQ(f.AsBool(), false);
}

TEST(TestConfigValueInt)
{
    ConfigValue v(int64_t(42));
    ASSERT_TRUE(v.IsInt());
    ASSERT_EQ(v.AsInt(), 42);

    ConfigValue neg(int64_t(-100));
    ASSERT_EQ(neg.AsInt(), -100);

    // int -> float 转换
    ASSERT_NEAR(v.AsFloat(), 42.0, 0.001);
}

TEST(TestConfigValueFloat)
{
    ConfigValue v(3.14);
    ASSERT_TRUE(v.IsFloat());
    ASSERT_NEAR(v.AsFloat(), 3.14, 0.001);

    // float -> int 转换
    ASSERT_EQ(v.AsInt(), 3);
}

TEST(TestConfigValueString)
{
    ConfigValue v(std::string("hello"));
    ASSERT_TRUE(v.IsString());
    ASSERT_EQ(v.AsString(), "hello");

    ConfigValue v2("world");
    ASSERT_TRUE(v2.IsString());
    ASSERT_EQ(v2.AsString(), "world");
}

TEST(TestConfigValueList)
{
    ConfigValue::List list;
    list.push_back(ConfigValue(1));
    list.push_back(ConfigValue("two"));
    list.push_back(ConfigValue(true));

    ConfigValue v(list);
    ASSERT_TRUE(v.IsList());
    ASSERT_EQ(v.AsList().size(), size_t(3));
    ASSERT_EQ(v.AsList()[0].AsInt(), 1);
    ASSERT_EQ(v.AsList()[1].AsString(), "two");
    ASSERT_EQ(v.AsList()[2].AsBool(), true);
}

TEST(TestConfigValueDict)
{
    ConfigValue::Dict dict;
    dict["name"] = ConfigValue("test");
    dict["count"] = ConfigValue(int64_t(5));

    ConfigValue v(dict);
    ASSERT_TRUE(v.IsDict());
    ASSERT_EQ(v.AsDict().size(), size_t(2));
    ASSERT_EQ(v.AsDict().at("name").AsString(), "test");
    ASSERT_EQ(v.AsDict().at("count").AsInt(), 5);
}

TEST(TestConfigValueDefaults)
{
    ConfigValue v; // Null
    ASSERT_EQ(v.AsBool(true), true);
    ASSERT_EQ(v.AsInt(99), 99);
    ASSERT_NEAR(v.AsFloat(1.5), 1.5, 0.001);
    ASSERT_EQ(v.AsString(), "");
    ASSERT_TRUE(v.AsList().empty());
    ASSERT_TRUE(v.AsDict().empty());
}

TEST(TestConfigValueEquality)
{
    ASSERT_TRUE(ConfigValue(true) == ConfigValue(true));
    ASSERT_TRUE(ConfigValue(true) != ConfigValue(false));
    ASSERT_TRUE(ConfigValue(int64_t(1)) == ConfigValue(int64_t(1)));
    ASSERT_TRUE(ConfigValue("abc") == ConfigValue("abc"));
    ASSERT_TRUE(ConfigValue("abc") != ConfigValue("def"));
}

// ============================================================================
// ConfigParser 基础测试
// ============================================================================

TEST(TestParseEmpty)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("", &err));
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

TEST(TestParseComments)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("# comment\n; another comment\n", &err));
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

TEST(TestParseBool)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("a = true\nb=false\n", &err));
    ASSERT_TRUE(parser.Get("a").IsBool());
    ASSERT_EQ(parser.Get("a").AsBool(), true);
    ASSERT_TRUE(parser.Get("b").IsBool());
    ASSERT_EQ(parser.Get("b").AsBool(), false);
}

TEST(TestParseInt)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("x = 123\ny = -456\n", &err));
    ASSERT_TRUE(parser.Get("x").IsInt());
    ASSERT_EQ(parser.Get("x").AsInt(), 123);
    ASSERT_TRUE(parser.Get("y").IsInt());
    ASSERT_EQ(parser.Get("y").AsInt(), -456);
}

TEST(TestParseFloat)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("pi = 3.14159\n", &err));
    ASSERT_TRUE(parser.Get("pi").IsFloat());
    ASSERT_NEAR(parser.Get("pi").AsFloat(), 3.14159, 0.00001);
}

TEST(TestParseString)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("name = \"hello world\"\n", &err));
    ASSERT_TRUE(parser.Get("name").IsString());
    ASSERT_EQ(parser.Get("name").AsString(), "hello world");
}

TEST(TestParseStringEscape)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("s = \"line1\\nline2\\ttab\"\n", &err));
    ASSERT_EQ(parser.Get("s").AsString(), "line1\nline2\ttab");
}

TEST(TestParseList)
{
    ConfigParser parser;
    std::string err;
    std::string input =
        "items = [\n"
        "    \"a\",\n"
        "    \"b\",\n"
        "    \"c\"\n"
        "]\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get("items").IsList());
    const auto& list = parser.Get("items").AsList();
    ASSERT_EQ(list.size(), size_t(3));
    ASSERT_EQ(list[0].AsString(), "a");
    ASSERT_EQ(list[1].AsString(), "b");
    ASSERT_EQ(list[2].AsString(), "c");
}

TEST(TestParseEmptyList)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("items = []\n", &err));
    ASSERT_TRUE(parser.Get("items").IsList());
    ASSERT_EQ(parser.Get("items").AsList().size(), size_t(0));
}

TEST(TestParseDict)
{
    ConfigParser parser;
    std::string err;
    std::string input =
        "data = {\n"
        "    \"key1\": \"value1\",\n"
        "    \"key2\": \"value2\"\n"
        "}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get("data").IsDict());
    const auto& dict = parser.Get("data").AsDict();
    ASSERT_EQ(dict.size(), size_t(2));
    ASSERT_EQ(dict.at("key1").AsString(), "value1");
    ASSERT_EQ(dict.at("key2").AsString(), "value2");
}

TEST(TestParseEmptyDict)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("data = {}\n", &err));
    ASSERT_TRUE(parser.Get("data").IsDict());
    ASSERT_EQ(parser.Get("data").AsDict().size(), size_t(0));
}

TEST(TestParseMixedTypes)
{
    ConfigParser parser;
    std::string err;
    std::string input =
        "flag = true\n"
        "count = 42\n"
        "ratio = 1.5\n"
        "name = \"test\"\n"
        "tags = [\"a\", \"b\"]\n"
        "meta = {\"x\": \"y\"}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get("flag").IsBool());
    ASSERT_TRUE(parser.Get("count").IsInt());
    ASSERT_TRUE(parser.Get("ratio").IsFloat());
    ASSERT_TRUE(parser.Get("name").IsString());
    ASSERT_TRUE(parser.Get("tags").IsList());
    ASSERT_TRUE(parser.Get("meta").IsDict());
}

// ============================================================================
// ConfigParser 操作测试
// ============================================================================

TEST(TestGetNonExistent)
{
    ConfigParser parser;
    ASSERT_TRUE(parser.Get("missing").IsNull());
}

TEST(TestSetAndGet)
{
    ConfigParser parser;
    parser.Set("key", ConfigValue(int64_t(100)));
    ASSERT_TRUE(parser.Contains("key"));
    ASSERT_EQ(parser.Get("key").AsInt(), 100);
}

TEST(TestRemove)
{
    ConfigParser parser;
    parser.Set("a", ConfigValue(true));
    ASSERT_TRUE(parser.Contains("a"));
    ASSERT_TRUE(parser.Remove("a"));
    ASSERT_FALSE(parser.Contains("a"));
    ASSERT_FALSE(parser.Remove("a"));
}

TEST(TestClear)
{
    ConfigParser parser;
    parser.Set("a", ConfigValue(1));
    parser.Set("b", ConfigValue(2));
    parser.Clear();
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

// ============================================================================
// ConfigParser 序列化测试
// ============================================================================

TEST(TestSerializeRoundTrip)
{
    ConfigParser parser;
    parser.Set("flag", ConfigValue(true));
    parser.Set("count", ConfigValue(int64_t(42)));
    parser.Set("ratio", ConfigValue(1.5));
    parser.Set("name", ConfigValue("hello"));

    ConfigValue::List list;
    list.push_back(ConfigValue("a"));
    list.push_back(ConfigValue("b"));
    parser.Set("tags", ConfigValue(std::move(list)));

    ConfigValue::Dict dict;
    dict["x"] = ConfigValue("y");
    parser.Set("meta", ConfigValue(std::move(dict)));

    std::string serialized = parser.Serialize();

    ConfigParser parser2;
    std::string err;
    ASSERT_TRUE(parser2.Parse(serialized, &err));

    ASSERT_EQ(parser2.Get("flag").AsBool(), true);
    ASSERT_EQ(parser2.Get("count").AsInt(), 42);
    ASSERT_NEAR(parser2.Get("ratio").AsFloat(), 1.5, 0.001);
    ASSERT_EQ(parser2.Get("name").AsString(), "hello");
    ASSERT_EQ(parser2.Get("tags").AsList().size(), size_t(2));
    ASSERT_EQ(parser2.Get("meta").AsDict().at("x").AsString(), "y");
}

// ============================================================================
// 解析 test_config.cfg 格式测试
// ============================================================================

TEST(TestParseConfigFile)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.LoadFile("..\\..\\..\\test\\test_config.cfg", &err));

    // 布尔值
    ASSERT_TRUE(parser.Get("bool").IsBool());
    ASSERT_EQ(parser.Get("bool").AsBool(), true);
    ASSERT_TRUE(parser.Get("bool2").IsBool());
    ASSERT_EQ(parser.Get("bool2").AsBool(), false);

    // 整数
    ASSERT_TRUE(parser.Get("int").IsInt());
    ASSERT_EQ(parser.Get("int").AsInt(), 123);
    ASSERT_TRUE(parser.Get("int2").IsInt());
    ASSERT_EQ(parser.Get("int2").AsInt(), -123);

    // 浮点数
    ASSERT_TRUE(parser.Get("float").IsFloat());
    ASSERT_NEAR(parser.Get("float").AsFloat(), 123.456, 0.001);

    // 字符串
    ASSERT_TRUE(parser.Get("string").IsString());
    ASSERT_EQ(parser.Get("string").AsString(), "This is a string value");

    // 列表
    ASSERT_TRUE(parser.Get("list").IsList());
    const auto& list = parser.Get("list").AsList();
    ASSERT_EQ(list.size(), size_t(3));
    ASSERT_EQ(list[0].AsString(), "value1");
    ASSERT_EQ(list[1].AsString(), "value2]");
    // ASSERT_EQ(list[2].AsString(), "[value3");

    // 字典
    ASSERT_TRUE(parser.Get("dict").IsDict());
    const auto& dict = parser.Get("dict").AsDict();
    ASSERT_EQ(dict.size(), size_t(3));
    ASSERT_EQ(dict.at("key1").AsString(), "{value1");
    ASSERT_EQ(dict.at("key2").AsString(), "[value2}");
    ASSERT_EQ(dict.at("key3]").AsString(), "你好");
}

// ============================================================================
// 错误处理测试
// ============================================================================

TEST(TestParseErrorUnterminatedString)
{
    ConfigParser parser;
    std::string err;
    ASSERT_FALSE(parser.Parse("s = \"unterminated\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorUnterminatedList)
{
    ConfigParser parser;
    std::string err;
    ASSERT_FALSE(parser.Parse("l = [\"a\", \"b\"\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorUnterminatedDict)
{
    ConfigParser parser;
    std::string err;
    ASSERT_FALSE(parser.Parse("d = {\"a\": \"b\"\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorMissingEquals)
{
    ConfigParser parser;
    std::string err;
    ASSERT_FALSE(parser.Parse("key_without_value\n", &err));
    ASSERT_FALSE(err.empty());
}

// ============================================================================
// UTF-8 BOM 测试
// ============================================================================

TEST(TestParseUtf8Bom)
{
    ConfigParser parser;
    std::string err;
    std::string content = "\xEF\xBB\xBF" "x = 42\n";
    ASSERT_TRUE(parser.Parse(content, &err));
    ASSERT_EQ(parser.Get("x").AsInt(), 42);
}

// ============================================================================
// 嵌套结构测试
// ============================================================================

TEST(TestParseNestedList)
{
    ConfigParser parser;
    std::string err;
    std::string input = "data = [[\"a\", \"b\"], [\"c\"]]\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get("data").IsList());
    const auto& outer = parser.Get("data").AsList();
    ASSERT_EQ(outer.size(), size_t(2));
    ASSERT_TRUE(outer[0].IsList());
    ASSERT_EQ(outer[0].AsList().size(), size_t(2));
    ASSERT_TRUE(outer[1].IsList());
    ASSERT_EQ(outer[1].AsList().size(), size_t(1));
}

TEST(TestParseDictWithMixedValues)
{
    ConfigParser parser;
    std::string err;
    std::string input =
        "config = {\n"
        "    \"enabled\": true,\n"
        "    \"count\": 10,\n"
        "    \"name\": \"test\"\n"
        "}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    const auto& dict = parser.Get("config").AsDict();
    ASSERT_EQ(dict.at("enabled").AsBool(), true);
    ASSERT_EQ(dict.at("count").AsInt(), 10);
    ASSERT_EQ(dict.at("name").AsString(), "test");
}

// ============================================================================
// Windows \r\n 换行测试
// ============================================================================

TEST(TestParseCRLF)
{
    ConfigParser parser;
    std::string err;
    ASSERT_TRUE(parser.Parse("a = 1\r\nb = 2\r\n", &err));
    ASSERT_EQ(parser.Get("a").AsInt(), 1);
    ASSERT_EQ(parser.Get("b").AsInt(), 2);
}

// ============================================================================
// main
// ============================================================================

int main()
{
    printf("=== ConfigValue Tests ===\n");
    RUN_TEST(TestConfigValueNull);
    RUN_TEST(TestConfigValueBool);
    RUN_TEST(TestConfigValueInt);
    RUN_TEST(TestConfigValueFloat);
    RUN_TEST(TestConfigValueString);
    RUN_TEST(TestConfigValueList);
    RUN_TEST(TestConfigValueDict);
    RUN_TEST(TestConfigValueDefaults);
    RUN_TEST(TestConfigValueEquality);

    printf("\n=== ConfigParser Basic Tests ===\n");
    RUN_TEST(TestParseEmpty);
    RUN_TEST(TestParseComments);
    RUN_TEST(TestParseBool);
    RUN_TEST(TestParseInt);
    RUN_TEST(TestParseFloat);
    RUN_TEST(TestParseString);
    RUN_TEST(TestParseStringEscape);
    RUN_TEST(TestParseList);
    RUN_TEST(TestParseEmptyList);
    RUN_TEST(TestParseDict);
    RUN_TEST(TestParseEmptyDict);
    RUN_TEST(TestParseMixedTypes);

    printf("\n=== ConfigParser Operations Tests ===\n");
    RUN_TEST(TestGetNonExistent);
    RUN_TEST(TestSetAndGet);
    RUN_TEST(TestRemove);
    RUN_TEST(TestClear);

    printf("\n=== Serialization Tests ===\n");
    RUN_TEST(TestSerializeRoundTrip);

    printf("\n=== Config File Format Tests ===\n");
    RUN_TEST(TestParseConfigFile);

    printf("\n=== Error Handling Tests ===\n");
    RUN_TEST(TestParseErrorUnterminatedString);
    RUN_TEST(TestParseErrorUnterminatedList);
    RUN_TEST(TestParseErrorUnterminatedDict);
    RUN_TEST(TestParseErrorMissingEquals);

    printf("\n=== Edge Case Tests ===\n");
    RUN_TEST(TestParseUtf8Bom);
    RUN_TEST(TestParseNestedList);
    RUN_TEST(TestParseDictWithMixedValues);
    RUN_TEST(TestParseCRLF);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
