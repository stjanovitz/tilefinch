/* The bounded JSON scanner shared by the provider resolvers and the web app
   manifest parser; see tilefinch/json_scan.h for its input policy. */

#include "tilefinch/json_scan.h"

#include <string.h>

#include "tilefinch/utf8_encode.h"

static bool json_scan_is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/* A byte that may follow a scalar: the end of its container or member. */
static bool json_scan_is_delimiter(char value)
{
    return value == ',' || value == ']' || value == '}'
        || json_scan_is_space(value);
}

void json_scan_space(JsonCursor *json)
{
    while (json->at < json->end && json_scan_is_space(*json->at)) json->at++;
}

static bool json_scan_hex4(const char *at, const char *end, unsigned *value)
{
    if ((size_t) (end - at) < 4u) return false;
    unsigned decoded = 0;
    for (size_t i = 0; i < 4u; i++) {
        char digit = at[i];
        unsigned nibble;
        if (digit >= '0' && digit <= '9') nibble = (unsigned) (digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            nibble = (unsigned) (digit - 'a') + 10u;
        else if (digit >= 'A' && digit <= 'F')
            nibble = (unsigned) (digit - 'A') + 10u;
        else return false;
        decoded = (decoded << 4) | nibble;
    }
    *value = decoded;
    return true;
}

/* The code point of a \u escape whose "\u" has been consumed: a surrogate
   pair is joined, and a surrogate without its other half (valid JSON, which
   JSON.parse accepts) becomes U+FFFD; an escape after a lone high surrogate
   is left for the caller to read on its own. */
static bool json_scan_escape_codepoint(JsonCursor *json, unsigned *codepoint)
{
    if (!json_scan_hex4(json->at, json->end, codepoint)) return false;
    json->at += 4;
    if (*codepoint >= 0xdc00u && *codepoint <= 0xdfffu) {
        *codepoint = 0xfffdu;
        return true;
    }
    if (*codepoint < 0xd800u || *codepoint > 0xdbffu) return true;
    unsigned low = 0;
    if ((size_t) (json->end - json->at) < 6u || json->at[0] != '\\'
        || json->at[1] != 'u'
        || !json_scan_hex4(json->at + 2, json->end, &low)
        || low < 0xdc00u || low > 0xdfffu) {
        *codepoint = 0xfffdu;
        return true;
    }
    json->at += 6;
    *codepoint = 0x10000u + ((*codepoint - 0xd800u) << 10) + (low - 0xdc00u);
    return true;
}

/* text[0, length) without an incomplete trailing UTF-8 sequence. */
static size_t json_scan_utf8_prefix(const char *text, size_t length)
{
    if (length == 0) return 0;
    size_t start = length - 1u;
    while (start > 0
           && ((unsigned char) text[start] & 0xc0u) == 0x80u) start--;
    unsigned char lead = (unsigned char) text[start];
    size_t expected = lead < 0x80u ? 1u
        : ((lead & 0xe0u) == 0xc0u ? 2u
        : ((lead & 0xf0u) == 0xe0u ? 3u
        : ((lead & 0xf8u) == 0xf0u ? 4u : 1u)));
    return length - start < expected ? start : length;
}

bool json_scan_string_copy(JsonCursor *json, char *output, size_t output_size,
                           unsigned flags, bool *truncated)
{
    json_scan_space(json);
    if (output == NULL || output_size == 0 || json->at >= json->end
        || *json->at != '"') return false;
    json->at++;
    bool truncate = (flags & JSON_SCAN_TRUNCATE) != 0;
    size_t used = 0;
    bool full = false;
    while (json->at < json->end) {
        unsigned char byte = (unsigned char) *json->at++;
        if (byte == '"') {
            if (full) used = json_scan_utf8_prefix(output, used);
            output[used] = '\0';
            if (truncated != NULL) *truncated = full;
            return true;
        }
        if (byte < 0x20u) return false;
        unsigned char bytes[4];
        size_t count = 1;
        bytes[0] = byte;
        if (byte == '\\') {
            if (json->at >= json->end) return false;
            char escape = *json->at++;
            switch (escape) {
            case '"': bytes[0] = '"'; break;
            case '\\': bytes[0] = '\\'; break;
            case '/': bytes[0] = '/'; break;
            case 'b': bytes[0] = '\b'; break;
            case 'f': bytes[0] = '\f'; break;
            case 'n': bytes[0] = '\n'; break;
            case 'r': bytes[0] = '\r'; break;
            case 't': bytes[0] = '\t'; break;
            case 'u': {
                unsigned codepoint = 0;
                /* U+0000 cannot live in the C string the caller gets:
                   refuse it rather than cut the string at the NUL. */
                if (!json_scan_escape_codepoint(json, &codepoint)
                    || codepoint == 0) return false;
                count = tilefinch_utf8_encode(codepoint, bytes);
                if (count == 0) return false;
                break;
            }
            default:
                return false;
            }
        }
        if (full) continue;
        if (count > output_size - 1u - used) {
            if (!truncate) return false;
            full = true;
            continue;
        }
        memcpy(output + used, bytes, count);
        used += count;
    }
    return false;
}

bool json_scan_string(JsonCursor *json, char *output, size_t output_size)
{
    return json_scan_string_copy(json, output, output_size, 0, NULL);
}

bool json_scan_skip_string(JsonCursor *json)
{
    json_scan_space(json);
    if (json->at >= json->end || *json->at != '"') return false;
    json->at++;
    while (json->at < json->end) {
        unsigned char byte = (unsigned char) *json->at++;
        if (byte == '"') return true;
        if (byte < 0x20u) return false;
        if (byte != '\\') continue;
        if (json->at >= json->end) return false;
        char escape = *json->at++;
        if (escape == 'u') {
            unsigned codepoint = 0;
            if (!json_scan_escape_codepoint(json, &codepoint)) return false;
        } else if (escape != '"' && escape != '\\' && escape != '/'
                   && escape != 'b' && escape != 'f' && escape != 'n'
                   && escape != 'r' && escape != 't') {
            return false;
        }
    }
    return false;
}

static bool json_scan_digits(JsonCursor *json)
{
    const char *start = json->at;
    while (json->at < json->end && *json->at >= '0' && *json->at <= '9')
        json->at++;
    return json->at != start;
}

/* A number in JSON's grammar, checked character by character. */
static bool json_scan_number(JsonCursor *json)
{
    if (json->at < json->end && *json->at == '-') json->at++;
    if (json->at >= json->end) return false;
    if (*json->at == '0') json->at++;
    else if (*json->at < '1' || *json->at > '9' || !json_scan_digits(json))
        return false;
    if (json->at < json->end && *json->at == '.') {
        json->at++;
        if (!json_scan_digits(json)) return false;
    }
    if (json->at < json->end && (*json->at == 'e' || *json->at == 'E')) {
        json->at++;
        if (json->at < json->end && (*json->at == '+' || *json->at == '-'))
            json->at++;
        if (!json_scan_digits(json)) return false;
    }
    return true;
}

static bool json_scan_literal(JsonCursor *json, const char *word,
                              size_t length)
{
    if ((size_t) (json->end - json->at) < length
        || memcmp(json->at, word, length) != 0) return false;
    json->at += length;
    return true;
}

static bool json_scan_scalar(JsonCursor *json)
{
    char first = *json->at;
    bool ok = first == 't' ? json_scan_literal(json, "true", 4u)
        : first == 'f' ? json_scan_literal(json, "false", 5u)
        : first == 'n' ? json_scan_literal(json, "null", 4u)
        : json_scan_number(json);
    return ok && (json->at == json->end || json_scan_is_delimiter(*json->at));
}

/* Containers are walked with an explicit bounded stack rather than by
   recursion, so the depth bound costs no C stack. */
#define JSON_SCAN_STACK_LIMIT 256u

bool json_scan_skip_value(JsonCursor *json, unsigned depth)
{
    char closers[JSON_SCAN_STACK_LIMIT];
    size_t open = 0;
    if (depth > JSON_SCAN_STACK_LIMIT) depth = JSON_SCAN_STACK_LIMIT;
    for (;;) {
        /* One value at nesting level `open`. */
        json_scan_space(json);
        if (open >= depth || json->at >= json->end) return false;
        char first = *json->at;
        if (first == '{' || first == '[') {
            json->at++;
            closers[open++] = first == '{' ? '}' : ']';
            json_scan_space(json);
            if (json->at < json->end && *json->at == closers[open - 1u]) {
                json->at++;
                open--;
            } else {
                if (closers[open - 1u] == '}') {
                    if (!json_scan_skip_string(json)) return false;
                    json_scan_space(json);
                    if (json->at >= json->end || *json->at++ != ':')
                        return false;
                }
                continue;
            }
        } else if (first == '"') {
            if (!json_scan_skip_string(json)) return false;
        } else if (!json_scan_scalar(json)) {
            return false;
        }
        /* The value is complete: close containers or move to the next
           member. */
        for (;;) {
            if (open == 0) return true;
            json_scan_space(json);
            if (json->at >= json->end) return false;
            char next = *json->at++;
            if (next == closers[open - 1u]) {
                open--;
                continue;
            }
            if (next != ',') return false;
            if (closers[open - 1u] == '}') {
                if (!json_scan_skip_string(json)) return false;
                json_scan_space(json);
                if (json->at >= json->end || *json->at++ != ':') return false;
            }
            break;
        }
    }
}

bool json_scan_value_span(JsonCursor *json, unsigned depth, JsonCursor *span)
{
    json_scan_space(json);
    const char *start = json->at;
    if (!json_scan_skip_value(json, depth)) return false;
    *span = (JsonCursor) {start, json->at};
    return true;
}

bool json_scan_member_key(JsonCursor *json, char *key, size_t key_size)
{
    if (!json_scan_string(json, key, key_size)) return false;
    json_scan_space(json);
    return json->at < json->end && *json->at++ == ':';
}

bool json_scan_member(JsonCursor object, const char *key, unsigned depth,
                      unsigned member_limit, JsonCursor *value)
{
    json_scan_space(&object);
    if (key == NULL || value == NULL || depth == 0
        || object.at >= object.end || *object.at != '{') return false;
    object.at++;
    for (unsigned members = 0; members < member_limit; members++) {
        json_scan_space(&object);
        if (object.at >= object.end || *object.at == '}') return false;
        char decoded[64];
        JsonCursor before = object;
        bool named = json_scan_member_key(&object, decoded, sizeof(decoded));
        if (!named) {
            /* A key too long to decode cannot be `key`; step over it. */
            JsonCursor skip = before;
            if (!json_scan_skip_string(&skip)) return false;
            json_scan_space(&skip);
            if (skip.at >= skip.end || *skip.at++ != ':') return false;
            object = skip;
            decoded[0] = '\0';
        }
        json_scan_space(&object);
        if (named && strcmp(decoded, key) == 0) {
            *value = object;
            return true;
        }
        if (!json_scan_skip_value(&object, depth - 1u)) return false;
        json_scan_space(&object);
        if (object.at >= object.end || *object.at++ != ',') return false;
    }
    return false;
}

bool json_scan_object_span(JsonCursor *json, unsigned depth, JsonCursor *span)
{
    json_scan_space(json);
    if (json->at >= json->end || *json->at != '{') return false;
    return json_scan_value_span(json, depth, span);
}
