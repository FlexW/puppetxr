#include "base.h"

#include <gtest/gtest.h>

static const char* DOC = R"({
    "name": "puppetxr",
    "version": 3,
    "scale": 1.5,
    "enabled": true,
    "disabled": false,
    "nothing": null,
    "escaped": "a\"b\\c\ndA",
    "values": [1, 2, 3],
    "nested": { "inner": { "deep": -7 } },
    "objects": [ { "id": 1 }, { "id": 2 } ]
})";

TEST(Json, parse_and_find_scalars)
{
    JsonParser* parser = json_parse_from_string(DOC);
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };

    const JsonValue root = json_get_root(parser);
    EXPECT_TRUE(json_is_object(root));

    char  name[32] = {};
    usize name_size = 0;
    EXPECT_TRUE(json_find_string(root, "name", name, &name_size, sizeof(name)));
    EXPECT_TRUE(cstr_equal(name, "puppetxr"));
    EXPECT_EQ(name_size, 8u);

    i32 version = 0;
    EXPECT_TRUE(json_find_int(root, "version", version));
    EXPECT_EQ(version, 3);

    u32 uversion = 0;
    EXPECT_TRUE(json_find_uint(root, "version", uversion));
    EXPECT_EQ(uversion, 3u);

    f32 scale = 0.0f;
    EXPECT_TRUE(json_find_float(root, "scale", scale));
    EXPECT_FLOAT_EQ(scale, 1.5f);

    b8 enabled = false;
    b8 disabled = true;
    EXPECT_TRUE(json_find_bool(root, "enabled", enabled));
    EXPECT_TRUE(json_find_bool(root, "disabled", disabled));
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(disabled);

    EXPECT_TRUE(json_is_null(json_find(root, "nothing")));
}

TEST(Json, missing_keys_yield_defaults)
{
    JsonParser* parser = json_parse_from_string(DOC);
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };
    const JsonValue root = json_get_root(parser);

    i32 missing = 0;
    EXPECT_FALSE(json_find_int(root, "missing", missing, 42));
    EXPECT_EQ(missing, 42);

    f32 wrong_type = 0.0f;
    EXPECT_FALSE(json_find_float(root, "name", wrong_type, 2.0f));
    EXPECT_FLOAT_EQ(wrong_type, 2.0f);

    char  buf[8] = {};
    usize size   = 0;
    EXPECT_FALSE(json_find_string(root, "version", buf, &size, sizeof(buf), "dflt"));
    EXPECT_TRUE(cstr_equal(buf, "dflt"));

    EXPECT_FALSE(json_is_valid(json_find(root, "missing")));
}

TEST(Json, strings_are_unescaped_and_truncated)
{
    JsonParser* parser = json_parse_from_string(DOC);
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };
    const JsonValue root = json_get_root(parser);

    char  buf[32] = {};
    usize size    = 0;
    EXPECT_TRUE(json_find_string(root, "escaped", buf, &size, sizeof(buf)));
    EXPECT_TRUE(cstr_equal(buf, "a\"b\\c\nd" "A"));

    char small[4] = {};
    EXPECT_FALSE(json_find_string(root, "name", small, &size, sizeof(small)));
    EXPECT_TRUE(cstr_equal(small, "pup"));
}

TEST(Json, arrays_and_nesting)
{
    JsonParser* parser = json_parse_from_string(DOC);
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };
    const JsonValue root = json_get_root(parser);

    const JsonValue values = json_find(root, "values");
    ASSERT_TRUE(json_is_array(values));
    EXPECT_EQ(json_length(values), 3u);

    i32 sum = 0;
    for (const JsonValue v : values)
    {
        i32 n = 0;
        EXPECT_TRUE(json_as_int(v, n));
        sum += n;
    }
    EXPECT_EQ(sum, 6);

    i32 third = 0;
    EXPECT_TRUE(json_as_int(json_at(values, 2), third));
    EXPECT_EQ(third, 3);
    EXPECT_FALSE(json_is_valid(json_at(values, 3)));

    i32 deep = 0;
    EXPECT_TRUE(json_find_int(json_find(json_find(root, "nested"), "inner"), "deep", deep));
    EXPECT_EQ(deep, -7);

    const JsonValue objects = json_find(root, "objects");
    i32             ids     = 0;
    for (const JsonValue o : objects)
    {
        i32 id = 0;
        EXPECT_TRUE(json_find_int(o, "id", id));
        ids += id;
    }
    EXPECT_EQ(ids, 3);
}

TEST(Json, member_iteration)
{
    JsonParser* parser = json_parse_from_string(R"({"a": 1, "b": [1, 2], "c": {"x": 0}})");
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };

    u32  count   = 0;
    char keys[8] = {};
    for (const JsonMember m : json_members(json_get_root(parser)))
    {
        char  key[2] = {};
        usize size   = 0;
        EXPECT_TRUE(json_as_string(m.key, key, &size, sizeof(key)));
        keys[count++] = key[0];
    }
    EXPECT_EQ(count, 3u);
    EXPECT_TRUE(cstr_equal(keys, "abc"));
}

TEST(Json, rejects_invalid_documents)
{
    EXPECT_EQ(json_parse_from_string("{"), nullptr);
    EXPECT_EQ(json_parse_from_string("{\"a\": }"), nullptr);
    EXPECT_EQ(json_parse_from_string(""), nullptr);
    EXPECT_EQ(json_parse_from_string(nullptr), nullptr);
    EXPECT_EQ(json_parse_from_file("/nonexistent/puppetxr.json"), nullptr);
}

TEST(Json, invalid_values_are_safe)
{
    const JsonValue none = {};
    EXPECT_FALSE(json_is_valid(none));
    EXPECT_FALSE(json_is_object(none));
    EXPECT_FALSE(json_is_array(none));
    EXPECT_EQ(json_length(none), 0u);
    EXPECT_FALSE(json_is_valid(json_find(none, "x")));
    EXPECT_FALSE(json_is_valid(json_at(none, 0)));
    i32 n = 1;
    EXPECT_FALSE(json_as_int(none, n, 5));
    EXPECT_EQ(n, 5);
    u32 iterations = 0;
    for (const JsonValue v : none)
    {
        UNUSED(v);
        iterations++;
    }
    for (const JsonMember m : json_members(none))
    {
        UNUSED(m);
        iterations++;
    }
    EXPECT_EQ(iterations, 0u);
}

TEST(Json, raw_text_of_values)
{
    JsonParser* parser = json_parse_from_string(R"({ "hash": 18446744073709551615, "list": [1, 2.5], "text": "a\"b" })");
    ASSERT_NE(parser, nullptr);
    const JsonValue root   = json_get_root(parser);
    const char*     text   = nullptr;
    usize           length = 0;
    ASSERT_TRUE(json_raw(json_find(root, "hash"), text, length));
    EXPECT_EQ(length, 20u);
    EXPECT_TRUE(memory_equal(text, "18446744073709551615", 20));
    ASSERT_TRUE(json_raw(json_find(root, "list"), text, length));
    EXPECT_TRUE(memory_equal(text, "[1, 2.5]", length));
    ASSERT_TRUE(json_raw(json_find(root, "text"), text, length));
    EXPECT_TRUE(memory_equal(text, "a\\\"b", length)); // still escaped, without the quotes
    EXPECT_FALSE(json_raw(json_find(root, "missing"), text, length));
    json_release(parser);
}


TEST(Json, u64_values_are_exact_integers)
{
    JsonParser* parser = json_parse_from_string(R"({ "big": 18446744073709551615, "small": 7, "negative": -1, "fraction": 1.5, "huge": 18446744073709551616, "text": "5" })");
    ASSERT_NE(parser, nullptr);
    const JsonValue root  = json_get_root(parser);
    u64             value = 0;
    EXPECT_TRUE(json_find_u64(root, "big", value));
    EXPECT_EQ(value, 18446744073709551615ull);
    EXPECT_TRUE(json_find_u64(root, "small", value));
    EXPECT_EQ(value, 7u);
    EXPECT_FALSE(json_find_u64(root, "negative", value, 3));
    EXPECT_EQ(value, 3u);
    EXPECT_FALSE(json_find_u64(root, "fraction", value));
    EXPECT_FALSE(json_find_u64(root, "huge", value));
    EXPECT_FALSE(json_find_u64(root, "text", value));
    EXPECT_FALSE(json_find_u64(root, "missing", value));
    json_release(parser);
}

// Floats written for JSON read back bit-exactly: SDL's %g loses digits (0.38268343 printed as
// 0.382683486), so control API replies and saved bodies use json_format_f32
TEST(Json, formatted_floats_read_back_exactly)
{
    char text[32];
    json_format_f32(text, sizeof(text), 0.38268343f);
    EXPECT_STREQ(text, "0.382683426");
    json_format_f32(text, sizeof(text), 1.5f);
    EXPECT_STREQ(text, "1.5");
    json_format_f32(text, sizeof(text), -0.0f);
    EXPECT_STREQ(text, "-0");
    json_format_f32(text, sizeof(text), 100.0f);
    EXPECT_STREQ(text, "100");
    json_format_f32(text, sizeof(text), 1e-30f);
    EXPECT_STREQ(text, "1e-30");
    const u32 nan_bits = 0x7fc00000u;
    f32       nan      = 0.0f;
    memory_copy(&nan, &nan_bits, sizeof(nan));
    json_format_f32(text, sizeof(text), nan);
    EXPECT_STREQ(text, "0") << "JSON has no NaN";

    // Every exponent, mantissas spread over the range
    u32 failures = 0;
    for (u32 exponent = 0; exponent < 255; exponent++)
    {
        for (u32 m = 0; m < 2048; m++)
        {
            const u32 bits  = (exponent << 23) | (m * 4099u % (1u << 23)) | (m & 1u ? 0x80000000u : 0u);
            f32       value = 0.0f;
            memory_copy(&value, &bits, sizeof(value));
            json_format_f32(text, sizeof(text), value);
            char doc[48];
            cstr_format(doc, sizeof(doc), "[%s]", text);
            JsonParser* parser = json_parse_from_string(doc);
            f32         parsed = 0.0f;
            if (!parser || !json_as_float(json_at(json_get_root(parser), 0), parsed) || !memory_equal(&parsed, &value, sizeof(value)))
            {
                failures++;
                EXPECT_LT(failures, 5u) << text;
            }
            json_release(parser);
        }
    }
    EXPECT_EQ(failures, 0u);
}
