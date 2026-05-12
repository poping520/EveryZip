#include <cassert>
#include <cstdio>
#include <string>
#include <cmath>
#include <algorithm>
#include <windows.h>

#include "advconfig.h"
#include "user_config.h"

using namespace AdvConfig;

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
    Value v;
    ASSERT_TRUE(v.IsNull());
    ASSERT_EQ(v.GetType(), Value::Type::Null);
}

TEST(TestConfigValueBool)
{
    Value t(true);
    Value f(false);
    ASSERT_TRUE(t.IsBool());
    ASSERT_TRUE(f.IsBool());
    ASSERT_EQ(t.AsBool(), true);
    ASSERT_EQ(f.AsBool(), false);
}

TEST(TestConfigValueInt)
{
    Value v(int64_t(42));
    ASSERT_TRUE(v.IsInt());
    ASSERT_EQ(v.AsInt(), 42);

    Value neg(int64_t(-100));
    ASSERT_EQ(neg.AsInt(), -100);

    // int -> float 转换
    ASSERT_NEAR(v.AsFloat(), 42.0, 0.001);
}

TEST(TestConfigValueFloat)
{
    Value v(3.14);
    ASSERT_TRUE(v.IsFloat());
    ASSERT_NEAR(v.AsFloat(), 3.14, 0.001);

    // float -> int 转换
    ASSERT_EQ(v.AsInt(), 3);
}

TEST(TestConfigValueString)
{
    Value v(std::wstring(L"hello"));
    ASSERT_TRUE(v.IsString());
    ASSERT_EQ(v.AsString(), L"hello");

    Value v2(L"world");
    ASSERT_TRUE(v2.IsString());
    ASSERT_EQ(v2.AsString(), L"world");
}

TEST(TestConfigValueList)
{
    Value::List list;
    list.push_back(Value(1));
    list.push_back(Value(L"two"));
    list.push_back(Value(true));

    Value v(list);
    ASSERT_TRUE(v.IsList());
    ASSERT_EQ(v.AsList().size(), size_t(3));
    ASSERT_EQ(v.AsList()[0].AsInt(), 1);
    ASSERT_EQ(v.AsList()[1].AsString(), L"two");
    ASSERT_EQ(v.AsList()[2].AsBool(), true);
}

TEST(TestConfigValueDict)
{
    Value::Dict dict;
    dict[L"name"] = Value(L"test");
    dict[L"count"] = Value(int64_t(5));

    Value v(dict);
    ASSERT_TRUE(v.IsDict());
    ASSERT_EQ(v.AsDict().size(), size_t(2));
    ASSERT_EQ(v.AsDict().at(L"name").AsString(), L"test");
    ASSERT_EQ(v.AsDict().at(L"count").AsInt(), 5);
}

TEST(TestConfigValueDefaults)
{
    Value v; // Null
    ASSERT_EQ(v.AsBool(true), true);
    ASSERT_EQ(v.AsInt(99), 99);
    ASSERT_NEAR(v.AsFloat(1.5), 1.5, 0.001);
    ASSERT_EQ(v.AsString(), L"");
    ASSERT_TRUE(v.AsList().empty());
    ASSERT_TRUE(v.AsDict().empty());
}

TEST(TestConfigValueEquality)
{
    ASSERT_TRUE(Value(true) == Value(true));
    ASSERT_TRUE(Value(true) != Value(false));
    ASSERT_TRUE(Value(int64_t(1)) == Value(int64_t(1)));
    ASSERT_TRUE(Value(L"abc") == Value(L"abc"));
    ASSERT_TRUE(Value(L"abc") != Value(L"def"));
}

// ============================================================================
// ConfigParser 基础测试
// ============================================================================

TEST(TestParseEmpty)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"", &err));
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

TEST(TestParseComments)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"# comment\n; another comment\n", &err));
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

TEST(TestParseBool)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"a = true\nb=false\n", &err));
    ASSERT_TRUE(parser.Get(L"a").IsBool());
    ASSERT_EQ(parser.Get(L"a").AsBool(), true);
    ASSERT_TRUE(parser.Get(L"b").IsBool());
    ASSERT_EQ(parser.Get(L"b").AsBool(), false);
}

TEST(TestParseInt)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"x = 123\ny = -456\n", &err));
    ASSERT_TRUE(parser.Get(L"x").IsInt());
    ASSERT_EQ(parser.Get(L"x").AsInt(), 123);
    ASSERT_TRUE(parser.Get(L"y").IsInt());
    ASSERT_EQ(parser.Get(L"y").AsInt(), -456);
}

TEST(TestParseFloat)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"pi = 3.14159\n", &err));
    ASSERT_TRUE(parser.Get(L"pi").IsFloat());
    ASSERT_NEAR(parser.Get(L"pi").AsFloat(), 3.14159, 0.00001);
}

TEST(TestParseString)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"name = \"hello world\"\n", &err));
    ASSERT_TRUE(parser.Get(L"name").IsString());
    ASSERT_EQ(parser.Get(L"name").AsString(), L"hello world");
}

TEST(TestParseStringEscape)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"s = \"line1\\nline2\\ttab\"\n", &err));
    ASSERT_EQ(parser.Get(L"s").AsString(), L"line1\nline2\ttab");
}

TEST(TestParseList)
{
    Parser parser;
    std::wstring err;
    std::wstring input =
        L"items = [\n"
        L"    \"a\",\n"
        L"    \"b\",\n"
        L"    \"c\"\n"
        L"]\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get(L"items").IsList());
    const auto& list = parser.Get(L"items").AsList();
    ASSERT_EQ(list.size(), size_t(3));
    ASSERT_EQ(list[0].AsString(), L"a");
    ASSERT_EQ(list[1].AsString(), L"b");
    ASSERT_EQ(list[2].AsString(), L"c");
}

TEST(TestParseEmptyList)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"items = []\n", &err));
    ASSERT_TRUE(parser.Get(L"items").IsList());
    ASSERT_EQ(parser.Get(L"items").AsList().size(), size_t(0));
}

TEST(TestParseDict)
{
    Parser parser;
    std::wstring err;
    std::wstring input =
        L"data = {\n"
        L"    \"key1\": \"value1\",\n"
        L"    \"key2\": \"value2\"\n"
        L"}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get(L"data").IsDict());
    const auto& dict = parser.Get(L"data").AsDict();
    ASSERT_EQ(dict.size(), size_t(2));
    ASSERT_EQ(dict.at(L"key1").AsString(), L"value1");
    ASSERT_EQ(dict.at(L"key2").AsString(), L"value2");
}

TEST(TestParseEmptyDict)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"data = {}\n", &err));
    ASSERT_TRUE(parser.Get(L"data").IsDict());
    ASSERT_EQ(parser.Get(L"data").AsDict().size(), size_t(0));
}

TEST(TestParseMixedTypes)
{
    Parser parser;
    std::wstring err;
    std::wstring input =
        L"flag = true\n"
        L"count = 42\n"
        L"ratio = 1.5\n"
        L"name = \"test\"\n"
        L"tags = [\"a\", \"b\"]\n"
        L"meta = {\"x\": \"y\"}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get(L"flag").IsBool());
    ASSERT_TRUE(parser.Get(L"count").IsInt());
    ASSERT_TRUE(parser.Get(L"ratio").IsFloat());
    ASSERT_TRUE(parser.Get(L"name").IsString());
    ASSERT_TRUE(parser.Get(L"tags").IsList());
    ASSERT_TRUE(parser.Get(L"meta").IsDict());
}

// ============================================================================
// ConfigParser 操作测试
// ============================================================================

TEST(TestGetNonExistent)
{
    Parser parser;
    ASSERT_TRUE(parser.Get(L"missing").IsNull());
}

TEST(TestSetAndGet)
{
    Parser parser;
    parser.Set(L"key", Value(int64_t(100)));
    ASSERT_TRUE(parser.Contains(L"key"));
    ASSERT_EQ(parser.Get(L"key").AsInt(), 100);
}

TEST(TestRemove)
{
    Parser parser;
    parser.Set(L"a", Value(true));
    ASSERT_TRUE(parser.Contains(L"a"));
    ASSERT_TRUE(parser.Remove(L"a"));
    ASSERT_FALSE(parser.Contains(L"a"));
    ASSERT_FALSE(parser.Remove(L"a"));
}

TEST(TestClear)
{
    Parser parser;
    parser.Set(L"a", Value(1));
    parser.Set(L"b", Value(2));
    parser.Clear();
    ASSERT_EQ(parser.GetAll().size(), size_t(0));
}

// ============================================================================
// ConfigParser 序列化测试
// ============================================================================

TEST(TestSerializeRoundTrip)
{
    Parser parser;
    parser.Set(L"flag", Value(true));
    parser.Set(L"count", Value(int64_t(42)));
    parser.Set(L"ratio", Value(1.5));
    parser.Set(L"name", Value(L"hello"));

    Value::List list;
    list.push_back(Value(L"a"));
    list.push_back(Value(L"b"));
    parser.Set(L"tags", Value(std::move(list)));

    Value::Dict dict;
    dict[L"x"] = Value(L"y");
    parser.Set(L"meta", Value(std::move(dict)));

    std::wstring serialized = parser.Serialize();

    Parser parser2;
    std::wstring err;
    ASSERT_TRUE(parser2.Parse(serialized, &err));

    ASSERT_EQ(parser2.Get(L"flag").AsBool(), true);
    ASSERT_EQ(parser2.Get(L"count").AsInt(), 42);
    ASSERT_NEAR(parser2.Get(L"ratio").AsFloat(), 1.5, 0.001);
    ASSERT_EQ(parser2.Get(L"name").AsString(), L"hello");
    ASSERT_EQ(parser2.Get(L"tags").AsList().size(), size_t(2));
    ASSERT_EQ(parser2.Get(L"meta").AsDict().at(L"x").AsString(), L"y");
}

// ============================================================================
// 解析 test_config.cfg 格式测试
// ============================================================================

TEST(TestParseConfigFile)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.LoadFile(L"test_config.cfg", &err));

    // 布尔值
    ASSERT_TRUE(parser.Get(L"bool").IsBool());
    ASSERT_EQ(parser.Get(L"bool").AsBool(), true);
    ASSERT_TRUE(parser.Get(L"bool2").IsBool());
    ASSERT_EQ(parser.Get(L"bool2").AsBool(), false);

    // 整数
    ASSERT_TRUE(parser.Get(L"int").IsInt());
    ASSERT_EQ(parser.Get(L"int").AsInt(), 123);
    ASSERT_TRUE(parser.Get(L"int2").IsInt());
    ASSERT_EQ(parser.Get(L"int2").AsInt(), -123);

    // 浮点数
    ASSERT_TRUE(parser.Get(L"float").IsFloat());
    ASSERT_NEAR(parser.Get(L"float").AsFloat(), 123.456, 0.001);

    // 字符串
    ASSERT_TRUE(parser.Get(L"string").IsString());
    ASSERT_EQ(parser.Get(L"string").AsString(), L"This is a string value");

    // 列表
    ASSERT_TRUE(parser.Get(L"list").IsList());
    const auto& list = parser.Get(L"list").AsList();
    ASSERT_EQ(list.size(), size_t(3));
    ASSERT_EQ(list[0].AsString(), L"value1");
    ASSERT_EQ(list[1].AsString(), L"value2]");
    // ASSERT_EQ(list[2].AsString(), L"[value3");

    // 字典
    ASSERT_TRUE(parser.Get(L"dict").IsDict());
    const auto& dict = parser.Get(L"dict").AsDict();
    ASSERT_EQ(dict.size(), size_t(3));
    ASSERT_EQ(dict.at(L"key1").AsString(), L"{value1");
    ASSERT_EQ(dict.at(L"key2").AsString(), L"[value2}");
    ASSERT_EQ(dict.at(L"key3]").AsString(), L"你好");
}

// ============================================================================
// 错误处理测试
// ============================================================================

TEST(TestParseErrorUnterminatedString)
{
    Parser parser;
    std::wstring err;
    ASSERT_FALSE(parser.Parse(L"s = \"unterminated\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorUnterminatedList)
{
    Parser parser;
    std::wstring err;
    ASSERT_FALSE(parser.Parse(L"l = [\"a\", \"b\"\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorUnterminatedDict)
{
    Parser parser;
    std::wstring err;
    ASSERT_FALSE(parser.Parse(L"d = {\"a\": \"b\"\n", &err));
    ASSERT_FALSE(err.empty());
}

TEST(TestParseErrorMissingEquals)
{
    Parser parser;
    std::wstring err;
    ASSERT_FALSE(parser.Parse(L"key_without_value\n", &err));
    ASSERT_FALSE(err.empty());
}

// ============================================================================
// UTF-8 BOM 测试
// ============================================================================

TEST(TestParseUtf8Bom)
{
    Parser parser;
    std::wstring err;
    std::wstring content = L"\xFEFF" L"x = 42\n";
    ASSERT_TRUE(parser.Parse(content, &err));
    ASSERT_EQ(parser.Get(L"x").AsInt(), 42);
}

// ============================================================================
// 嵌套结构测试
// ============================================================================

TEST(TestParseNestedList)
{
    Parser parser;
    std::wstring err;
    std::wstring input = L"data = [[\"a\", \"b\"], [\"c\"]]\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    ASSERT_TRUE(parser.Get(L"data").IsList());
    const auto& outer = parser.Get(L"data").AsList();
    ASSERT_EQ(outer.size(), size_t(2));
    ASSERT_TRUE(outer[0].IsList());
    ASSERT_EQ(outer[0].AsList().size(), size_t(2));
    ASSERT_TRUE(outer[1].IsList());
    ASSERT_EQ(outer[1].AsList().size(), size_t(1));
}

TEST(TestParseDictWithMixedValues)
{
    Parser parser;
    std::wstring err;
    std::wstring input =
        L"config = {\n"
        L"    \"enabled\": true,\n"
        L"    \"count\": 10,\n"
        L"    \"name\": \"test\"\n"
        L"}\n";
    ASSERT_TRUE(parser.Parse(input, &err));
    const auto& dict = parser.Get(L"config").AsDict();
    ASSERT_EQ(dict.at(L"enabled").AsBool(), true);
    ASSERT_EQ(dict.at(L"count").AsInt(), 10);
    ASSERT_EQ(dict.at(L"name").AsString(), L"test");
}

// ============================================================================
// Windows \r\n 换行测试
// ============================================================================

TEST(TestParseCRLF)
{
    Parser parser;
    std::wstring err;
    ASSERT_TRUE(parser.Parse(L"a = 1\r\nb = 2\r\n", &err));
    ASSERT_EQ(parser.Get(L"a").AsInt(), 1);
    ASSERT_EQ(parser.Get(L"b").AsInt(), 2);
}

TEST(TestUserConfigUiStateDefaultsAndRoundTrip)
{
    const wchar_t* path = L"test_user_config_ui_state.cfg";
    DeleteFileW(path);

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));
        ASSERT_TRUE(config.GetRememberUiState());
        ASSERT_FALSE(config.GetStartupScanConfirmed());

        const auto& defaultWidths = config.GetListColumnWidths();
        ASSERT_EQ(defaultWidths.size(), size_t(6));
        ASSERT_EQ(defaultWidths[0], 210);
        ASSERT_EQ(defaultWidths[1], 210);
        ASSERT_EQ(defaultWidths[2], 280);
        ASSERT_EQ(defaultWidths[3], 100);
        ASSERT_EQ(defaultWidths[4], 100);
        ASSERT_EQ(defaultWidths[5], 140);

        UserConfig::WindowPlacementConfig placement;
        placement.left = 10;
        placement.top = 20;
        placement.right = 1210;
        placement.bottom = 820;
        placement.maximized = true;
        config.SetWindowPlacement(placement);
        config.SetRememberUiState(false);
        config.SetStartupScanConfirmed(true);
        config.SetListColumnWidths({ 111, 222, 333, 444, 555, 666 });
        ASSERT_TRUE(config.Save(&err));

        Parser parser;
        ASSERT_TRUE(parser.LoadFile(path, &err));
        ASSERT_FALSE(parser.Contains(L"window_left"));
        ASSERT_FALSE(parser.Contains(L"window_top"));
        ASSERT_FALSE(parser.Contains(L"window_right"));
        ASSERT_FALSE(parser.Contains(L"window_bottom"));
        ASSERT_TRUE(parser.Get(L"window_rect").IsDict());
        const auto& rect = parser.Get(L"window_rect").AsDict();
        ASSERT_EQ(rect.at(L"left").AsInt(), 10);
        ASSERT_EQ(rect.at(L"top").AsInt(), 20);
        ASSERT_EQ(rect.at(L"right").AsInt(), 1210);
        ASSERT_EQ(rect.at(L"bottom").AsInt(), 820);
    }

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));
        ASSERT_FALSE(config.GetRememberUiState());
        ASSERT_TRUE(config.GetStartupScanConfirmed());

        const auto& placement = config.GetWindowPlacement();
        ASSERT_EQ(placement.left, 10);
        ASSERT_EQ(placement.top, 20);
        ASSERT_EQ(placement.right, 1210);
        ASSERT_EQ(placement.bottom, 820);
        ASSERT_TRUE(placement.maximized);

        const auto& widths = config.GetListColumnWidths();
        ASSERT_EQ(widths.size(), size_t(6));
        ASSERT_EQ(widths[0], 111);
        ASSERT_EQ(widths[1], 222);
        ASSERT_EQ(widths[2], 333);
        ASSERT_EQ(widths[3], 444);
        ASSERT_EQ(widths[4], 555);
        ASSERT_EQ(widths[5], 666);
    }

    DeleteFileW(path);
}

TEST(TestUserConfigInvalidColumnWidthsFallback)
{
    const wchar_t* path = L"test_user_config_invalid_widths.cfg";
    DeleteFileW(path);

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));
        config.SetListColumnWidths({ 100, 0, 300 });
        ASSERT_TRUE(config.Save(&err));
    }

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));

        const auto& widths = config.GetListColumnWidths();
        ASSERT_EQ(widths.size(), size_t(6));
        ASSERT_EQ(widths[0], 210);
        ASSERT_EQ(widths[1], 210);
        ASSERT_EQ(widths[2], 280);
        ASSERT_EQ(widths[3], 100);
        ASSERT_EQ(widths[4], 100);
        ASSERT_EQ(widths[5], 140);
    }

    DeleteFileW(path);
}

TEST(TestUserConfigResetListColumnWidths)
{
    const wchar_t* path = L"test_user_config_reset_widths.cfg";
    DeleteFileW(path);

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));
        config.SetListColumnWidths({ 111, 222, 333, 444, 555, 666 });
        config.ResetListColumnWidths();
        ASSERT_TRUE(config.Save(&err));
    }

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));

        const auto& widths = config.GetListColumnWidths();
        const auto& defaultWidths = UserConfig::GetDefaultListColumnWidths();
        ASSERT_EQ(widths.size(), defaultWidths.size());
        for (size_t i = 0; i < defaultWidths.size(); ++i) {
            ASSERT_EQ(widths[i], defaultWidths[i]);
        }
    }

    DeleteFileW(path);
}

TEST(TestUserConfigArchiveFormatDefaults)
{
    const wchar_t* path = L"test_user_config_archive_defaults.cfg";
    DeleteFileW(path);

    UserConfig config;
    std::wstring err;
    ASSERT_TRUE(config.Load(path, &err));

    const auto& rules = config.GetArchiveFormatRules();
    ASSERT_EQ(rules.size(), size_t(7));
    ASSERT_EQ(config.GetParserForExtension(L".zip"), L"zip");
    ASSERT_EQ(config.GetParserForExtension(L".rar"), L"rar");
    ASSERT_EQ(config.GetParserForExtension(L".7z"), L"7z");
    ASSERT_EQ(config.GetParserForExtension(L".apk"), L"");
    ASSERT_EQ(config.GetParserForExtension(L".ipa"), L"");
    ASSERT_EQ(config.GetParserForExtension(L".jar"), L"");
    ASSERT_EQ(config.GetParserForExtension(L".war"), L"");

    const auto& exts = config.GetArchiveExtensions();
    ASSERT_EQ(exts.size(), size_t(3));
    ASSERT_EQ(exts[0], L".zip");
    ASSERT_EQ(exts[1], L".rar");
    ASSERT_EQ(exts[2], L".7z");

    Parser parser;
    ASSERT_TRUE(parser.LoadFile(path, &err));
    ASSERT_TRUE(parser.Get(L"archive_formats").IsDict());
    const auto& groups = parser.Get(L"archive_formats").AsDict();
    ASSERT_TRUE(groups.at(L"default").IsList());
    ASSERT_TRUE(groups.at(L"known_aliases").IsList());
    ASSERT_TRUE(groups.at(L"custom").IsList());
    ASSERT_EQ(groups.at(L"known_aliases").AsList().size(), size_t(4));

    DeleteFileW(path);
}

TEST(TestUserConfigArchiveFormatRoundTrip)
{
    const wchar_t* path = L"test_user_config_archive_roundtrip.cfg";
    DeleteFileW(path);

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));

        std::vector<UserConfig::ArchiveFormatRule> rules = config.GetArchiveFormatRules();
        for (auto& rule : rules) {
            if (rule.extension == L".apk") rule.enabled = true;
            if (rule.extension == L".zip") rule.enabled = false;
        }
        rules.push_back({ L".foo", L"rar", true, L"custom" });
        config.SetArchiveFormatRules(rules);
        ASSERT_TRUE(config.Save(&err));
    }

    {
        UserConfig config;
        std::wstring err;
        ASSERT_TRUE(config.Load(path, &err));

        ASSERT_EQ(config.GetParserForExtension(L".zip"), L"");
        ASSERT_EQ(config.GetParserForExtension(L".apk"), L"zip");
        ASSERT_EQ(config.GetParserForExtension(L".foo"), L"rar");

        const auto& exts = config.GetArchiveExtensions();
        ASSERT_TRUE(std::find(exts.begin(), exts.end(), L".apk") != exts.end());
        ASSERT_TRUE(std::find(exts.begin(), exts.end(), L".foo") != exts.end());
        ASSERT_TRUE(std::find(exts.begin(), exts.end(), L".zip") == exts.end());
    }

    DeleteFileW(path);
}

TEST(TestUserConfigCustomArchiveExtensionValidation)
{
    ASSERT_EQ(UserConfig::NormalizeArchiveExtension(L" foo "), L".foo");
    ASSERT_TRUE(UserConfig::IsValidCustomArchiveExtension(L".foo"));
    ASSERT_TRUE(UserConfig::IsValidCustomArchiveExtension(L"bar"));
    ASSERT_FALSE(UserConfig::IsValidCustomArchiveExtension(L""));
    ASSERT_FALSE(UserConfig::IsValidCustomArchiveExtension(L"."));
    ASSERT_FALSE(UserConfig::IsValidCustomArchiveExtension(L".bad/name"));
    ASSERT_FALSE(UserConfig::IsValidCustomArchiveExtension(L".bad.name"));
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
    RUN_TEST(TestUserConfigUiStateDefaultsAndRoundTrip);
    RUN_TEST(TestUserConfigInvalidColumnWidthsFallback);
    RUN_TEST(TestUserConfigResetListColumnWidths);
    RUN_TEST(TestUserConfigArchiveFormatDefaults);
    RUN_TEST(TestUserConfigArchiveFormatRoundTrip);
    RUN_TEST(TestUserConfigCustomArchiveExtensionValidation);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
