#include "base.h"

#define JSMN_STATIC
#define JSMN_STRICT
#include <jsmn.h>

//
// Types
//

#define JSON_MAX_DEPTH 64

struct JsonParser
{
    char*      text;
    usize      text_size;
    jsmntok_t* tokens;
    u32*       subtree_end; // index of the first token after each token's subtree
    u32        tokens_count;
};

//
// Statics
//

static u32 crc32_table[256];
static b8  crc32_table_ready;

static void crc32_init ()
{
    for (u32 i = 0; i < 256; i++)
    {
        u32 c = i;
        for (u32 k = 0; k < 8; k++)
        {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

static b8 json_compute_subtree_ends (JsonParser* parser, u32 index, u32 depth, u32& out_end)
{
    if (depth > JSON_MAX_DEPTH || index >= parser->tokens_count)
    {
        return false;
    }

    const jsmntok_t& token = parser->tokens[index];
    u32              end   = index + 1;

    if (token.type == JSMN_OBJECT || token.type == JSMN_ARRAY || (token.type == JSMN_STRING && token.size == 1))
    {
        for (i32 i = 0; i < token.size; i++)
        {
            if (token.type == JSMN_OBJECT && (end >= parser->tokens_count || parser->tokens[end].type != JSMN_STRING || parser->tokens[end].size != 1))
            {
                return false;
            }
            u32 child_end = 0;
            if (!json_compute_subtree_ends(parser, end, depth + 1, child_end))
            {
                return false;
            }
            end = child_end;
        }
    }

    parser->subtree_end[index] = end;
    out_end                    = end;
    return true;
}

static JsonParser* json_parse (char* text, usize text_size)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);
    const i32 count = jsmn_parse(&jsmn, text, text_size, nullptr, 0);
    if (count <= 0)
    {
        os_memory_release(text);
        return nullptr;
    }

    JsonParser* parser   = (JsonParser*)os_memory_alloc(sizeof(JsonParser));
    parser->text         = text;
    parser->text_size    = text_size;
    parser->tokens_count = (u32)count;
    parser->tokens       = (jsmntok_t*)os_memory_alloc((usize)count * sizeof(jsmntok_t));
    parser->subtree_end  = (u32*)os_memory_alloc((usize)count * sizeof(u32));

    jsmn_init(&jsmn);
    const i32 parsed = jsmn_parse(&jsmn, text, text_size, parser->tokens, (u32)count);

    u32 end = 0;
    if (parsed != count || !json_compute_subtree_ends(parser, 0, 0, end) || end != (u32)count)
    {
        json_release(parser);
        return nullptr;
    }

    return parser;
}

static const jsmntok_t* json_token (const JsonValue& value)
{
    if (!value.parser || value.index >= value.parser->tokens_count)
    {
        return nullptr;
    }
    return &value.parser->tokens[value.index];
}

static b8 json_token_text (const JsonValue& value, const char*& out_text, usize& out_size)
{
    const jsmntok_t* token = json_token(value);
    if (!token)
    {
        return false;
    }
    out_text = value.parser->text + token->start;
    out_size = (usize)(token->end - token->start);
    return true;
}

static b8 json_token_equals (const JsonValue& value, const char* str)
{
    const char* text = nullptr;
    usize       size = 0;
    if (!json_token_text(value, text, size))
    {
        return false;
    }
    return size == cstr_length(str) && memory_equal(text, str, size);
}

static u32 json_hex_digit (char c)
{
    if (c >= '0' && c <= '9')
    {
        return (u32)(c - '0');
    }
    if (c >= 'a' && c <= 'f')
    {
        return (u32)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F')
    {
        return (u32)(c - 'A' + 10);
    }
    return 0xFFFFFFFFu;
}

static usize json_write_utf8 (char* out, usize out_size, u32 codepoint)
{
    char  bytes[4] = {};
    usize count    = 0;
    if (codepoint < 0x80)
    {
        bytes[0] = (char)codepoint;
        count    = 1;
    }
    else if (codepoint < 0x800)
    {
        bytes[0] = (char)(0xC0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3F));
        count    = 2;
    }
    else if (codepoint < 0x10000)
    {
        bytes[0] = (char)(0xE0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (codepoint & 0x3F));
        count    = 3;
    }
    else
    {
        bytes[0] = (char)(0xF0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (codepoint & 0x3F));
        count    = 4;
    }

    const usize written = count < out_size ? count : out_size;
    memory_copy(out, bytes, written);
    return written;
}

// Decimal to double without libc so results are identical on every platform.
// Exact for up to 19 significant digits and |exponent| <= 22, which covers config files.
static b8 json_parse_number (const char* text, usize size, f64& out_value)
{
    usize i        = 0;
    b8    negative = false;
    if (i < size && (text[i] == '-' || text[i] == '+'))
    {
        negative = text[i] == '-';
        i++;
    }

    u64 mantissa  = 0;
    i32 exponent  = 0;
    u32 digits    = 0;
    b8  any_digit = false;

    for (; i < size && text[i] >= '0' && text[i] <= '9'; i++)
    {
        any_digit = true;
        if (digits < 19)
        {
            mantissa = mantissa * 10 + (u64)(text[i] - '0');
            digits++;
        }
        else
        {
            exponent++;
        }
    }

    if (i < size && text[i] == '.')
    {
        i++;
        for (; i < size && text[i] >= '0' && text[i] <= '9'; i++)
        {
            any_digit = true;
            if (digits < 19)
            {
                mantissa = mantissa * 10 + (u64)(text[i] - '0');
                digits++;
                exponent--;
            }
        }
    }

    if (!any_digit)
    {
        return false;
    }

    if (i < size && (text[i] == 'e' || text[i] == 'E'))
    {
        i++;
        b8 exp_negative = false;
        if (i < size && (text[i] == '-' || text[i] == '+'))
        {
            exp_negative = text[i] == '-';
            i++;
        }
        i32 exp = 0;
        b8  any = false;
        for (; i < size && text[i] >= '0' && text[i] <= '9'; i++)
        {
            any = true;
            if (exp < 10000)
            {
                exp = exp * 10 + (text[i] - '0');
            }
        }
        if (!any)
        {
            return false;
        }
        exponent += exp_negative ? -exp : exp;
    }

    if (i != size)
    {
        return false;
    }

    static const f64 powers[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

    f64 value = (f64)mantissa;
    while (exponent > 22)
    {
        value *= 1e22;
        exponent -= 22;
    }
    while (exponent < -22)
    {
        value /= 1e22;
        exponent += 22;
    }
    if (exponent > 0)
    {
        value *= powers[exponent];
    }
    else if (exponent < 0)
    {
        value /= powers[-exponent];
    }

    out_value = negative ? -value : value;
    return true;
}

static f64 json_pow10 (i32 n)
{
    static const f64 powers[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    f64 scale = 1.0;
    for (; n > 22; n -= 22)
    {
        scale *= 1e22;
    }
    for (; n < -22; n += 22)
    {
        scale /= 1e22;
    }
    return n >= 0 ? scale * powers[n] : scale / powers[-n];
}

//
// Hashes
//

u64 hash (const u8* key, usize size)
{
    u64 h = 0xCBF29CE484222325ull;
    for (usize i = 0; i < size; i++)
    {
        h = (h ^ key[i]) * 0x100000001B3ull;
    }
    return h;
}

u32 hash_u32 (const u8* key, usize size)
{
    u32 h = 0x811C9DC5u;
    for (usize i = 0; i < size; i++)
    {
        h = (h ^ key[i]) * 0x01000193u;
    }
    return h;
}

u32 crc32 (const u8* data, usize size)
{
    if (!crc32_table_ready)
    {
        crc32_init();
    }
    u32 c = 0xFFFFFFFFu;
    for (usize i = 0; i < size; i++)
    {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

//
// Json
//

JsonParser* json_parse_from_file (const char* path)
{
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        return nullptr;
    }
    return json_parse((char*)data, size);
}

JsonParser* json_parse_from_string (const char* str)
{
    if (!str)
    {
        return nullptr;
    }
    const usize size = cstr_length(str);
    char*       text = (char*)os_memory_alloc(size + 1);
    memory_copy(text, str, size);
    return json_parse(text, size);
}

void json_release (JsonParser* parser)
{
    if (!parser)
    {
        return;
    }
    os_memory_release(parser->text);
    os_memory_release(parser->tokens);
    os_memory_release(parser->subtree_end);
    os_memory_release(parser);
}

JsonValue json_get_root (const JsonParser* parser)
{
    return { .parser = parser, .index = 0 };
}

b8 json_is_valid (const JsonValue& value)
{
    return json_token(value) != nullptr;
}

// SDL's %g loses digits, so floats are formatted here: the value times a power of ten, rounded
// to nine digits, which tell every f32 apart. The power of ten is one correctly rounded double
// (two beyond 1e22), far finer than the f32's last digit.
usize json_format_f32 (char* dest, usize size, f32 value)
{
    u32 bits = 0;
    memory_copy(&bits, &value, sizeof(bits));
    if ((bits & 0x7f800000u) == 0x7f800000u)
    {
        return cstr_format(dest, size, "0");
    }
    const b8  negative  = (bits >> 31) != 0;
    const f64 magnitude = negative ? -(f64)value : (f64)value;
    if (magnitude == 0.0)
    {
        return cstr_format(dest, size, negative ? "-0" : "0");
    }
    i32 exponent = 0;
    while (magnitude >= json_pow10(exponent + 1))
    {
        exponent++;
    }
    while (magnitude < json_pow10(exponent))
    {
        exponent--;
    }
    u64 mantissa = (u64)(magnitude * json_pow10(8 - exponent) + 0.5);
    if (mantissa >= 1000000000ull)
    {
        mantissa = (mantissa + 5) / 10;
        exponent++;
    }

    char digits[10] = {};
    for (i32 i = 8; i >= 0; i--)
    {
        digits[i]  = (char)('0' + mantissa % 10);
        mantissa  /= 10;
    }
    i32 count = 9;
    while (count > 1 && digits[count - 1] == '0')
    {
        count--;
    }

    char text[32] = {};
    i32  at       = 0;
    if (negative)
    {
        text[at++] = '-';
    }
    if (exponent >= -5 && exponent <= 8)
    {
        if (exponent < 0)
        {
            text[at++] = '0';
            text[at++] = '.';
            for (i32 i = 0; i < -exponent - 1; i++)
            {
                text[at++] = '0';
            }
            for (i32 i = 0; i < count; i++)
            {
                text[at++] = digits[i];
            }
        }
        else
        {
            for (i32 i = 0; i <= exponent; i++)
            {
                text[at++] = i < count ? digits[i] : '0';
            }
            if (count > exponent + 1)
            {
                text[at++] = '.';
                for (i32 i = exponent + 1; i < count; i++)
                {
                    text[at++] = digits[i];
                }
            }
        }
        text[at] = 0;
        return cstr_format(dest, size, "%s", text);
    }
    text[at++] = digits[0];
    if (count > 1)
    {
        text[at++] = '.';
        for (i32 i = 1; i < count; i++)
        {
            text[at++] = digits[i];
        }
    }
    text[at] = 0;
    return cstr_format(dest, size, "%se%d", text, exponent);
}

b8 json_raw (const JsonValue& value, const char*& out_text, usize& out_length)
{
    const jsmntok_t* token = json_token(value);
    if (!token)
    {
        return false;
    }
    out_text   = value.parser->text + token->start;
    out_length = (usize)(token->end - token->start);
    return true;
}

b8 json_is_object (const JsonValue& value)
{
    const jsmntok_t* token = json_token(value);
    return token && token->type == JSMN_OBJECT;
}

b8 json_is_array (const JsonValue& value)
{
    const jsmntok_t* token = json_token(value);
    return token && token->type == JSMN_ARRAY;
}

b8 json_is_string (const JsonValue& value)
{
    const jsmntok_t* token = json_token(value);
    return token && token->type == JSMN_STRING;
}

b8 json_is_number (const JsonValue& value)
{
    const jsmntok_t* token = json_token(value);
    if (!token || token->type != JSMN_PRIMITIVE)
    {
        return false;
    }
    const char c = value.parser->text[token->start];
    return c == '-' || (c >= '0' && c <= '9');
}

b8 json_is_bool (const JsonValue& value)
{
    return json_token_equals(value, "true") || json_token_equals(value, "false");
}

b8 json_is_null (const JsonValue& value)
{
    return json_token_equals(value, "null");
}

b8 json_as_bool (const JsonValue& value, b8& out_value, b8 default_value)
{
    if (json_token_equals(value, "true"))
    {
        out_value = true;
        return true;
    }
    if (json_token_equals(value, "false"))
    {
        out_value = false;
        return true;
    }
    out_value = default_value;
    return false;
}

b8 json_as_float (const JsonValue& value, f32& out_value, f32 default_value)
{
    const char* text = nullptr;
    usize       size = 0;
    f64         parsed = 0.0;
    if (!json_is_number(value) || !json_token_text(value, text, size) || !json_parse_number(text, size, parsed))
    {
        out_value = default_value;
        return false;
    }
    out_value = (f32)parsed;
    return true;
}

b8 json_as_int (const JsonValue& value, i32& out_value, i32 default_value)
{
    const char* text = nullptr;
    usize       size = 0;
    f64         parsed = 0.0;
    if (!json_is_number(value) || !json_token_text(value, text, size) || !json_parse_number(text, size, parsed) || parsed < -2147483648.0 || parsed > 2147483647.0)
    {
        out_value = default_value;
        return false;
    }
    out_value = (i32)parsed;
    return true;
}

b8 json_as_uint (const JsonValue& value, u32& out_value, u32 default_value)
{
    const char* text = nullptr;
    usize       size = 0;
    f64         parsed = 0.0;
    if (!json_is_number(value) || !json_token_text(value, text, size) || !json_parse_number(text, size, parsed) || parsed < 0.0 || parsed > 4294967295.0)
    {
        out_value = default_value;
        return false;
    }
    out_value = (u32)parsed;
    return true;
}

b8 json_as_u64 (const JsonValue& value, u64& out_value, u64 default_value)
{
    const char* text  = nullptr;
    usize       size  = 0;
    u64         total = 0;
    b8          valid = json_is_number(value) && json_token_text(value, text, size) && size > 0;
    for (usize i = 0; valid && i < size; i++)
    {
        const u64 digit = (u64)(text[i] - '0');
        valid           = text[i] >= '0' && text[i] <= '9' && total <= (0xFFFFFFFFFFFFFFFFull - digit) / 10;
        total           = total * 10 + digit;
    }
    out_value = valid ? total : default_value;
    return valid;
}

b8 json_as_string (const JsonValue& value, char* out_value, usize* out_size, usize max_size, const char* default_value)
{
    ASSERT(out_value && max_size > 0);

    const char* text = nullptr;
    usize       size = 0;
    if (!json_is_string(value) || !json_token_text(value, text, size))
    {
        cstr_copy(out_value, max_size, default_value);
        if (out_size)
        {
            *out_size = cstr_length(out_value);
        }
        return false;
    }

    usize written   = 0;
    b8    truncated = false;
    for (usize i = 0; i < size && !truncated; i++)
    {
        char c = text[i];
        if (c == '\\' && i + 1 < size)
        {
            i++;
            switch (text[i])
            {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
            {
                u32 codepoint = 0;
                for (u32 k = 0; k < 4; k++)
                {
                    const u32 digit = i + 1 + k < size ? json_hex_digit(text[i + 1 + k]) : 0xFFFFFFFFu;
                    codepoint = (codepoint << 4) | (digit == 0xFFFFFFFFu ? 0 : digit);
                }
                i += 4;
                const usize room = max_size - 1 - written;
                const usize n    = json_write_utf8(out_value + written, room, codepoint);
                written += n;
                if (n == 0 || written >= max_size - 1)
                {
                    truncated = written < max_size - 1 ? false : (i + 1 < size);
                }
                continue;
            }
            default:
                break;
            }
        }

        if (written >= max_size - 1)
        {
            truncated = true;
            break;
        }
        out_value[written++] = c;
    }

    out_value[written] = 0;
    if (out_size)
    {
        *out_size = written;
    }
    return !truncated;
}

b8 json_find_bool (const JsonValue& object, const char* key, b8& out_value, b8 default_value)
{
    return json_as_bool(json_find(object, key), out_value, default_value);
}

b8 json_find_float (const JsonValue& object, const char* key, f32& out_value, f32 default_value)
{
    return json_as_float(json_find(object, key), out_value, default_value);
}

b8 json_find_int (const JsonValue& object, const char* key, i32& out_value, i32 default_value)
{
    return json_as_int(json_find(object, key), out_value, default_value);
}

b8 json_find_uint (const JsonValue& object, const char* key, u32& out_value, u32 default_value)
{
    return json_as_uint(json_find(object, key), out_value, default_value);
}

b8 json_find_string (const JsonValue& object, const char* key, char* out_value, usize* out_size, usize max_size, const char* default_value)
{
    return json_as_string(json_find(object, key), out_value, out_size, max_size, default_value);
}

b8 json_find_u64 (const JsonValue& object, const char* key, u64& out_value, u64 default_value)
{
    return json_as_u64(json_find(object, key), out_value, default_value);
}

JsonValue json_find (const JsonValue& object, const char* key)
{
    if (!json_is_object(object) || !key)
    {
        return {};
    }
    for (const JsonMember member : json_members(object))
    {
        if (json_token_equals(member.key, key))
        {
            return member.value;
        }
    }
    return {};
}

u32 json_length (const JsonValue& array)
{
    const jsmntok_t* token = json_token(array);
    if (!token || (token->type != JSMN_ARRAY && token->type != JSMN_OBJECT))
    {
        return 0;
    }
    return (u32)token->size;
}

JsonValue json_at (const JsonValue& array, u32 index)
{
    if (!json_is_array(array))
    {
        return {};
    }
    for (JsonArrayIterator it = array.begin(); it != array.end(); ++it)
    {
        if (index == 0)
        {
            return *it;
        }
        index--;
    }
    return {};
}

JsonMembers json_members (const JsonValue& object)
{
    if (!json_is_object(object))
    {
        return { .parser = nullptr, .object_index = 0 };
    }
    return { .parser = object.parser, .object_index = object.index };
}

JsonArrayIterator JsonValue::begin () const
{
    if (!json_is_array(*this))
    {
        return { .parser = nullptr, .index = 0, .remaining = 0 };
    }
    return { .parser = parser, .index = index + 1, .remaining = (u32)parser->tokens[index].size };
}

JsonArrayIterator JsonValue::end () const
{
    return { .parser = nullptr, .index = 0, .remaining = 0 };
}

JsonValue JsonValue::operator[] (u32 at) const
{
    return json_at(*this, at);
}

JsonValue JsonArrayIterator::operator* () const
{
    return { .parser = parser, .index = index };
}

JsonArrayIterator& JsonArrayIterator::operator++ ()
{
    index = parser->subtree_end[index];
    remaining--;
    return *this;
}

b8 JsonArrayIterator::operator!= (const JsonArrayIterator& other) const
{
    UNUSED(other);
    return remaining != 0;
}

JsonMemberIterator JsonMembers::begin () const
{
    if (!parser)
    {
        return { .parser = nullptr, .index = 0, .remaining = 0 };
    }
    return { .parser = parser, .index = object_index + 1, .remaining = (u32)parser->tokens[object_index].size };
}

JsonMemberIterator JsonMembers::end () const
{
    return { .parser = nullptr, .index = 0, .remaining = 0 };
}

JsonMember JsonMemberIterator::operator* () const
{
    return {
        .key   = { .parser = parser, .index = index },
        .value = { .parser = parser, .index = index + 1 },
    };
}

JsonMemberIterator& JsonMemberIterator::operator++ ()
{
    index = parser->subtree_end[index];
    remaining--;
    return *this;
}

b8 JsonMemberIterator::operator!= (const JsonMemberIterator& other) const
{
    UNUSED(other);
    return remaining != 0;
}
