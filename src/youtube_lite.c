#include "tilefinch/youtube_lite.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/request_context.h"
#include "tilefinch/url.h"
#include "tilefinch/youtube_resolver.h"

#define YOUTUBE_LITE_TITLE_LIMIT 256
#define YOUTUBE_LITE_CHANNEL_LIMIT 128
#define YOUTUBE_LITE_METADATA_LIMIT 80
/*
 * A search result's description snippet is retained only as much of it as a
 * 480x272 row can ever show. One 11px line inside the 290px text column of a
 * result card holds roughly 46 characters, so a 96 byte visible budget is
 * already generous; the extra bytes in the buffer only hold the marker the
 * clamp appends. Descriptions themselves are unbounded, so nothing longer is
 * ever copied.
 */
#define YOUTUBE_LITE_SNIPPET_LIMIT 128
#define YOUTUBE_LITE_SNIPPET_BUDGET 96
#define YOUTUBE_LITE_QUERY_LIMIT 256
#define YOUTUBE_LITE_DESCRIPTION_LIMIT 8192
#define YOUTUBE_LITE_DESCRIPTION_SUMMARY_LIMIT 512
#define YOUTUBE_LITE_COMMENT_TEXT_LIMIT 384
#define YOUTUBE_LITE_CONTINUATION_LIMIT 1024
#define YOUTUBE_LITE_LANGUAGE_LIMIT 8
#define YOUTUBE_LITE_IDENTITY_CACHE_KEY "youtube-mweb-identity-v1"
#define YOUTUBE_LITE_IDENTITY_CACHE_VERSION 1u
#define YOUTUBE_LITE_IDENTITY_CACHE_MAX_AGE_NS \
    (UINT64_C(30) * 60u * UINT64_C(1000000000))

#define YOUTUBE_LITE_MOBILE_UA \
    "Mozilla/5.0 (Linux; Android 10; K) " \
    "AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/136.0.0.0 Mobile Safari/537.36"

typedef struct {
    char id[YOUTUBE_VIDEO_ID_CAPACITY];
    char title[YOUTUBE_LITE_TITLE_LIMIT];
    char channel[YOUTUBE_LITE_CHANNEL_LIMIT];
    char duration[YOUTUBE_LITE_METADATA_LIMIT];
    char views[YOUTUBE_LITE_METADATA_LIMIT];
    char published[YOUTUBE_LITE_METADATA_LIMIT];
    char snippet[YOUTUBE_LITE_SNIPPET_LIMIT];
} YoutubeLiteVideo;

typedef struct {
    YoutubeLiteVideo video;
    char description[YOUTUBE_LITE_DESCRIPTION_LIMIT];
    char published[YOUTUBE_LITE_METADATA_LIMIT];
    char category[YOUTUBE_LITE_METADATA_LIMIT];
    bool localized_views;
} YoutubeLiteWatch;

static bool lite_header_value_safe(const char *value)
{
    if (value == NULL) return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (*at < 0x20u || *at >= 0x7fu) return false;
    }
    return true;
}

static bool lite_api_token_safe(const char *value, bool allow_dot)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (!isalnum(*at) && *at != '-' && *at != '_'
            && (!allow_dot || *at != '.')) return false;
    }
    return true;
}

static const char *lite_preferred_language(void)
{
    static const char *const supported[] = {
        "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko",
        "zh-TW", "zh-CN"
    };
    const char *language = tilefinch_platform_preferred_language();
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(language, supported[i]) == 0) return supported[i];
    }
    return "en";
}

typedef struct {
    char author[YOUTUBE_LITE_CHANNEL_LIMIT];
    char text[YOUTUBE_LITE_COMMENT_TEXT_LIMIT];
    char published[YOUTUBE_LITE_METADATA_LIMIT];
    char likes[YOUTUBE_LITE_METADATA_LIMIT];
} YoutubeLiteComment;

typedef struct {
    const char *start;
    const char *end;
} YoutubeLiteSpan;

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} YoutubeLiteHtml;

static void lite_error(char *error, size_t error_size,
                       const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool lite_host_is(const TilefinchUrl *url, const char *host)
{
    return strlen(host) == url->host_length
        && strncasecmp(url->value + url->host_offset,
                       host, url->host_length) == 0;
}

static bool lite_youtube_host(const TilefinchUrl *url)
{
    return lite_host_is(url, "youtube.com")
        || lite_host_is(url, "www.youtube.com")
        || lite_host_is(url, "m.youtube.com");
}

static bool lite_path_is(const TilefinchUrl *url, const char *path)
{
    size_t length = strlen(path);
    return url->path_length == length
        && memcmp(url->value + url->path_offset, path, length) == 0;
}

YoutubeLiteRoute youtube_lite_route(const char *url)
{
    if (url == NULL) return YOUTUBE_LITE_ROUTE_NONE;
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    if (youtube_watch_url_video_id(url, video_id))
        return YOUTUBE_LITE_ROUTE_WATCH;
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(url, &parsed)
        || parsed.scheme != TILEFINCH_URL_SCHEME_HTTPS
        || !lite_youtube_host(&parsed)) return YOUTUBE_LITE_ROUTE_NONE;
    if (parsed.path_length == 0 || lite_path_is(&parsed, "/"))
        return YOUTUBE_LITE_ROUTE_HOME;
    if (lite_path_is(&parsed, "/results"))
        return YOUTUBE_LITE_ROUTE_SEARCH;
    const char *path = parsed.value + parsed.path_offset;
    if ((parsed.path_length > 2u && path[0] == '/' && path[1] == '@')
        || (parsed.path_length > 9u
            && memcmp(path, "/channel/", 9) == 0)
        || (parsed.path_length > 3u && memcmp(path, "/c/", 3) == 0)
        || (parsed.path_length > 6u && memcmp(path, "/user/", 6) == 0))
        return YOUTUBE_LITE_ROUTE_CHANNEL;
    return YOUTUBE_LITE_ROUTE_NONE;
}

static const char *lite_find_bytes(const char *start, const char *end,
                                   const char *needle)
{
    size_t length = strlen(needle);
    if (length == 0) return start;
    if ((size_t) (end - start) < length) return NULL;
    for (const char *at = start; at <= end - (ptrdiff_t) length; at++) {
        if (*at == *needle && memcmp(at, needle, length) == 0) return at;
    }
    return NULL;
}

static bool lite_hex(char byte, unsigned *value)
{
    if (byte >= '0' && byte <= '9') *value = (unsigned) (byte - '0');
    else if (byte >= 'a' && byte <= 'f')
        *value = (unsigned) (byte - 'a' + 10);
    else if (byte >= 'A' && byte <= 'F')
        *value = (unsigned) (byte - 'A' + 10);
    else return false;
    return true;
}

static bool lite_append_utf8(char *output, size_t capacity,
                             size_t *length, unsigned codepoint)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7fu) {
        bytes[count++] = (unsigned char) codepoint;
    } else if (codepoint <= 0x7ffu) {
        bytes[count++] = (unsigned char) (0xc0u | (codepoint >> 6));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0xffffu
               && (codepoint < 0xd800u || codepoint > 0xdfffu)) {
        bytes[count++] = (unsigned char) (0xe0u | (codepoint >> 12));
        bytes[count++] =
            (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0x10ffffu) {
        bytes[count++] = (unsigned char) (0xf0u | (codepoint >> 18));
        bytes[count++] =
            (unsigned char) (0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[count++] =
            (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else {
        return false;
    }
    if (count > capacity - 1u - *length) return false;
    memcpy(output + *length, bytes, count);
    *length += count;
    return true;
}

static bool lite_decode_hex_escape(const char **at, const char *end,
                                   size_t digits, unsigned *value)
{
    if ((size_t) (end - *at) < digits) return false;
    unsigned decoded = 0;
    for (size_t i = 0; i < digits; i++) {
        unsigned nibble = 0;
        if (!lite_hex((*at)[i], &nibble)) return false;
        decoded = (decoded << 4) | nibble;
    }
    *at += digits;
    *value = decoded;
    return true;
}

/*
 * Mobile YouTube serializes ytInitialData as a JavaScript string containing
 * \xNN escapes. Decode only that bounded literal; no script is evaluated.
 */
static char *lite_initial_data(Budget *budget, const char *source,
                               size_t source_length, size_t *decoded_length)
{
    const char *end = source + source_length;
    const char *marker = lite_find_bytes(
        source, end, "ytInitialData = ");
    if (marker == NULL) marker = lite_find_bytes(
        source, end, "ytInitialData=");
    if (marker == NULL) return NULL;
    const char *at = strchr(marker, '=');
    if (at == NULL || at >= end) return NULL;
    at++;
    while (at < end && isspace((unsigned char) *at)) at++;
    if (at >= end || (*at != '\'' && *at != '"')) return NULL;
    char quote = *at++;
    char *decoded = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, source_length + 1u);
    if (decoded == NULL) return NULL;
    size_t used = 0;
    while (at < end && *at != quote) {
        unsigned char byte = (unsigned char) *at++;
        if (byte != '\\') {
            if (used >= source_length) goto fail;
            decoded[used++] = (char) byte;
            continue;
        }
        if (at >= end) goto fail;
        char escape = *at++;
        char simple = '\0';
        if (escape == 'n') simple = '\n';
        else if (escape == 'r') simple = '\r';
        else if (escape == 't') simple = '\t';
        else if (escape == 'b') simple = '\b';
        else if (escape == 'f') simple = '\f';
        else if (escape == '\n') continue;
        else if (escape != 'x' && escape != 'u') simple = escape;
        if (escape == 'x' || escape == 'u') {
            unsigned codepoint = 0;
            if (!lite_decode_hex_escape(
                    &at, end, escape == 'x' ? 2u : 4u, &codepoint)) goto fail;
            if (escape == 'u' && codepoint >= 0xd800u
                && codepoint <= 0xdbffu) {
                if ((size_t) (end - at) < 6u
                    || at[0] != '\\' || at[1] != 'u') goto fail;
                at += 2;
                unsigned low = 0;
                if (!lite_decode_hex_escape(&at, end, 4, &low)
                    || low < 0xdc00u || low > 0xdfffu) goto fail;
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10)
                          + (low - 0xdc00u);
            }
            if (!lite_append_utf8(
                    decoded, source_length + 1u, &used, codepoint)) goto fail;
        } else {
            if (used >= source_length) goto fail;
            decoded[used++] = simple;
        }
    }
    if (at >= end || *at != quote) goto fail;
    decoded[used] = '\0';
    *decoded_length = used;
    return decoded;
fail:
    budget_free(budget, decoded);
    return NULL;
}

static size_t lite_utf8_complete_prefix(const char *text, size_t length)
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

/*
 * Trims an already bounded snippet to its visible byte budget on a UTF-8
 * boundary and marks the cut. The caller's buffer must leave room for the
 * marker, which is what separates the budget from the buffer size.
 */
static void lite_clamp_snippet(char *text, size_t capacity, size_t budget)
{
    static const char marker[] = "\xe2\x80\xa6";
    size_t marker_length = sizeof(marker) - 1u;
    if (text == NULL || capacity <= budget + marker_length) return;
    size_t length = strlen(text);
    if (length <= budget) return;
    length = lite_utf8_complete_prefix(text, budget);
    while (length > 0 && (unsigned char) text[length - 1u] <= ' ') length--;
    memcpy(text + length, marker, marker_length + 1u);
}

static bool lite_json_string_bounded(const char *at, const char *end,
                                     char *output, size_t output_size,
                                     bool truncate)
{
    if (at >= end || *at++ != '"' || output_size == 0) return false;
    size_t used = 0;
    bool full = false;
    while (at < end) {
        unsigned char byte = (unsigned char) *at++;
        if (byte == '"') {
            if (full) used = lite_utf8_complete_prefix(output, used);
            output[used] = '\0';
            return true;
        }
        if (byte < 0x20u) return false;
        if (byte != '\\') {
            if (used >= output_size - 1u) {
                if (!truncate) return false;
                full = true;
            } else if (!full) {
                output[used++] = (char) byte;
            }
            continue;
        }
        if (at >= end) return false;
        char escape = *at++;
        char simple = '\0';
        if (escape == '"') simple = '"';
        else if (escape == '\\') simple = '\\';
        else if (escape == '/') simple = '/';
        else if (escape == 'b') simple = '\b';
        else if (escape == 'f') simple = '\f';
        else if (escape == 'n') simple = '\n';
        else if (escape == 'r') simple = '\r';
        else if (escape == 't') simple = '\t';
        else if (escape == 'u') {
            unsigned codepoint = 0;
            if (!lite_decode_hex_escape(&at, end, 4, &codepoint)) return false;
            if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                if ((size_t) (end - at) < 6u
                    || at[0] != '\\' || at[1] != 'u') return false;
                at += 2;
                unsigned low = 0;
                if (!lite_decode_hex_escape(&at, end, 4, &low)
                    || low < 0xdc00u || low > 0xdfffu) return false;
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10)
                          + (low - 0xdc00u);
            }
            if (!full && !lite_append_utf8(
                    output, output_size, &used, codepoint)) {
                if (!truncate) return false;
                full = true;
            }
            continue;
        } else {
            return false;
        }
        if (used >= output_size - 1u) {
            if (!truncate) return false;
            full = true;
        } else if (!full) {
            output[used++] = simple;
        }
    }
    return false;
}

static bool lite_json_string(const char *at, const char *end,
                             char *output, size_t output_size)
{
    return lite_json_string_bounded(
        at, end, output, output_size, false);
}

static bool lite_json_span(const char *at, const char *end,
                           YoutubeLiteSpan *span)
{
    while (at < end && isspace((unsigned char) *at)) at++;
    if (at >= end) return false;
    const char *start = at;
    if (*at == '"') {
        at++;
        while (at < end) {
            if (*at == '\\') {
                at += (size_t) (end - at) >= 2u ? 2 : 1;
                continue;
            }
            if (*at++ == '"') {
                *span = (YoutubeLiteSpan) {start, at};
                return true;
            }
        }
        return false;
    }
    if (*at != '{' && *at != '[') {
        while (at < end && *at != ',' && *at != '}' && *at != ']'
               && !isspace((unsigned char) *at)) at++;
        *span = (YoutubeLiteSpan) {start, at};
        return at > start;
    }
    char stack[48];
    size_t depth = 0;
    stack[depth++] = *at == '{' ? '}' : ']';
    bool in_string = false;
    bool escaped = false;
    for (at++; at < end; at++) {
        char byte = *at;
        if (in_string) {
            if (escaped) escaped = false;
            else if (byte == '\\') escaped = true;
            else if (byte == '"') in_string = false;
            continue;
        }
        if (byte == '"') {
            in_string = true;
        } else if (byte == '{' || byte == '[') {
            if (depth >= sizeof(stack)) return false;
            stack[depth++] = byte == '{' ? '}' : ']';
        } else if (byte == '}' || byte == ']') {
            if (depth == 0 || stack[depth - 1u] != byte) return false;
            depth--;
            if (depth == 0) {
                *span = (YoutubeLiteSpan) {start, at + 1};
                return true;
            }
        }
    }
    return false;
}

static bool lite_json_key(const YoutubeLiteSpan *scope, const char *key,
                          const char *after, YoutubeLiteSpan *value)
{
    size_t key_length = strlen(key);
    const char *at = after == NULL || after < scope->start
        ? scope->start : after;
    while (at < scope->end) {
        const char *found = lite_find_bytes(at, scope->end, key);
        if (found == NULL) return false;
        if (found > scope->start && found[-1] == '"'
            && found + key_length < scope->end
            && found[key_length] == '"') {
            size_t slashes = 0;
            const char *back = found - 1;
            while (back > scope->start && back[-1] == '\\') {
                slashes++;
                back--;
            }
            if ((slashes & 1u) == 0) {
                const char *colon = found + key_length + 1;
                while (colon < scope->end
                       && isspace((unsigned char) *colon)) colon++;
                if (colon < scope->end && *colon == ':'
                    && lite_json_span(colon + 1, scope->end, value)) {
                    return true;
                }
            }
        }
        at = found + key_length;
    }
    return false;
}

static bool lite_json_key_string(const YoutubeLiteSpan *scope,
                                 const char *key, char *output,
                                 size_t output_size)
{
    YoutubeLiteSpan value;
    return lite_json_key(scope, key, NULL, &value)
        && lite_json_string(value.start, value.end, output, output_size);
}

static bool lite_json_key_string_truncated(
    const YoutubeLiteSpan *scope, const char *key,
    char *output, size_t output_size)
{
    YoutubeLiteSpan value;
    return lite_json_key(scope, key, NULL, &value)
        && lite_json_string_bounded(
            value.start, value.end, output, output_size, true);
}

typedef enum {
    YOUTUBE_LITE_SCAN_PENDING = 0,
    YOUTUBE_LITE_SCAN_FOUND,
    YOUTUBE_LITE_SCAN_EXHAUSTED
} YoutubeLiteScanStatus;

static bool lite_json_key_string_at(
    const char *source, size_t source_length, size_t at,
    const char *key, char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    if (at >= source_length || source[at] != key[0]
        || key_length > source_length - at
        || memcmp(source + at, key, key_length) != 0
        || at == 0 || source[at - 1u] != '"'
        || at + key_length >= source_length
        || source[at + key_length] != '"') return false;
    size_t slashes = 0;
    for (size_t back = at - 1u;
         back != 0 && source[back - 1u] == '\\'; back--) slashes++;
    if ((slashes & 1u) != 0) return false;
    const char *colon = source + at + key_length + 1u;
    const char *end = source + source_length;
    while (colon < end && isspace((unsigned char) *colon)) colon++;
    if (colon >= end || *colon != ':') return false;
    colon++;
    while (colon < end && isspace((unsigned char) *colon)) colon++;
    return colon < end && *colon == '"'
        && lite_json_string(colon, end, output, output_size);
}

/* Large provider responses are retained only after the bounded transport has
   finished, but walking them is still device work. Keep every subsequent
   lexical search resumable too: one provider pump examines at most 16 KiB.
   The helper deliberately matches lite_json_key's quoted-key contract while
   accepting only bounded string values, so a hostile megabyte string cannot
   turn a successful key match into another unbounded unit. */
static YoutubeLiteScanStatus lite_json_key_string_scan_pump(
    const char *source, size_t source_length, size_t scan_limit,
    const char *key, size_t *offset, char *output, size_t output_size)
{
    enum { SCAN_BYTES_PER_PUMP = 16u * 1024u };
    if (source == NULL || key == NULL || offset == NULL || output == NULL
        || output_size == 0) return YOUTUBE_LITE_SCAN_EXHAUSTED;
    if (scan_limit > source_length) scan_limit = source_length;
    if (*offset > scan_limit) *offset = scan_limit;
    size_t key_length = strlen(key);
    size_t stop = *offset + SCAN_BYTES_PER_PUMP;
    if (stop < *offset || stop > scan_limit) stop = scan_limit;
    for (size_t at = *offset; at < stop; at++) {
        if (!lite_json_key_string_at(
                source, source_length, at, key, output, output_size)) {
            continue;
        }
        *offset = at + key_length;
        return YOUTUBE_LITE_SCAN_FOUND;
    }
    *offset = stop;
    return stop == scan_limit
        ? YOUTUBE_LITE_SCAN_EXHAUSTED : YOUTUBE_LITE_SCAN_PENDING;
}

static YoutubeLiteScanStatus lite_bytes_scan_pump(
    const char *source, size_t source_length, size_t scan_limit,
    const char *needle, size_t *offset, size_t *found_offset)
{
    enum { SCAN_BYTES_PER_PUMP = 16u * 1024u };
    if (source == NULL || needle == NULL || offset == NULL)
        return YOUTUBE_LITE_SCAN_EXHAUSTED;
    if (scan_limit > source_length) scan_limit = source_length;
    if (*offset > scan_limit) *offset = scan_limit;
    size_t needle_length = strlen(needle);
    size_t stop = *offset + SCAN_BYTES_PER_PUMP;
    if (stop < *offset || stop > scan_limit) stop = scan_limit;
    for (size_t at = *offset; at < stop; at++) {
        if (source[at] == needle[0] && needle_length <= source_length - at
            && memcmp(source + at, needle, needle_length) == 0) {
            if (found_offset != NULL) *found_offset = at;
            *offset = at + needle_length;
            return YOUTUBE_LITE_SCAN_FOUND;
        }
    }
    *offset = stop;
    return stop == scan_limit
        ? YOUTUBE_LITE_SCAN_EXHAUSTED : YOUTUBE_LITE_SCAN_PENDING;
}

static bool lite_text_runs(const YoutubeLiteSpan *scope, const char *key,
                           char *output, size_t output_size)
{
    YoutubeLiteSpan text;
    if (!lite_json_key(scope, key, NULL, &text) || output_size == 0)
        return false;
    if (lite_json_key_string_truncated(
            &text, "simpleText", output, output_size)) return true;
    YoutubeLiteSpan runs;
    if (!lite_json_key(&text, "runs", NULL, &runs)) return false;
    size_t used = 0;
    const char *after = runs.start;
    bool found = false;
    while (after < runs.end && used < output_size - 1u) {
        YoutubeLiteSpan value;
        if (!lite_json_key(&runs, "text", after, &value)) break;
        char part[YOUTUBE_LITE_COMMENT_TEXT_LIMIT] = {0};
        if (!lite_json_string_bounded(
                value.start, value.end, part, sizeof(part), true)) break;
        size_t length = strlen(part);
        if (length > output_size - 1u - used)
            length = output_size - 1u - used;
        length = lite_utf8_complete_prefix(part, length);
        memcpy(output + used, part, length);
        used += length;
        found = true;
        after = value.end;
    }
    output[used] = '\0';
    return found;
}

static bool lite_text_object(const YoutubeLiteSpan *scope, const char *key,
                             char *output, size_t output_size)
{
    YoutubeLiteSpan text;
    if (!lite_json_key(scope, key, NULL, &text)) return false;
    return lite_json_key_string(&text, "text", output, output_size)
        || lite_json_key_string(&text, "simpleText", output, output_size);
}

static bool lite_valid_video_id(const char *id)
{
    size_t length = strlen(id);
    if (length == 0 || length >= YOUTUBE_VIDEO_ID_CAPACITY) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) id[i];
        if (!isalnum(byte) && byte != '-' && byte != '_') return false;
    }
    return true;
}

static bool lite_video_duplicate(const YoutubeLiteVideo *videos,
                                 size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(videos[i].id, id) == 0) return true;
    }
    return false;
}

static bool lite_parse_video(const YoutubeLiteSpan *renderer,
                             bool modern, YoutubeLiteVideo *video)
{
    YoutubeLiteVideo parsed = {0};
    if (!lite_json_key_string(
            renderer, "videoId", parsed.id, sizeof(parsed.id))
        || !lite_valid_video_id(parsed.id)) return false;
    if (!lite_text_object(
            renderer, modern ? "headline" : "title",
            parsed.title, sizeof(parsed.title))) {
        snprintf(parsed.title, sizeof(parsed.title), "YouTube video");
    }
    (void) lite_text_object(
        renderer, "shortBylineText",
        parsed.channel, sizeof(parsed.channel));
    (void) lite_text_object(
        renderer, "lengthText",
        parsed.duration, sizeof(parsed.duration));
    (void) lite_text_object(
        renderer, "shortViewCountText",
        parsed.views, sizeof(parsed.views));
    (void) lite_text_object(
        renderer, "publishedTimeText",
        parsed.published, sizeof(parsed.published));
    /*
     * Search results carry a description snippet. The mobile page spells it
     * descriptionSnippet on its renderers; the desktop shape wraps the same
     * text in detailedMetadataSnippets. Either way it arrives as runs whose
     * query-match segments are split apart, so the runs are joined and then
     * clamped to what a result row can show.
     */
    YoutubeLiteSpan snippets;
    if (!lite_text_runs(
            renderer, "descriptionSnippet",
            parsed.snippet, sizeof(parsed.snippet))
        && lite_json_key(
               renderer, "detailedMetadataSnippets", NULL, &snippets)) {
        (void) lite_text_runs(
            &snippets, "snippetText",
            parsed.snippet, sizeof(parsed.snippet));
    }
    lite_clamp_snippet(
        parsed.snippet, sizeof(parsed.snippet),
        YOUTUBE_LITE_SNIPPET_BUDGET);
    *video = parsed;
    return true;
}

enum {
    YOUTUBE_LITE_RENDERER_VIDEO_WITH_CONTEXT = 0,
    YOUTUBE_LITE_RENDERER_VIDEO,
    YOUTUBE_LITE_RENDERER_GRID_VIDEO,
    YOUTUBE_LITE_RENDERER_COMPACT_VIDEO,
    YOUTUBE_LITE_RENDERER_DESCRIPTION_HEADER
};

typedef struct {
    const char *name;
    unsigned char length;
} YoutubeLiteRendererKey;

static bool lite_next_video_renderer(
    const YoutubeLiteSpan *scope, const char *after,
    bool include_description_header,
    YoutubeLiteSpan *renderer, size_t *kind)
{
    static const YoutubeLiteRendererKey keys[] = {
        {"videoWithContextRenderer",
         sizeof("videoWithContextRenderer") - 1u},
        {"videoRenderer", sizeof("videoRenderer") - 1u},
        {"gridVideoRenderer", sizeof("gridVideoRenderer") - 1u},
        {"compactVideoRenderer", sizeof("compactVideoRenderer") - 1u},
        {"videoDescriptionHeaderRenderer",
         sizeof("videoDescriptionHeaderRenderer") - 1u}
    };
    if (scope == NULL || renderer == NULL || kind == NULL) return false;
    const char *at = after == NULL || after < scope->start
        ? scope->start : after;
    while (at < scope->end) {
        const char *quote = memchr(at, '"', (size_t) (scope->end - at));
        if (quote == NULL) return false;
        size_t slashes = 0;
        for (const char *back = quote;
             back > scope->start && back[-1] == '\\'; back--) slashes++;
        if ((slashes & 1u) == 0) {
            size_t key_count = include_description_header
                ? sizeof(keys) / sizeof(keys[0])
                : YOUTUBE_LITE_RENDERER_DESCRIPTION_HEADER;
            for (size_t candidate = 0; candidate < key_count; candidate++) {
                size_t key_length = keys[candidate].length;
                const char *key_start = quote + 1;
                size_t remaining = (size_t) (scope->end - key_start);
                if (remaining <= key_length
                    || *key_start != keys[candidate].name[0]) {
                    continue;
                }
                const char *key_end = key_start + key_length;
                if (*key_end != '"'
                    || memcmp(
                           key_start, keys[candidate].name,
                           key_length) != 0) {
                    continue;
                }
                const char *colon = key_end + 1;
                while (colon < scope->end
                       && isspace((unsigned char) *colon)) colon++;
                if (colon < scope->end && *colon == ':'
                    && lite_json_span(colon + 1, scope->end, renderer)) {
                    *kind = candidate;
                    return true;
                }
            }
        }
        at = quote + 1;
    }
    return false;
}

static size_t lite_parse_videos(const char *json, size_t length,
                                YoutubeLiteVideo *videos, size_t capacity)
{
    YoutubeLiteSpan all = {json, json + length};
    size_t count = 0;
    const char *after = all.start;
    while (count < capacity && after < all.end) {
        YoutubeLiteSpan renderer = {0};
        size_t selected_kind = 0;
        if (!lite_next_video_renderer(
                &all, after, false, &renderer, &selected_kind)) break;
        after = renderer.end;
        YoutubeLiteVideo candidate;
        if (lite_parse_video(
                &renderer,
                selected_kind == YOUTUBE_LITE_RENDERER_VIDEO_WITH_CONTEXT,
                &candidate)
            && !lite_video_duplicate(videos, count, candidate.id)) {
            videos[count++] = candidate;
        }
    }
    return count;
}

static bool lite_query_value(const char *url, const char *name,
                             char *output, size_t output_size)
{
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(url, &parsed) || !parsed.has_query
        || output_size == 0) return false;
    const char *at = parsed.value + parsed.query_offset;
    const char *end = at + parsed.query_length;
    size_t name_length = strlen(name);
    while (at < end) {
        const char *field_end = memchr(at, '&', (size_t) (end - at));
        if (field_end == NULL) field_end = end;
        const char *equals = memchr(at, '=', (size_t) (field_end - at));
        if (equals != NULL && (size_t) (equals - at) == name_length
            && memcmp(at, name, name_length) == 0) {
            size_t used = 0;
            for (const char *value = equals + 1; value < field_end; value++) {
                unsigned char byte = (unsigned char) *value;
                if (byte == '+') byte = ' ';
                else if (byte == '%' && field_end - value >= 3) {
                    unsigned high = 0, low = 0;
                    if (lite_hex(value[1], &high)
                        && lite_hex(value[2], &low)) {
                        byte = (unsigned char) ((high << 4) | low);
                        value += 2;
                    }
                }
                if (byte == '\0' || used >= output_size - 1u) return false;
                output[used++] = (char) byte;
            }
            output[used] = '\0';
            return true;
        }
        at = field_end < end ? field_end + 1 : end;
    }
    return false;
}

static bool lite_url_encode(const char *input, char *output,
                            size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *at = (const unsigned char *) input;
         *at != '\0'; at++) {
        if (isalnum(*at) || *at == '-' || *at == '_' || *at == '.'
            || *at == '~') {
            if (used >= output_size - 1u) return false;
            output[used++] = (char) *at;
        } else {
            if (used > output_size - 4u) return false;
            output[used++] = '%';
            output[used++] = hex[*at >> 4];
            output[used++] = hex[*at & 0x0fu];
        }
    }
    output[used] = '\0';
    return true;
}

static bool lite_html_reserve(YoutubeLiteHtml *html, size_t addition)
{
    if (html->failed || addition > YOUTUBE_LITE_MAXIMUM_HTML_BYTES
        || html->length > YOUTUBE_LITE_MAXIMUM_HTML_BYTES - addition) {
        html->failed = true;
        return false;
    }
    size_t needed = html->length + addition + 1u;
    if (needed <= html->capacity) return true;
    const size_t maximum_capacity =
        YOUTUBE_LITE_MAXIMUM_HTML_BYTES + 1u;
    size_t capacity = html->capacity == 0 ? 4096u : html->capacity;
    while (capacity < needed && capacity < maximum_capacity) {
        capacity = capacity > maximum_capacity / 2u
            ? maximum_capacity : capacity * 2u;
    }
    char *grown = budget_realloc_category(
        html->budget, BUDGET_CATEGORY_RESOURCE, html->data, capacity);
    if (grown == NULL) {
        html->failed = true;
        return false;
    }
    html->data = grown;
    html->capacity = capacity;
    return true;
}

static bool lite_html_bytes(YoutubeLiteHtml *html,
                            const char *text, size_t length)
{
    if (!lite_html_reserve(html, length)) return false;
    memcpy(html->data + html->length, text, length);
    html->length += length;
    html->data[html->length] = '\0';
    return true;
}

static bool lite_html_text(YoutubeLiteHtml *html, const char *text)
{
    return lite_html_bytes(html, text, strlen(text));
}

static bool lite_html_format(YoutubeLiteHtml *html,
                             const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    return length >= 0 && (size_t) length < sizeof(buffer)
        && lite_html_bytes(html, buffer, (size_t) length);
}

static bool lite_html_escape_bytes(YoutubeLiteHtml *html, const char *text,
                                   size_t length)
{
    const unsigned char *at = (const unsigned char *) text;
    const unsigned char *end = at + length;
    for (; at < end; at++) {
        const char *escaped = NULL;
        if (*at == '&') escaped = "&amp;";
        else if (*at == '<') escaped = "&lt;";
        else if (*at == '>') escaped = "&gt;";
        else if (*at == '"') escaped = "&quot;";
        else if (*at == '\'') escaped = "&#39;";
        if (escaped != NULL) {
            if (!lite_html_text(html, escaped)) return false;
        } else if (!lite_html_bytes(html, (const char *) at, 1)) {
            return false;
        }
    }
    return true;
}

static bool lite_html_escape(YoutubeLiteHtml *html, const char *text)
{
    return lite_html_escape_bytes(html, text, strlen(text));
}

static bool lite_html_continuation_link(
    YoutubeLiteHtml *html, const char *base_url, const char *token,
    const char *label)
{
    char encoded[YOUTUBE_LITE_CONTINUATION_LIMIT * 3u] = {0};
    return base_url != NULL && token != NULL && token[0] != '\0'
        && label != NULL
        && lite_url_encode(token, encoded, sizeof(encoded))
        && lite_html_text(html, "<p><a class=more href=\"")
        && lite_html_escape(html, base_url)
        && lite_html_text(html, encoded)
        && lite_html_text(html, "\">")
        && lite_html_escape(html, label)
        && lite_html_text(html, "</a></p>");
}

static bool lite_html_header(YoutubeLiteHtml *html, const char *query,
                             const char *page_title,
                             bool search_autofocus,
                             bool compact_results)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>";
    static const char style[] =
        "</title><style>"
        "*{box-sizing:border-box}html{background:#0f0f0f;color:#f1f1f1}"
        "body{margin:0;font-family:Arial,sans-serif;font-size:14px}"
        "header{background:#0f0f0f;border-bottom:1px solid #333;padding:8px 10px}"
        ".top{display:flex;align-items:center;gap:10px;margin-bottom:8px}"
        ".brand{color:#fff;text-decoration:none;font-size:19px;font-weight:bold}"
        "form{display:flex;gap:6px;width:100%}"
        "input{flex:none;width:calc(100% - 82px);min-width:0;"
        "background:#181818;color:#fff;"
        "border:1px solid #666;border-radius:16px;height:32px;"
        "line-height:16px;padding:8px 11px 6px}"
        "button,.play{background:#eee;color:#111;border:0;border-radius:16px;"
        "text-decoration:none}.play{padding:7px 13px}"
        "button{flex:none;width:76px;height:32px;padding:0 13px;"
        "line-height:32px;text-align:center;font-family:inherit;font-size:14px}"
        "main{padding:10px;max-width:720px;margin:0 auto}"
        "h1{font-size:18px;margin:4px 0 12px}h2{font-size:16px}"
        ".hint,.meta,.empty{color:#aaa}.empty{padding:20px 0;line-height:1.5}";
    static const char content_style[] =
        ".result-row{display:flex;gap:7px;margin:0 0 12px;min-height:90px}"
        ".card{display:flex;flex:1;min-width:0;color:#fff;text-decoration:none;"
        "gap:10px;min-height:90px}.details{display:block;position:relative;"
        "flex:none;width:54px;color:#ddd;text-decoration:none;"
        "border:1px solid #606060;border-radius:8px;background:#202020}"
        ".details-icon{display:block;position:absolute;left:50%;top:29px;"
        "margin-left:-15px;width:30px;height:30px;border:1px solid #999;"
        "border-radius:15px;background:#303030}.details-i-dot,.details-i-stem{"
        "display:block;position:absolute;left:13px;width:3px;background:#eee}"
        ".details-i-dot{top:6px;height:3px;border-radius:2px}"
        ".details-i-stem{top:12px;height:11px;border-radius:2px}"
        ".card:focus,.details:focus{outline:3px solid #fff;outline-offset:1px}"
        ".details:focus{background:#303030}.details:focus .details-icon{"
        "background:#eee;border-color:#fff}.details:focus .details-i-dot,"
        ".details:focus .details-i-stem{background:#111}"
        ".thumb-wrap{position:relative;width:160px;"
        "height:90px;flex:none}.thumb{display:block;width:100%;height:100%;"
        "background:#292929;border-radius:7px;object-fit:cover}"
        ".info{min-width:0}.title{display:block;font-size:15px;line-height:1.25;"
        "font-weight:bold;margin:1px 0 5px}.meta{font-size:12px;"
        "display:block;line-height:1.35;margin-top:2px}.duration{position:"
        "absolute;right:5px;bottom:4px;color:#eee;background:rgba(0,0,0,.78);"
        "font-size:11px;line-height:1.1;padding:2px 4px;border-radius:3px}"
        /* The snippet fills the space a 90px thumbnail leaves beside three
           metadata lines, so it is held to a single clipped line. The right
           padding is the ellipsis marker's own width: the marker is placed
           past the content edge when nothing more fits, and without that
           reserve it would be sliced by the clip and collide with the
           scrollbar. */
        ".snippet{display:block;font-size:11px;line-height:1.3;color:#8f8f8f;"
        "margin-top:3px;padding-right:13px;white-space:nowrap;overflow:clip;"
        "text-overflow:ellipsis}"
        ".hero{background:#181818;border-radius:10px;padding:18px;"
        "margin-top:8px}.watch-hero{display:flex;gap:12px;"
        "align-items:flex-start;margin-bottom:10px}.watch-target{display:block;"
        "position:relative;overflow:hidden;background:#222;"
        "flex:0 0 48%;max-width:230px;border-radius:8px;"
        "color:#fff;text-decoration:none}.watch-target:focus{outline:3px solid "
        "#fff;outline-offset:2px}.watch{display:block;width:100%;min-width:0;"
        "aspect-ratio:16 / 9;object-fit:cover;background:#222;"
        /* The badge is a painted box, not an inline <svg>: an inline SVG only
           reaches the screen through the image pipeline (discovery, NanoSVG
           parse, rasterise, decoded-image budget), so it vanished on any
           commit where images were capped or off while the glyph above it
           still drew.  Same 50x38 rectangle at radius 12 in #25282b/76% the
           native overlay paints, centred on the same point the 52x40 box
           reserved: left 50%, top 64px. */
        "border-radius:8px}.play-icon{display:block;position:absolute;"
        "left:50%;top:64px;width:50px;height:38px;margin-left:-25px;"
        "margin-top:-19px;border-radius:12px;"
        "background:rgba(37,40,43,.76);z-index:1}.play-glyph{display:block;"
        "position:absolute;left:50%;top:64px;width:52px;height:40px;"
        "margin-left:-26px;margin-top:-20px;line-height:40px;text-align:center;"
        "color:#e4e6e8;font-family:Arial,sans-serif;font-size:22px;"
        "font-weight:normal;z-index:2}"
        ".watch-copy{flex:1;min-width:0}.watch-copy h1{font-size:17px;"
        "line-height:1.25;margin:0 0 8px}.watch-meta{line-height:1.45;"
        "margin:0 0 8px}.play-label{display:inline-block;background:#eee;"
        "color:#111;border-radius:16px;padding:7px 13px;font-weight:bold}"
        ".description{background:#1d1d1d;border-radius:8px;padding:10px;"
        "line-height:1.45;color:#ddd}.comments-link{display:inline-block;"
        "color:#fff;border:1px solid #666;border-radius:16px;padding:7px 13px;"
        "text-decoration:none}.more{display:block;background:#272727;color:#fff;"
        "border-radius:18px;padding:10px;text-align:center;text-decoration:none;"
        "margin:12px 0}.channel{color:#aaa;text-decoration:none}"
        ".comment{border-top:1px solid #333;"
        "padding:10px 0;line-height:1.4}.comment-author{font-weight:bold}"
        ".comment-meta{color:#aaa;font-size:12px;margin-left:6px}"
        ".comment-text{margin:5px 0}.comment-likes{color:#aaa;font-size:12px}"
        "@media(max-width:380px){.thumb-wrap{width:136px;height:77px}"
        ".card{min-height:77px}.title{font-size:14px}"
        ".watch-hero{display:block}.watch-target{max-width:none}"
        ".watch-copy{margin-top:10px}}";
    static const char compact_style[] =
        ".compact-results main{padding-top:7px}"
        ".compact-results h1{font-size:15px;margin:3px 0 7px}"
        ".compact-results .result-row{min-height:60px;margin-bottom:6px;gap:6px}"
        ".compact-results .card{min-height:60px;gap:8px}"
        ".compact-results .details{width:46px}"
        ".compact-results .details-icon{top:17px;width:24px;height:24px;"
        "margin-left:-12px;border-radius:12px}"
        ".compact-results .details-i-dot,.compact-results .details-i-stem{"
        "left:10px;width:3px}.compact-results .details-i-dot{top:5px}"
        ".compact-results .details-i-stem{top:10px;height:9px}"
        ".compact-results .thumb-wrap{width:106px;height:60px}"
        ".compact-results .thumb{border-radius:5px}"
        ".compact-results .title{font-size:13px;line-height:1.2;"
        "margin:0 0 2px;max-height:32px;overflow:clip}"
        ".compact-results .meta{font-size:11px;line-height:1.2;margin-top:1px}"
        ".compact-results .snippet{display:none}";
    static const char style_end[] = "</style></head><body";
    static const char header[] =
        "><header><div class=top>"
        "<a class=brand href=\"https://www.youtube.com/\">"
        "YouTube</a></div>"
        "<form action=\"https://www.youtube.com/results\" method=get>"
        "<input id=yt-search name=search_query type=search"
        " aria-label=\"Search YouTube\""
        " placeholder=\"Search YouTube\" value=\"";
    return lite_html_text(html, prefix)
        && lite_html_escape(html, page_title)
        && lite_html_text(html, style)
        && lite_html_text(html, content_style)
        && (!compact_results || lite_html_text(html, compact_style))
        && lite_html_text(html, style_end)
        && (!compact_results
            || lite_html_text(html, " class=compact-results"))
        && lite_html_text(html, header)
        && lite_html_escape(html, query)
        /* Close the value attribute before autofocus. A quote glued to the
           attribute name parses as part of the name (`autofocus"`), so the
           engine's autofocus lookup would never see it. */
        && lite_html_text(html, "\"")
        && (!search_autofocus || lite_html_text(html, " autofocus"))
        && lite_html_text(html, ">")
        && lite_html_text(
            html, "<button type=submit>Search</button></form></header><main>");
}

static bool lite_html_video(YoutubeLiteHtml *html,
                            const YoutubeLiteVideo *video,
                            bool autofocus, const char *search_query)
{
    char encoded_query[YOUTUBE_LITE_QUERY_LIMIT * 3u] = {0};
    bool retain_query = search_query != NULL && search_query[0] != '\0';
    if (retain_query
        && !lite_url_encode(
            search_query, encoded_query, sizeof(encoded_query))) {
        return false;
    }
    if (!lite_html_format(
            html, "<div class=result-row><a class=card%s "
                  "data-tilefinch-provider-media href=\""
                  "https://www.youtube.com/watch?v=%s",
            autofocus ? " autofocus" : "", video->id)
        || (retain_query
            && (!lite_html_text(html, "&amp;search_query=")
                || !lite_html_text(html, encoded_query)))
        || !lite_html_format(
            html, "\"><span class=thumb-wrap><img class=thumb "
                  "src=\"https://i.ytimg.com/vi/%s/mqdefault.jpg\" alt=\"\">",
            video->id)
        || (video->duration[0] != '\0'
            && (!lite_html_text(html, "<span class=duration>")
                || !lite_html_escape(html, video->duration)
                || !lite_html_text(html, "</span>")))
        || !lite_html_text(
            html, "</span><span class=info><span class=title>")
        || !lite_html_escape(html, video->title)
        || !lite_html_text(html, "</span>")) return false;
    if (video->channel[0] != '\0'
        && !lite_html_text(html, "<span class=meta>")) return false;
    if (video->channel[0] != '\0') {
        /* A result is one semantic link. Nesting a channel anchor inside the
           card anchor is invalid HTML; parser repair split each result into
           several focus targets and made spatial navigation and scrolling do
           redundant work. */
        if (!lite_html_text(html, "<span class=channel>")
            || !lite_html_escape(html, video->channel)
            || !lite_html_text(html, "</span>")) {
            return false;
        }
        if (!lite_html_text(html, "</span>")) return false;
    }
    if (video->views[0] != '\0' || video->published[0] != '\0') {
        if (!lite_html_text(html, "<span class=meta>")
            || !lite_html_escape(html, video->views)
            || (video->views[0] != '\0' && video->published[0] != '\0'
                && !lite_html_text(html, " &middot; "))
            || !lite_html_escape(html, video->published)
            || !lite_html_text(html, "</span>")) return false;
    }
    if (video->snippet[0] != '\0'
        && (!lite_html_text(html, "<span class=snippet>")
            || !lite_html_escape(html, video->snippet)
            || !lite_html_text(html, "</span>"))) return false;
    if (!lite_html_text(
            html, "</span></a><a class=details href=\""
                  "https://www.youtube.com/watch?v=")
        || !lite_html_escape(html, video->id)
        || (retain_query
            && (!lite_html_text(html, "&amp;search_query=")
                || !lite_html_text(html, encoded_query)))
        || !lite_html_text(
            html, "\" aria-label=\"Open video details\">"
                  "<span class=details-icon aria-hidden=true>"
                  "<span class=details-i-dot></span>"
                  "<span class=details-i-stem></span></span></a></div>")) {
        return false;
    }
    return true;
}

static bool lite_html_multiline(YoutubeLiteHtml *html, const char *text)
{
    const char *at = text;
    const char *line = text;
    while (*at != '\0') {
        if (*at == '\r' || *at == '\n') {
            if (!lite_html_escape_bytes(
                    html, line, (size_t) (at - line)))
                return false;
            if (*at == '\r' && at[1] == '\n') at++;
            if (!lite_html_text(html, "<br>")) return false;
            line = at + 1;
        }
        at++;
    }
    if (at != line
        && !lite_html_escape_bytes(
            html, line, (size_t) (at - line))) return false;
    return true;
}

static void lite_watch_details(
    const YoutubeLiteSpan *details, YoutubeLiteWatch *watch)
{
    (void) lite_json_key_string(
        details, "title", watch->video.title, sizeof(watch->video.title));
    (void) lite_json_key_string(
        details, "author", watch->video.channel,
        sizeof(watch->video.channel));
    if (!watch->localized_views) {
        (void) lite_json_key_string(
            details, "viewCount", watch->video.views,
            sizeof(watch->video.views));
    }
    (void) lite_json_key_string_truncated(
        details, "shortDescription", watch->description,
        sizeof(watch->description));
    char seconds[32] = {0};
    if (lite_json_key_string(
            details, "lengthSeconds", seconds, sizeof(seconds))) {
        unsigned long total = strtoul(seconds, NULL, 10);
        snprintf(watch->video.duration, sizeof(watch->video.duration),
                 "%lu:%02lu", total / 60u, total % 60u);
    }
}

static bool lite_watch_numeric_date(
    const char *source, char *output, size_t output_size)
{
    if (source == NULL || output == NULL || output_size == 0
        || strlen(source) < 10u
        || source[4] != '-' || source[7] != '-') return false;
    for (size_t i = 0; i < 10u; i++) {
        if (i != 4u && i != 7u
            && !isdigit((unsigned char) source[i])) return false;
    }
    if (source[10] != '\0' && source[10] != 'T' && source[10] != 't')
        return false;
    unsigned year = (unsigned) (source[0] - '0') * 1000u
                  + (unsigned) (source[1] - '0') * 100u
                  + (unsigned) (source[2] - '0') * 10u
                  + (unsigned) (source[3] - '0');
    unsigned month = (unsigned) (source[5] - '0') * 10u
                   + (unsigned) (source[6] - '0');
    unsigned day = (unsigned) (source[8] - '0') * 10u
                 + (unsigned) (source[9] - '0');
    static const unsigned char days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year == 0 || month == 0 || month > 12u) return false;
    unsigned maximum_day = days_per_month[month - 1u];
    if (month == 2u
        && (year % 400u == 0u
            || (year % 4u == 0u && year % 100u != 0u))) maximum_day = 29u;
    if (day == 0 || day > maximum_day) return false;
    int written = 0;
    switch (tilefinch_platform_preferred_date_format()) {
    case TILEFINCH_DATE_FORMAT_MONTH_DAY_YEAR:
        written = snprintf(
            output, output_size, "%02u/%02u/%04u", month, day, year);
        break;
    case TILEFINCH_DATE_FORMAT_DAY_MONTH_YEAR:
        written = snprintf(
            output, output_size, "%02u/%02u/%04u", day, month, year);
        break;
    case TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY:
    default:
        written = snprintf(
            output, output_size, "%04u/%02u/%02u", year, month, day);
        break;
    }
    return written > 0 && (size_t) written < output_size;
}

static void lite_watch_microformat(
    const YoutubeLiteSpan *microformat, YoutubeLiteWatch *watch)
{
    char machine_date[YOUTUBE_LITE_METADATA_LIMIT] = {0};
    (void) lite_json_key_string(
        microformat, "publishDate", machine_date, sizeof(machine_date));
    if (machine_date[0] == '\0')
        (void) lite_json_key_string(
            microformat, "uploadDate", machine_date, sizeof(machine_date));
    if (machine_date[0] != '\0' && watch->published[0] == '\0') {
        if (!lite_watch_numeric_date(
                machine_date, watch->published,
                sizeof(watch->published))) {
            snprintf(
                watch->published, sizeof(watch->published), "%s",
                machine_date);
        }
    }
    (void) lite_json_key_string(
        microformat, "category", watch->category,
        sizeof(watch->category));
}

static void lite_watch_description_header(
    const YoutubeLiteSpan *renderer, YoutubeLiteWatch *watch)
{
    char display[YOUTUBE_LITE_METADATA_LIMIT] = {0};
    if (lite_text_runs(
            renderer, "publishDate", display, sizeof(display))) {
        snprintf(
            watch->published, sizeof(watch->published), "%s", display);
    }
    display[0] = '\0';
    if (lite_text_runs(renderer, "views", display, sizeof(display))) {
        snprintf(
            watch->video.views, sizeof(watch->video.views), "%s", display);
        watch->localized_views = true;
    }
}

static bool lite_watch_description_header_from(
    const char *source, size_t source_length, YoutubeLiteWatch *watch)
{
    if (source == NULL || source_length == 0) return false;
    YoutubeLiteSpan scope = {source, source + source_length};
    YoutubeLiteSpan renderer = {0};
    if (!lite_json_key(
            &scope, "videoDescriptionHeaderRenderer", NULL, &renderer)) {
        return false;
    }
    lite_watch_description_header(&renderer, watch);
    return true;
}

static bool lite_watch_metadata(const char *source, size_t source_length,
                                const char *decoded, size_t decoded_length,
                                YoutubeLiteWatch *watch)
{
    YoutubeLiteSpan source_span = {source, source + source_length};
    const char *after = source_span.start;
    YoutubeLiteSpan player = {0};
    YoutubeLiteSpan details = {0};
    unsigned candidates = 0;
    while (candidates < 4u) {
        const char *marker = lite_find_bytes(
            after, source_span.end, "ytInitialPlayerResponse");
        if (marker == NULL) break;
        candidates++;
        const char *marker_end =
            marker + strlen("ytInitialPlayerResponse");
        size_t assignment_window =
            (size_t) (source_span.end - marker_end);
        if (assignment_window > 128u) assignment_window = 128u;
        const char *equals = memchr(
            marker_end, '=', assignment_window);
        if (equals == NULL) {
            after = marker_end;
            continue;
        }
        YoutubeLiteSpan candidate = {0};
        if (lite_json_span(equals + 1, source_span.end, &candidate)
            && candidate.start < candidate.end
            && *candidate.start == '{'
            && lite_json_key(
                &candidate, "videoDetails", NULL, &details)) {
            player = candidate;
            break;
        }
        after = candidate.end != NULL && candidate.end > marker_end
            ? candidate.end : marker_end;
        if (after >= source_span.end) break;
    }
    if (player.start == NULL) return false;
    lite_watch_details(&details, watch);
    YoutubeLiteSpan microformat;
    if (lite_json_key(&player, "playerMicroformatRenderer", NULL,
                      &microformat)) {
        lite_watch_microformat(&microformat, watch);
    }
    /* The machine-readable microformat is intentionally locale-neutral.
       Prefer the renderer YouTube already localized for the request's `hl`;
       decoded initial data is the normal carrier, with raw source as a
       shape-churn fallback. */
    if (!lite_watch_description_header_from(
            decoded, decoded_length, watch)) {
        (void) lite_watch_description_header_from(
            source, source_length, watch);
    }
    return watch->video.title[0] != '\0';
}

static bool lite_parse_comment_renderer(
    const YoutubeLiteSpan *renderer, YoutubeLiteComment *comment)
{
    YoutubeLiteComment parsed = {0};
    (void) lite_text_runs(
        renderer, "authorText", parsed.author,
        sizeof(parsed.author));
    (void) lite_text_runs(
        renderer, "contentText", parsed.text,
        sizeof(parsed.text));
    (void) lite_text_runs(
        renderer, "publishedTimeText", parsed.published,
        sizeof(parsed.published));
    (void) lite_text_runs(
        renderer, "voteCount", parsed.likes,
        sizeof(parsed.likes));
    if (parsed.text[0] == '\0') return false;
    *comment = parsed;
    return true;
}

static size_t lite_parse_comments(
    const char *json, size_t length, YoutubeLiteComment *comments,
    size_t capacity, char *count, size_t count_size)
{
    if (json == NULL || length == 0 || comments == NULL || capacity == 0)
        return 0;
    YoutubeLiteSpan all = {json, json + length};
    YoutubeLiteSpan header;
    if (count != NULL && count_size != 0
        && lite_json_key(&all, "commentsHeaderRenderer", NULL, &header)) {
        if (!lite_text_runs(
                &header, "commentsCount", count, count_size))
            (void) lite_text_runs(
                &header, "countText", count, count_size);
    }
    size_t result_count = 0;
    const char *after = all.start;
    while (result_count < capacity && after < all.end) {
        YoutubeLiteSpan renderer;
        if (!lite_json_key(
                &all, "commentRenderer", after, &renderer)) break;
        after = renderer.end;
        YoutubeLiteComment parsed;
        if (lite_parse_comment_renderer(&renderer, &parsed))
            comments[result_count++] = parsed;
    }
    return result_count;
}

static bool lite_next_continuation(
    const char *json, size_t length,
    char token[YOUTUBE_LITE_CONTINUATION_LIMIT])
{
    if (json == NULL || length == 0 || token == NULL) return false;
    YoutubeLiteSpan all = {json, json + length};
    const char *after = all.start;
    bool found = false;
    token[0] = '\0';
    while (after < all.end) {
        YoutubeLiteSpan item;
        if (!lite_json_key(
                &all, "continuationItemRenderer", after, &item))
            break;
        YoutubeLiteSpan command;
        YoutubeLiteSpan value;
        char candidate[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
        if (lite_json_key(&item, "continuationCommand", NULL, &command)
            && lite_json_key(&command, "token", NULL, &value)
            && lite_json_string(
                value.start, value.end, candidate, sizeof(candidate))) {
            snprintf(token, YOUTUBE_LITE_CONTINUATION_LIMIT, "%s",
                     candidate);
            found = true;
        }
        after = item.end;
    }
    return found;
}

static bool lite_html_comment(YoutubeLiteHtml *html,
                              const YoutubeLiteComment *comment)
{
    if (!lite_html_text(html, "<article class=comment><div>")
        || (comment->author[0] != '\0'
            && (!lite_html_text(html, "<span class=comment-author>")
                || !lite_html_escape(html, comment->author)
                || !lite_html_text(html, "</span>")))
        || (comment->published[0] != '\0'
            && (!lite_html_text(html, "<span class=comment-meta>")
                || !lite_html_escape(html, comment->published)
                || !lite_html_text(html, "</span>")))
        || !lite_html_text(html, "</div><p class=comment-text>")
        || !lite_html_multiline(html, comment->text)
        || !lite_html_text(html, "</p>")) return false;
    if (comment->likes[0] != '\0'
        && (!lite_html_text(html, "<div class=comment-likes>")
            || !lite_html_escape(html, comment->likes)
            || !lite_html_text(html, " likes</div>"))) return false;
    return lite_html_text(html, "</article>");
}

static bool lite_comments_view_requested(const char *url)
{
    char view[32] = {0};
    return lite_query_value(url, "tilefinch_view", view, sizeof(view))
        && strcmp(view, "comments") == 0;
}

static bool lite_description_view_requested(const char *url)
{
    char view[32] = {0};
    return lite_query_value(url, "tilefinch_view", view, sizeof(view))
        && strcmp(view, "description") == 0;
}

static bool lite_compact_results_requested(const char *url)
{
    char layout[16] = {0};
    return lite_query_value(
               url, "tilefinch_layout", layout, sizeof(layout))
        && strcmp(layout, "compact") == 0;
}

static bool lite_html_search_intro(YoutubeLiteHtml *html, const char *query,
                                   size_t result_count)
{
    bool ok = lite_html_text(html, "<h1>Search results");
    if (ok && query[0] != '\0')
        ok = lite_html_text(html, " for &ldquo;")
            && lite_html_escape(html, query)
            && lite_html_text(html, "&rdquo;");
    ok = ok && lite_html_text(html, "</h1>");
    if (ok && result_count == 0)
        ok = lite_html_text(
            html, "<p class=empty>No playable video results were "
                  "present in the bounded public response. Try another "
                  "search.</p>");
    return ok;
}

static bool lite_search_continuation_base(
    const char *query, char *base, size_t base_size)
{
    char encoded_query[YOUTUBE_LITE_QUERY_LIMIT * 3u] = {0};
    if (!lite_url_encode(query, encoded_query, sizeof(encoded_query)))
        return false;
    int written = snprintf(
        base, base_size,
        "https://www.youtube.com/results?search_query=%s"
        "&tilefinch_token=",
        encoded_query);
    return written >= 0 && (size_t) written < base_size;
}

static bool lite_html_description_summary(YoutubeLiteHtml *html,
                                          const char *description)
{
    size_t length = strlen(description);
    if (length <= YOUTUBE_LITE_DESCRIPTION_SUMMARY_LIMIT)
        return lite_html_multiline(html, description);
    length = YOUTUBE_LITE_DESCRIPTION_SUMMARY_LIMIT;
    while (length != 0
           && ((unsigned char) description[length] & 0xc0u) == 0x80u)
        length--;
    char summary[YOUTUBE_LITE_DESCRIPTION_SUMMARY_LIMIT + 1];
    memcpy(summary, description, length);
    summary[length] = '\0';
    return lite_html_multiline(html, summary)
        && lite_html_text(html, "&hellip;");
}

static bool lite_html_watch_intro(
    YoutubeLiteHtml *html, const YoutubeLiteWatch *watch,
    bool autofocus)
{
    bool ok = lite_html_format(
        html, "<section class=watch-hero><a class=watch-target%s "
              "href=\"https://www.youtube.com/watch?v=%s\" "
              "aria-label=\"Play video\">"
              "<span class=play-icon aria-hidden=true></span>"
              "<span class=play-glyph aria-hidden=true>&#9658;</span>"
              "<img class=watch src=\"https://i.ytimg.com/vi/%s/"
              "hqdefault.jpg\" alt=\"\"></a>"
              "<div class=watch-copy><h1>",
        autofocus ? " autofocus" : "", watch->video.id,
        watch->video.id)
        && lite_html_escape(html, watch->video.title)
        && lite_html_text(html, "</h1><p class=watch-meta>");
    if (ok && watch->video.channel[0] != '\0')
        ok = lite_html_escape(html, watch->video.channel);
    if (ok && watch->video.views[0] != '\0') {
        if (watch->video.channel[0] != '\0')
            ok = lite_html_text(html, "<br>");
        ok = ok && lite_html_escape(html, watch->video.views)
            && (watch->localized_views
                || lite_html_text(html, " views"));
    }
    if (ok && watch->video.duration[0] != '\0')
        ok = lite_html_text(html, " &middot; ")
            && lite_html_escape(html, watch->video.duration);
    if (ok && watch->published[0] != '\0')
        ok = lite_html_text(html, "<br>Published ")
            && lite_html_escape(html, watch->published);
    if (ok && watch->category[0] != '\0')
        ok = lite_html_text(html, "<br>Category: ")
            && lite_html_escape(html, watch->category);
    return ok
        && lite_html_text(html, "</p><span class=play-label>")
        && lite_html_text(
            html, autofocus ? "Play in native player" : "Play video")
        && lite_html_format(
            html,
            "</span><p><a class=comments-link href=\"https://tilefinch.local/"
            "offline/youtube?id=%s\">Save video offline</a></p>"
            "</div></section>",
            watch->video.id);
}

static bool lite_build_document_with_comments_decoded(
    Budget *budget, const char *url, const char *source,
    size_t source_length, const char *comments_source,
    size_t comments_length, YoutubeLiteDocument *document,
    char *error, size_t error_size,
    const char *prepared_decoded, size_t prepared_decoded_length)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    YoutubeLiteRoute route =
        url == NULL ? YOUTUBE_LITE_ROUTE_NONE : youtube_lite_route(url);
    bool direct_search_continuation =
        route == YOUTUBE_LITE_ROUTE_SEARCH
        && comments_source != NULL && comments_length != 0;
    if (budget == NULL || url == NULL || document == NULL
        || (!direct_search_continuation
            && (source == NULL || source_length == 0))
        || source_length > YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES
        || comments_length > YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES
        || (comments_length != 0 && comments_source == NULL)) {
        lite_error(error, error_size, "invalid bounded YouTube page data");
        return false;
    }
    if (route == YOUTUBE_LITE_ROUTE_NONE) {
        lite_error(error, error_size, "unsupported YouTube lite route");
        return false;
    }
    *document = (YoutubeLiteDocument) {.budget = budget, .route = route};
    YoutubeLiteVideo videos[YOUTUBE_LITE_MAXIMUM_RESULTS] = {0};
    size_t decoded_length = 0;
    char *decoded = NULL;
    bool decoded_borrowed = false;
    if (route != YOUTUBE_LITE_ROUTE_HOME) {
        bool search_continuation =
            route == YOUTUBE_LITE_ROUTE_SEARCH
            && comments_source != NULL && comments_length != 0;
        if (search_continuation) {
            decoded = budget_malloc_category(
                budget, BUDGET_CATEGORY_RESOURCE, comments_length + 1u);
            if (decoded != NULL) {
                memcpy(decoded, comments_source, comments_length);
                decoded[comments_length] = '\0';
                decoded_length = comments_length;
            }
        } else if (prepared_decoded != NULL) {
            decoded = (char *) prepared_decoded;
            decoded_length = prepared_decoded_length;
            decoded_borrowed = true;
        } else {
            decoded = lite_initial_data(
                budget, source, source_length, &decoded_length);
        }
    }
    size_t result_count = decoded == NULL ? 0 : lite_parse_videos(
        decoded, decoded_length, videos, YOUTUBE_LITE_MAXIMUM_RESULTS);
    size_t display_count = route == YOUTUBE_LITE_ROUTE_WATCH
        && result_count > 6 ? 6 : result_count;

    char query[YOUTUBE_LITE_QUERY_LIMIT] = {0};
    (void) lite_query_value(
        url, "search_query", query, sizeof(query));
    bool compact_results = lite_compact_results_requested(url);
    YoutubeLiteWatch *watch = NULL;
    if (route == YOUTUBE_LITE_ROUTE_WATCH) {
        watch = budget_calloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*watch));
        if (watch == NULL) {
            if (decoded != NULL && !decoded_borrowed)
                budget_free(budget, decoded);
            lite_error(error, error_size,
                       "YouTube watch metadata exceeded its memory bound");
            return false;
        }
        (void) youtube_watch_url_video_id(url, watch->video.id);
        (void) lite_watch_metadata(
            source, source_length, decoded, decoded_length, watch);
        if (watch->video.id[0] == '\0')
            (void) youtube_watch_url_video_id(url, watch->video.id);
        if (watch->video.title[0] == '\0')
            snprintf(watch->video.title, sizeof(watch->video.title),
                     "YouTube video");
    }
    bool comments_requested = route == YOUTUBE_LITE_ROUTE_WATCH
        && lite_comments_view_requested(url);
    bool description_requested = route == YOUTUBE_LITE_ROUTE_WATCH
        && lite_description_view_requested(url);
    YoutubeLiteComment *comments = NULL;
    size_t comment_count = 0;
    char comments_count[YOUTUBE_LITE_METADATA_LIMIT] = {0};
    char next_continuation[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
    if (decoded != NULL
        && (route == YOUTUBE_LITE_ROUTE_SEARCH
            || route == YOUTUBE_LITE_ROUTE_CHANNEL)) {
        (void) lite_next_continuation(
            decoded, decoded_length, next_continuation);
    }
    if (comments_requested && comments_source != NULL
        && comments_length != 0) {
        comments = budget_calloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            YOUTUBE_LITE_MAXIMUM_COMMENTS, sizeof(*comments));
        if (comments != NULL)
            comment_count = lite_parse_comments(
                comments_source, comments_length, comments,
                YOUTUBE_LITE_MAXIMUM_COMMENTS,
                comments_count, sizeof(comments_count));
        (void) lite_next_continuation(
            comments_source, comments_length, next_continuation);
    }

    YoutubeLiteHtml html = {.budget = budget};
    char page_title[YOUTUBE_LITE_TITLE_LIMIT + 32];
    if (route == YOUTUBE_LITE_ROUTE_HOME) {
        snprintf(page_title, sizeof(page_title), "YouTube");
    } else if (route == YOUTUBE_LITE_ROUTE_SEARCH) {
        snprintf(page_title, sizeof(page_title), "%s%s",
                 query[0] == '\0' ? "" : query,
                 query[0] == '\0' ? "YouTube Search" : " - YouTube");
    } else if (route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        snprintf(page_title, sizeof(page_title), "YouTube Channel");
    } else {
        snprintf(page_title, sizeof(page_title), "%s", watch->video.title);
    }
    bool ok = lite_html_header(
        &html, query, page_title, route == YOUTUBE_LITE_ROUTE_HOME,
        compact_results);
    if (ok && route == YOUTUBE_LITE_ROUTE_HOME) {
        ok = lite_html_text(
            &html, "<section class=hero>"
                   "<h1>Watch YouTube on this device</h1></section>");
    } else if (ok && route == YOUTUBE_LITE_ROUTE_SEARCH) {
        ok = lite_html_search_intro(&html, query, result_count);
    } else if (ok && route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        ok = lite_html_text(
            &html, "<h1>Channel videos</h1>"
                   "<p class=hint>Recent public uploads from this channel."
                   "</p>");
        if (ok && result_count == 0)
            ok = lite_html_text(
                &html, "<p class=empty>No public video entries were present "
                       "in the bounded channel response.</p>");
    } else if (ok) {
        ok = lite_html_watch_intro(
            &html, watch,
            !comments_requested && !description_requested);
        if (ok && !comments_requested && !description_requested)
            ok = lite_html_text(
                &html, "<p class=hint>Select the thumbnail to retry playback. "
                       "The player provides play, pause, seek, time, and exit "
                       "controls.</p>");
        if (ok && watch->description[0] != '\0') {
            ok = lite_html_text(
                    &html, "<h2>Description</h2>"
                           "<div id=description class=description>")
                && (description_requested
                    ? lite_html_multiline(&html, watch->description)
                    : lite_html_description_summary(
                          &html, watch->description))
                && lite_html_text(&html, "</div>");
            if (ok && !description_requested)
                ok = lite_html_format(
                    &html, "<p><a class=comments-link href=\"https://www."
                           "youtube.com/watch?v=%s&amp;tilefinch_view="
                           "description#description\">"
                           "View full description</a></p>",
                    watch->video.id);
            else if (ok)
                ok = lite_html_format(
                    &html, "<p><a class=comments-link href=\"https://www."
                           "youtube.com/watch?v=%s\">Back to video</a></p>",
                    watch->video.id);
        }
        if (ok && !comments_requested && !description_requested)
            ok = lite_html_format(
                &html, "<p><a class=comments-link href=\"https://www.youtube."
                       "com/watch?v=%s&amp;tilefinch_view=comments#comments\">"
                       "View comments</a></p>",
                watch->video.id);
        if (ok && comments_requested) {
            ok = lite_html_text(&html, "<section id=comments><h2>Comments");
            if (ok && comments_count[0] != '\0')
                ok = lite_html_text(&html, " (")
                    && lite_html_escape(&html, comments_count)
                    && lite_html_text(&html, ")");
            ok = ok && lite_html_text(&html, "</h2>");
            if (ok && comment_count == 0)
                ok = lite_html_text(
                    &html, "<p class=empty>No public comments were returned "
                           "within the bounded request.</p>");
            for (size_t i = 0; ok && i < comment_count; i++)
                ok = lite_html_comment(&html, &comments[i]);
            if (ok && next_continuation[0] != '\0') {
                char base[512];
                snprintf(
                    base, sizeof(base),
                    "https://www.youtube.com/watch?v=%s"
                    "&tilefinch_view=comments&tilefinch_token=",
                    watch->video.id);
                ok = lite_html_continuation_link(
                    &html, base, next_continuation,
                    "Load more comments");
            }
            ok = ok && lite_html_text(&html, "</section>");
        }
        if (ok && !description_requested && display_count != 0)
            ok = lite_html_text(&html, "<h2>Up next</h2>");
    }
    if (ok && route != YOUTUBE_LITE_ROUTE_HOME
        && !description_requested) {
        for (size_t i = 0; ok && i < display_count; i++)
            ok = lite_html_video(
                &html, &videos[i],
                route == YOUTUBE_LITE_ROUTE_SEARCH && i == 0,
                query);
    }
    if (ok && route == YOUTUBE_LITE_ROUTE_SEARCH
        && next_continuation[0] != '\0') {
        char base[1024];
        if (!lite_search_continuation_base(query, base, sizeof(base))) {
            ok = false;
        } else {
            ok = lite_html_continuation_link(
                &html, base, next_continuation, "Load more results");
        }
    }
    ok = ok && lite_html_text(&html, "</main></body></html>");
    if (comments != NULL) budget_free(budget, comments);
    if (decoded != NULL && !decoded_borrowed) budget_free(budget, decoded);
    if (watch != NULL) budget_free(budget, watch);
    if (!ok || html.failed || html.data == NULL) {
        budget_free(budget, html.data);
        lite_error(error, error_size,
                   "YouTube lite HTML exceeded its memory bound");
        return false;
    }
    document->html = html.data;
    document->html_length = html.length;
    document->source_bytes = source_length + comments_length;
    document->result_count = display_count;
    return true;
}

bool youtube_lite_build_document_with_comments(
    Budget *budget, const char *url, const char *source,
    size_t source_length, const char *comments_source,
    size_t comments_length, YoutubeLiteDocument *document,
    char *error, size_t error_size)
{
    return lite_build_document_with_comments_decoded(
        budget, url, source, source_length, comments_source,
        comments_length, document, error, error_size, NULL, 0);
}

bool youtube_lite_build_document(
    Budget *budget, const char *url, const char *source,
    size_t source_length, YoutubeLiteDocument *document,
    char *error, size_t error_size)
{
    return youtube_lite_build_document_with_comments(
        budget, url, source, source_length, NULL, 0,
        document, error, error_size);
}

/*
 * Browser-driven builds use a separate cooperative path.  Input discovery is
 * capped to a 16 KiB advance with at most one 32 KiB renderer object per
 * pump.  Renderer/object caps are deliberately much larger than every field
 * we retain, but prevent an attacker-controlled JSON object from becoming an
 * uninterruptible multi-megabyte scan.
 */
#define YOUTUBE_LITE_BUILD_SCAN_BYTES (16u * 1024u)
#define YOUTUBE_LITE_BUILD_OBJECT_BYTES (32u * 1024u)

typedef enum {
    YOUTUBE_LITE_BUILD_VIDEOS = 0,
    YOUTUBE_LITE_BUILD_WATCH_MARKER,
    YOUTUBE_LITE_BUILD_WATCH_DETAILS,
    YOUTUBE_LITE_BUILD_WATCH_MICROFORMAT,
    YOUTUBE_LITE_BUILD_COMMENTS,
    YOUTUBE_LITE_BUILD_EMIT_HEADER,
    YOUTUBE_LITE_BUILD_EMIT_INTRO,
    YOUTUBE_LITE_BUILD_EMIT_DESCRIPTION,
    YOUTUBE_LITE_BUILD_EMIT_COMMENTS_HEADER,
    YOUTUBE_LITE_BUILD_EMIT_COMMENTS,
    YOUTUBE_LITE_BUILD_EMIT_COMMENTS_END,
    YOUTUBE_LITE_BUILD_EMIT_UP_NEXT,
    YOUTUBE_LITE_BUILD_EMIT_VIDEOS,
    YOUTUBE_LITE_BUILD_EMIT_CONTINUATION,
    YOUTUBE_LITE_BUILD_EMIT_FOOTER,
    YOUTUBE_LITE_BUILD_DONE,
    YOUTUBE_LITE_BUILD_FAILED
} YoutubeLiteBuildPhase;

typedef struct {
    Budget *budget;
    const char *url;
    const char *source;
    size_t source_length;
    size_t source_bytes;
    const char *decoded;
    size_t decoded_length;
    const char *supplemental;
    size_t supplemental_length;
    YoutubeLiteRoute route;
    YoutubeLiteBuildPhase phase;
    size_t scan_offset;
    size_t metadata_start;
    size_t emit_index;
    YoutubeLiteVideo videos[YOUTUBE_LITE_MAXIMUM_RESULTS];
    size_t video_count;
    YoutubeLiteWatch watch;
    YoutubeLiteComment comments[YOUTUBE_LITE_MAXIMUM_COMMENTS];
    size_t comment_count;
    bool comments_count_found;
    bool comments_requested;
    bool description_requested;
    bool compact_results;
    bool watch_details_found;
    bool watch_microformat_found;
    bool watch_description_header_found;
    char query[YOUTUBE_LITE_QUERY_LIMIT];
    char comments_count[YOUTUBE_LITE_METADATA_LIMIT];
    char next_continuation[YOUTUBE_LITE_CONTINUATION_LIMIT];
    YoutubeLiteHtml html;
} YoutubeLiteBuildWork;

typedef char YoutubeLiteBuildWorkMustRemainSmall[
    sizeof(YoutubeLiteBuildWork) <= 32u * 1024u ? 1 : -1];

static YoutubeLiteSpan lite_build_window(
    const char *data, size_t length, size_t offset)
{
    if (offset > length) offset = length;
    size_t remaining = length - offset;
    size_t window = YOUTUBE_LITE_BUILD_SCAN_BYTES
                  + YOUTUBE_LITE_BUILD_OBJECT_BYTES;
    if (window > remaining) window = remaining;
    return (YoutubeLiteSpan) {
        data + offset, data + offset + window
    };
}

static void lite_build_work_fail(YoutubeLiteBuildWork *work)
{
    if (work != NULL) work->phase = YOUTUBE_LITE_BUILD_FAILED;
}

static void lite_build_begin_emission(YoutubeLiteBuildWork *work)
{
    if (work->route == YOUTUBE_LITE_ROUTE_WATCH) {
        if (work->watch.video.id[0] == '\0')
            (void) youtube_watch_url_video_id(
                work->url, work->watch.video.id);
        if (work->watch.video.title[0] == '\0')
            snprintf(
                work->watch.video.title,
                sizeof(work->watch.video.title), "YouTube video");
    }
    work->scan_offset = 0;
    work->phase = YOUTUBE_LITE_BUILD_EMIT_HEADER;
}

static void lite_build_after_videos(YoutubeLiteBuildWork *work)
{
    work->scan_offset = 0;
    if (work->route == YOUTUBE_LITE_ROUTE_WATCH) {
        work->phase = YOUTUBE_LITE_BUILD_WATCH_MARKER;
    } else if (work->comments_requested
               && work->supplemental != NULL
               && work->supplemental_length != 0) {
        work->phase = YOUTUBE_LITE_BUILD_COMMENTS;
    } else {
        lite_build_begin_emission(work);
    }
}

static void lite_build_after_watch(YoutubeLiteBuildWork *work)
{
    work->scan_offset = 0;
    if (work->comments_requested
        && work->supplemental != NULL
        && work->supplemental_length != 0) {
        work->phase = YOUTUBE_LITE_BUILD_COMMENTS;
    } else {
        lite_build_begin_emission(work);
    }
}

static YoutubeLiteBuildWork *lite_build_work_create(
    Budget *budget, const char *url, const char *source,
    size_t source_length, size_t source_bytes, const char *supplemental,
    size_t supplemental_length, const char *decoded,
    size_t decoded_length, bool compact_results,
    char *error, size_t error_size)
{
    YoutubeLiteRoute route =
        url == NULL ? YOUTUBE_LITE_ROUTE_NONE : youtube_lite_route(url);
    bool direct_search_continuation =
        route == YOUTUBE_LITE_ROUTE_SEARCH
        && supplemental != NULL && supplemental_length != 0;
    bool raw_source_required = route == YOUTUBE_LITE_ROUTE_WATCH;
    if (budget == NULL || url == NULL || route == YOUTUBE_LITE_ROUTE_NONE
        || (raw_source_required && (source == NULL || source_length == 0))
        || source_length > YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES
        || source_bytes > YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES
        || supplemental_length > YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES
        || (supplemental_length != 0 && supplemental == NULL)) {
        lite_error(error, error_size, "invalid bounded YouTube build data");
        return NULL;
    }
    YoutubeLiteBuildWork *work = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*work));
    if (work == NULL) {
        lite_error(error, error_size,
                   "YouTube cooperative build exceeded its memory bound");
        return NULL;
    }
    work->budget = budget;
    work->url = url;
    work->source = source;
    work->source_length = source_length;
    work->source_bytes = source_bytes;
    work->supplemental = supplemental;
    work->supplemental_length = supplemental_length;
    work->route = route;
    work->html.budget = budget;
    work->comments_requested = route == YOUTUBE_LITE_ROUTE_WATCH
        && lite_comments_view_requested(url);
    work->description_requested = route == YOUTUBE_LITE_ROUTE_WATCH
        && lite_description_view_requested(url);
    work->compact_results = compact_results;
    (void) lite_query_value(
        url, "search_query", work->query, sizeof(work->query));
    if (route == YOUTUBE_LITE_ROUTE_WATCH)
        (void) youtube_watch_url_video_id(url, work->watch.video.id);
    if (direct_search_continuation) {
        work->decoded = supplemental;
        work->decoded_length = supplemental_length;
    } else {
        work->decoded = decoded;
        work->decoded_length = decoded_length;
    }
    if (route != YOUTUBE_LITE_ROUTE_HOME
        && work->decoded != NULL && work->decoded_length != 0) {
        work->phase = YOUTUBE_LITE_BUILD_VIDEOS;
    } else {
        lite_build_after_videos(work);
    }
    return work;
}

static void lite_build_video_pump(YoutubeLiteBuildWork *work)
{
    if (work->scan_offset >= work->decoded_length
        || (work->video_count >= YOUTUBE_LITE_MAXIMUM_RESULTS
            && work->route != YOUTUBE_LITE_ROUTE_SEARCH
            && work->route != YOUTUBE_LITE_ROUTE_CHANNEL)) {
        lite_build_after_videos(work);
        return;
    }
    if (work->video_count >= YOUTUBE_LITE_MAXIMUM_RESULTS
        && (work->route == YOUTUBE_LITE_ROUTE_SEARCH
            || work->route == YOUTUBE_LITE_ROUTE_CHANNEL)) {
        /* Once the bounded result table is full, only the next-page token is
           useful. The object-window parser below deliberately overlaps each
           16 KiB advance by 32 KiB so a renderer cannot straddle a window;
           repeating that scan across the remainder of a large response did
           roughly three times the necessary lexical work. Find continuation
           objects once, then parse only their bounded object windows. */
        size_t found = 0;
        YoutubeLiteScanStatus status = lite_bytes_scan_pump(
            work->decoded, work->decoded_length, work->decoded_length,
            "\"continuationItemRenderer\"", &work->scan_offset, &found);
        if (status == YOUTUBE_LITE_SCAN_FOUND) {
            size_t remaining = work->decoded_length - found;
            size_t window = YOUTUBE_LITE_BUILD_OBJECT_BYTES
                          + YOUTUBE_LITE_BUILD_SCAN_BYTES;
            if (window > remaining) window = remaining;
            char token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
            if (lite_next_continuation(
                    work->decoded + found, window, token)) {
                snprintf(work->next_continuation,
                         sizeof(work->next_continuation), "%s", token);
            }
            return;
        }
        if (status == YOUTUBE_LITE_SCAN_EXHAUSTED) {
            lite_build_after_videos(work);
        }
        return;
    }
    YoutubeLiteSpan scope = lite_build_window(
        work->decoded, work->decoded_length, work->scan_offset);
    if (work->route == YOUTUBE_LITE_ROUTE_SEARCH
        || work->route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        char token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
        if (lite_next_continuation(
                scope.start, (size_t) (scope.end - scope.start), token)) {
            snprintf(
                work->next_continuation,
                sizeof(work->next_continuation), "%s", token);
        }
    }
    YoutubeLiteSpan renderer = {0};
    size_t kind = 0;
    /* Discover the localized watch header in the renderer walk already
       needed for result cards. A separate key scan doubled the overlapping
       JSON-window work on PSP until the header happened to be found. */
    bool found = work->video_count < YOUTUBE_LITE_MAXIMUM_RESULTS
        && lite_next_video_renderer(
            &scope, scope.start,
            work->route == YOUTUBE_LITE_ROUTE_WATCH
                && !work->watch_description_header_found,
            &renderer, &kind);
    if (found) {
        if (kind == YOUTUBE_LITE_RENDERER_DESCRIPTION_HEADER) {
            lite_watch_description_header(&renderer, &work->watch);
            work->watch_description_header_found = true;
            work->scan_offset =
                (size_t) (renderer.end - work->decoded);
            return;
        }
        YoutubeLiteVideo parsed;
        if (lite_parse_video(
                &renderer,
                kind == YOUTUBE_LITE_RENDERER_VIDEO_WITH_CONTEXT,
                &parsed)
            && !lite_video_duplicate(
                work->videos, work->video_count, parsed.id)) {
            work->videos[work->video_count++] = parsed;
        }
        work->scan_offset =
            (size_t) (renderer.end - work->decoded);
        return;
    }
    if (scope.end == work->decoded + work->decoded_length) {
        lite_build_after_videos(work);
    } else {
        work->scan_offset += YOUTUBE_LITE_BUILD_SCAN_BYTES;
    }
}

static void lite_build_watch_marker_pump(YoutubeLiteBuildWork *work)
{
    static const char marker[] = "ytInitialPlayerResponse";
    if (work->scan_offset >= work->source_length) {
        lite_build_after_watch(work);
        return;
    }
    size_t remaining = work->source_length - work->scan_offset;
    size_t window = YOUTUBE_LITE_BUILD_SCAN_BYTES + sizeof(marker) - 1u;
    if (window > remaining) window = remaining;
    const char *start = work->source + work->scan_offset;
    const char *found = lite_find_bytes(start, start + window, marker);
    if (found != NULL) {
        work->metadata_start =
            (size_t) (found - work->source) + sizeof(marker) - 1u;
        work->scan_offset = work->metadata_start;
        work->phase = YOUTUBE_LITE_BUILD_WATCH_DETAILS;
    } else if (window == remaining) {
        lite_build_after_watch(work);
    } else {
        work->scan_offset += YOUTUBE_LITE_BUILD_SCAN_BYTES;
    }
}

static void lite_build_watch_key_pump(
    YoutubeLiteBuildWork *work, const char *key, bool details)
{
    if (work->scan_offset >= work->source_length) {
        work->scan_offset = work->metadata_start;
        if (details) {
            work->phase = YOUTUBE_LITE_BUILD_WATCH_MICROFORMAT;
        } else {
            lite_build_after_watch(work);
        }
        return;
    }
    YoutubeLiteSpan scope = lite_build_window(
        work->source, work->source_length, work->scan_offset);
    YoutubeLiteSpan value = {0};
    if (lite_json_key(&scope, key, NULL, &value)) {
        if (details) {
            lite_watch_details(&value, &work->watch);
            work->watch_details_found = true;
            work->scan_offset = work->metadata_start;
            work->phase = YOUTUBE_LITE_BUILD_WATCH_MICROFORMAT;
        } else {
            lite_watch_microformat(&value, &work->watch);
            work->watch_microformat_found = true;
            lite_build_after_watch(work);
        }
    } else if (scope.end == work->source + work->source_length) {
        work->scan_offset = work->source_length;
    } else {
        work->scan_offset += YOUTUBE_LITE_BUILD_SCAN_BYTES;
    }
}

static void lite_build_comments_pump(YoutubeLiteBuildWork *work)
{
    if (work->scan_offset >= work->supplemental_length) {
        lite_build_begin_emission(work);
        return;
    }
    YoutubeLiteSpan scope = lite_build_window(
        work->supplemental, work->supplemental_length,
        work->scan_offset);
    if (!work->comments_count_found) {
        YoutubeLiteSpan header;
        if (lite_json_key(
                &scope, "commentsHeaderRenderer", NULL, &header)) {
            work->comments_count_found =
                lite_text_runs(
                    &header, "commentsCount",
                    work->comments_count,
                    sizeof(work->comments_count))
                || lite_text_runs(
                    &header, "countText",
                    work->comments_count,
                    sizeof(work->comments_count));
        }
    }
    char token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
    if (lite_next_continuation(
            scope.start, (size_t) (scope.end - scope.start), token)) {
        snprintf(
            work->next_continuation,
            sizeof(work->next_continuation), "%s", token);
    }
    YoutubeLiteSpan renderer = {0};
    bool found = work->comment_count < YOUTUBE_LITE_MAXIMUM_COMMENTS
        && lite_json_key(
            &scope, "commentRenderer", NULL, &renderer);
    if (found) {
        YoutubeLiteComment parsed;
        if (lite_parse_comment_renderer(&renderer, &parsed))
            work->comments[work->comment_count++] = parsed;
        work->scan_offset =
            (size_t) (renderer.end - work->supplemental);
        return;
    }
    if (scope.end
        == work->supplemental + work->supplemental_length) {
        lite_build_begin_emission(work);
    } else {
        work->scan_offset += YOUTUBE_LITE_BUILD_SCAN_BYTES;
    }
}

static bool lite_build_emit_header(YoutubeLiteBuildWork *work)
{
    char title[YOUTUBE_LITE_TITLE_LIMIT + 32];
    if (work->route == YOUTUBE_LITE_ROUTE_HOME) {
        snprintf(title, sizeof(title), "YouTube");
    } else if (work->route == YOUTUBE_LITE_ROUTE_SEARCH) {
        snprintf(title, sizeof(title), "%s%s",
                 work->query[0] == '\0' ? "" : work->query,
                 work->query[0] == '\0'
                    ? "YouTube Search" : " - YouTube");
    } else if (work->route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        snprintf(title, sizeof(title), "YouTube Channel");
    } else {
        snprintf(
            title, sizeof(title), "%s", work->watch.video.title);
    }
    return lite_html_header(
        &work->html, work->query, title,
        work->route == YOUTUBE_LITE_ROUTE_HOME,
        work->compact_results);
}

static bool lite_build_emit_intro(YoutubeLiteBuildWork *work)
{
    YoutubeLiteHtml *html = &work->html;
    if (work->route == YOUTUBE_LITE_ROUTE_HOME) {
        return lite_html_text(
            html, "<section class=hero>"
                  "<h1>Watch YouTube on this device</h1></section>");
    }
    if (work->route == YOUTUBE_LITE_ROUTE_SEARCH) {
        return lite_html_search_intro(
            html, work->query, work->video_count);
    }
    if (work->route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        bool ok = lite_html_text(
            html, "<h1>Channel videos</h1>"
                  "<p class=hint>Recent public uploads from this channel."
                  "</p>");
        if (ok && work->video_count == 0)
            ok = lite_html_text(
                html, "<p class=empty>No public video entries were present "
                      "in the bounded channel response.</p>");
        return ok;
    }
    YoutubeLiteWatch *watch = &work->watch;
    bool ok = lite_html_watch_intro(
        html, watch,
        !work->comments_requested && !work->description_requested);
    if (ok && !work->comments_requested
        && !work->description_requested)
        ok = lite_html_text(
            html, "<p class=hint>Select the thumbnail to retry playback. "
                  "The player provides play, pause, seek, time, and exit "
                  "controls.</p>");
    return ok;
}

static bool lite_build_emit_description(YoutubeLiteBuildWork *work)
{
    if (work->route != YOUTUBE_LITE_ROUTE_WATCH) return true;
    YoutubeLiteHtml *html = &work->html;
    YoutubeLiteWatch *watch = &work->watch;
    bool ok = true;
    if (watch->description[0] != '\0') {
        ok = lite_html_text(
                 html, "<h2>Description</h2>"
                       "<div id=description class=description>")
            && (work->description_requested
                ? lite_html_multiline(html, watch->description)
                : lite_html_description_summary(
                      html, watch->description))
            && lite_html_text(html, "</div>");
        if (ok && !work->description_requested)
            ok = lite_html_format(
                html, "<p><a class=comments-link href=\"https://www."
                      "youtube.com/watch?v=%s&amp;tilefinch_view="
                      "description#description\">"
                      "View full description</a></p>",
                watch->video.id);
        else if (ok)
            ok = lite_html_format(
                html, "<p><a class=comments-link href=\"https://www."
                      "youtube.com/watch?v=%s\">Back to video</a></p>",
                watch->video.id);
    }
    if (ok && !work->comments_requested
        && !work->description_requested) {
        ok = lite_html_format(
            html, "<p><a class=comments-link href=\"https://www.youtube."
                  "com/watch?v=%s&amp;tilefinch_view=comments#comments\">"
                  "View comments</a></p>",
            watch->video.id);
    }
    return ok;
}

static bool lite_build_emit_comments_header(YoutubeLiteBuildWork *work)
{
    YoutubeLiteHtml *html = &work->html;
    bool ok = lite_html_text(
        html, "<section id=comments><h2>Comments");
    if (ok && work->comments_count[0] != '\0')
        ok = lite_html_text(html, " (")
            && lite_html_escape(html, work->comments_count)
            && lite_html_text(html, ")");
    ok = ok && lite_html_text(html, "</h2>");
    if (ok && work->comment_count == 0)
        ok = lite_html_text(
            html, "<p class=empty>No public comments were returned "
                  "within the bounded request.</p>");
    return ok;
}

static bool lite_build_emit_comments_end(YoutubeLiteBuildWork *work)
{
    bool ok = true;
    if (work->next_continuation[0] != '\0') {
        char base[512];
        snprintf(
            base, sizeof(base),
            "https://www.youtube.com/watch?v=%s"
            "&tilefinch_view=comments&tilefinch_token=",
            work->watch.video.id);
        ok = lite_html_continuation_link(
            &work->html, base, work->next_continuation,
            "Load more comments");
    }
    return ok && lite_html_text(&work->html, "</section>");
}

static bool lite_build_emit_search_continuation(
    YoutubeLiteBuildWork *work)
{
    if (work->route != YOUTUBE_LITE_ROUTE_SEARCH
        || work->next_continuation[0] == '\0') return true;
    char base[1024];
    if (!lite_search_continuation_base(
            work->query, base, sizeof(base)))
        return false;
    return lite_html_continuation_link(
        &work->html, base, work->next_continuation,
        "Load more results");
}

static void lite_build_work_pump(YoutubeLiteBuildWork *work)
{
    if (work == NULL || work->phase >= YOUTUBE_LITE_BUILD_DONE) return;
    bool ok = true;
    switch (work->phase) {
    case YOUTUBE_LITE_BUILD_VIDEOS:
        lite_build_video_pump(work);
        return;
    case YOUTUBE_LITE_BUILD_WATCH_MARKER:
        lite_build_watch_marker_pump(work);
        return;
    case YOUTUBE_LITE_BUILD_WATCH_DETAILS:
        lite_build_watch_key_pump(
            work, "videoDetails", true);
        return;
    case YOUTUBE_LITE_BUILD_WATCH_MICROFORMAT:
        lite_build_watch_key_pump(
            work, "playerMicroformatRenderer", false);
        return;
    case YOUTUBE_LITE_BUILD_COMMENTS:
        lite_build_comments_pump(work);
        return;
    case YOUTUBE_LITE_BUILD_EMIT_HEADER:
        ok = lite_build_emit_header(work);
        work->phase = YOUTUBE_LITE_BUILD_EMIT_INTRO;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_INTRO:
        ok = lite_build_emit_intro(work);
        work->phase = YOUTUBE_LITE_BUILD_EMIT_DESCRIPTION;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_DESCRIPTION:
        ok = lite_build_emit_description(work);
        work->phase = work->comments_requested
            ? YOUTUBE_LITE_BUILD_EMIT_COMMENTS_HEADER
            : YOUTUBE_LITE_BUILD_EMIT_UP_NEXT;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_COMMENTS_HEADER:
        ok = lite_build_emit_comments_header(work);
        work->emit_index = 0;
        work->phase = YOUTUBE_LITE_BUILD_EMIT_COMMENTS;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_COMMENTS:
        if (work->emit_index < work->comment_count) {
            ok = lite_html_comment(
                &work->html, &work->comments[work->emit_index++]);
        } else {
            work->phase = YOUTUBE_LITE_BUILD_EMIT_COMMENTS_END;
        }
        break;
    case YOUTUBE_LITE_BUILD_EMIT_COMMENTS_END:
        ok = lite_build_emit_comments_end(work);
        work->phase = YOUTUBE_LITE_BUILD_EMIT_UP_NEXT;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_UP_NEXT: {
        size_t display = work->route == YOUTUBE_LITE_ROUTE_WATCH
            && work->video_count > 6u ? 6u : work->video_count;
        if (work->route == YOUTUBE_LITE_ROUTE_WATCH
            && !work->description_requested && display != 0) {
            ok = lite_html_text(&work->html, "<h2>Up next</h2>");
        }
        work->emit_index = 0;
        work->phase = YOUTUBE_LITE_BUILD_EMIT_VIDEOS;
        break;
    }
    case YOUTUBE_LITE_BUILD_EMIT_VIDEOS: {
        size_t display = work->route == YOUTUBE_LITE_ROUTE_WATCH
            && work->video_count > 6u ? 6u : work->video_count;
        if (work->route != YOUTUBE_LITE_ROUTE_HOME
            && !work->description_requested
            && work->emit_index < display) {
            size_t emit_index = work->emit_index++;
            ok = lite_html_video(
                &work->html, &work->videos[emit_index],
                work->route == YOUTUBE_LITE_ROUTE_SEARCH
                    && emit_index == 0,
                work->query);
        } else {
            work->phase = YOUTUBE_LITE_BUILD_EMIT_CONTINUATION;
        }
        break;
    }
    case YOUTUBE_LITE_BUILD_EMIT_CONTINUATION:
        ok = lite_build_emit_search_continuation(work);
        work->phase = YOUTUBE_LITE_BUILD_EMIT_FOOTER;
        break;
    case YOUTUBE_LITE_BUILD_EMIT_FOOTER:
        ok = lite_html_text(&work->html, "</main></body></html>");
        work->phase = YOUTUBE_LITE_BUILD_DONE;
        break;
    case YOUTUBE_LITE_BUILD_DONE:
    case YOUTUBE_LITE_BUILD_FAILED:
        return;
    }
    if (!ok || work->html.failed)
        lite_build_work_fail(work);
}

static bool lite_build_work_take_document(
    YoutubeLiteBuildWork *work, YoutubeLiteDocument *document)
{
    if (work == NULL || document == NULL
        || work->phase != YOUTUBE_LITE_BUILD_DONE
        || work->html.data == NULL) return false;
    size_t display = work->route == YOUTUBE_LITE_ROUTE_WATCH
        && work->video_count > 6u ? 6u : work->video_count;
    *document = (YoutubeLiteDocument) {
        .budget = work->budget,
        .html = work->html.data,
        .html_length = work->html.length,
        .source_bytes = work->source_bytes + work->supplemental_length,
        .result_count = display,
        .route = work->route
    };
    work->html.data = NULL;
    work->html.length = 0;
    work->html.capacity = 0;
    return true;
}

static void lite_build_work_destroy(YoutubeLiteBuildWork *work)
{
    if (work == NULL) return;
    Budget *budget = work->budget;
    budget_free(budget, work->html.data);
    budget_free(budget, work);
}

static void lite_accept_cookies(
    BrowserSession *session, const TilefinchRequestContext *context,
    const FetchResult *fetch, const char *fallback_url)
{
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        TilefinchRequestContext response = *context;
        response.target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            session, &response, fetch->set_cookies[i]);
    }
}

static bool lite_fetch_url(const char *url, YoutubeLiteRoute route,
                           char output[1024])
{
    if (route == YOUTUBE_LITE_ROUTE_HOME) {
        snprintf(output, 1024, "https://m.youtube.com/");
        return true;
    }
    if (route == YOUTUBE_LITE_ROUTE_WATCH) {
        char id[YOUTUBE_VIDEO_ID_CAPACITY];
        if (!youtube_watch_url_video_id(url, id)) return false;
        int length = snprintf(
            output, 1024, "https://m.youtube.com/watch?v=%s"
                          "&bpctr=9999999999&has_verified=1", id);
        return length >= 0 && length < 1024;
    }
    if (route == YOUTUBE_LITE_ROUTE_CHANNEL) {
        TilefinchUrl parsed;
        if (!tilefinch_url_parse(url, &parsed)
            || parsed.path_length == 0 || parsed.path_length > 900u)
            return false;
        int length = snprintf(
            output, 1024, "https://m.youtube.com%.*s",
            (int) parsed.path_length,
            parsed.value + parsed.path_offset);
        return length >= 0 && length < 1024;
    }
    char query[YOUTUBE_LITE_QUERY_LIMIT] = {0};
    char encoded[YOUTUBE_LITE_QUERY_LIMIT * 3u] = {0};
    (void) lite_query_value(url, "search_query", query, sizeof(query));
    if (!lite_url_encode(query, encoded, sizeof(encoded))) return false;
    int length = snprintf(
        output, 1024,
        "https://m.youtube.com/results?search_query=%s", encoded);
    return length >= 0 && length < 1024;
}

static bool lite_json_quote(const char *input, char *output,
                            size_t output_size)
{
    if (input == NULL || output == NULL || output_size < 3u) return false;
    size_t used = 0;
    output[used++] = '"';
    for (const unsigned char *at = (const unsigned char *) input;
         *at != '\0'; at++) {
        const char *escaped = NULL;
        if (*at == '"') escaped = "\\\"";
        else if (*at == '\\') escaped = "\\\\";
        else if (*at == '\n') escaped = "\\n";
        else if (*at == '\r') escaped = "\\r";
        else if (*at == '\t') escaped = "\\t";
        if (escaped != NULL) {
            if (used + 2u >= output_size) return false;
            output[used++] = escaped[0];
            output[used++] = escaped[1];
        } else {
            if (*at < 0x20u || used + 1u >= output_size) return false;
            output[used++] = (char) *at;
        }
    }
    if (used + 1u >= output_size) return false;
    output[used++] = '"';
    output[used] = '\0';
    return true;
}

typedef struct {
    uint32_t version;
    char api_key[128];
    char client_version[64];
    char visitor[1024];
} YoutubeLiteIdentity;

static bool lite_identity_valid(const YoutubeLiteIdentity *identity)
{
    return identity != NULL
        && identity->version == YOUTUBE_LITE_IDENTITY_CACHE_VERSION
        && lite_api_token_safe(identity->api_key, false)
        && lite_api_token_safe(identity->client_version, true)
        && lite_header_value_safe(identity->visitor);
}

static bool lite_identity_cache_get(
    BrowserSession *session, YoutubeLiteIdentity *identity)
{
    size_t length = 0;
    uint64_t now_ns = tilefinch_platform_monotonic_time_ns();
    return browser_session_site_adapter_state_get(
               session, YOUTUBE_LITE_IDENTITY_CACHE_KEY,
               identity, sizeof(*identity), &length, now_ns,
               YOUTUBE_LITE_IDENTITY_CACHE_MAX_AGE_NS)
        && length == sizeof(*identity)
        && lite_identity_valid(identity);
}

static bool lite_identity_cache_put(
    BrowserSession *session, const YoutubeLiteIdentity *identity)
{
    return lite_identity_valid(identity)
        && browser_session_site_adapter_state_put(
            session, YOUTUBE_LITE_IDENTITY_CACHE_KEY,
            identity, sizeof(*identity),
            tilefinch_platform_monotonic_time_ns());
}

bool youtube_lite_resolver_identity_get(
    BrowserSession *session, YoutubeLiteResolverIdentity *identity)
{
    if (session == NULL || identity == NULL) return false;
    YoutubeLiteIdentity cached = {0};
    size_t length = 0;
    uint64_t now_ns = tilefinch_platform_monotonic_time_ns();
    if (!browser_session_site_adapter_state_get(
            session, YOUTUBE_LITE_IDENTITY_CACHE_KEY,
            &cached, sizeof(cached), &length, now_ns,
            YOUTUBE_LITE_IDENTITY_CACHE_MAX_AGE_NS)
        || length != sizeof(cached)
        || cached.version != YOUTUBE_LITE_IDENTITY_CACHE_VERSION
        || !lite_api_token_safe(cached.api_key, false)
        || !lite_header_value_safe(cached.visitor)) return false;
    snprintf(identity->api_key, sizeof(identity->api_key), "%s",
             cached.api_key);
    snprintf(identity->visitor, sizeof(identity->visitor), "%s",
             cached.visitor);
    return true;
}

static void lite_resolver_identity_cache_put(
    BrowserSession *session, const YoutubeLiteIdentity *identity)
{
    if (session == NULL || identity == NULL
        || !lite_api_token_safe(identity->api_key, false)
        || !lite_header_value_safe(identity->visitor)) return;
    (void) browser_session_site_adapter_state_put(
        session, YOUTUBE_LITE_IDENTITY_CACHE_KEY,
        identity, sizeof(*identity), tilefinch_platform_monotonic_time_ns());
}

typedef enum {
    YOUTUBE_LITE_JOB_PRIMARY = 0,
    YOUTUBE_LITE_JOB_IDENTITY_API_KEY,
    YOUTUBE_LITE_JOB_DECODE,
    YOUTUBE_LITE_JOB_COMMENTS_PANEL,
    YOUTUBE_LITE_JOB_COMMENTS_MARKER,
    YOUTUBE_LITE_JOB_COMMENTS_COMMAND,
    YOUTUBE_LITE_JOB_COMMENTS_TOKEN,
    YOUTUBE_LITE_JOB_PREPARE,
    YOUTUBE_LITE_JOB_SUPPLEMENTAL,
    YOUTUBE_LITE_JOB_BUILD
} YoutubeLiteJobPhase;

typedef struct {
    char endpoint[512];
    char body[8192];
    char extra_headers[1536];
    char cookies[4096];
    TilefinchRequestContext context;
    FetchRequest request;
} YoutubeLiteSupplementalRequest;

struct YoutubeLiteLoadJob {
    Budget *budget;
    BrowserSession *session;
    FetchScheduler *scheduler;
    uint64_t request_id;
    YoutubeLiteRoute route;
    YoutubeLiteJobPhase phase;
    YoutubeLiteLoadStatus status;
    long timeout_ms;
    uint64_t started_us;
    size_t maximum_source_bytes;
    bool supplemental_requested;
    bool supplemental_fetched;
    bool direct_continuation;
    bool fallback_attempted;
    bool identity_available;
    bool document_taken;
    bool compact_results;
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char fetch_url[1024];
    char language[YOUTUBE_LITE_LANGUAGE_LIMIT];
    char supplemental_url[512];
    char error[256];
    FetchResult source;
    size_t primary_source_bytes;
    long primary_status_code;
    char primary_server[64];
    char primary_cf_mitigated[32];
    FetchResult *supplemental;
    char *decoded;
    size_t decoded_length;
    size_t decode_search_offset;
    size_t decode_input_offset;
    /* A response without an ytInitialData marker leaves `decoded` NULL with
       nothing left to scan.  Latch the completed attempt so PREPARE cannot
       hand the job back to DECODE forever. */
    bool decode_attempted;
    char decode_quote;
    size_t fact_scan_offset;
    size_t fact_scan_limit;
    char continuation_token[YOUTUBE_LITE_CONTINUATION_LIMIT];
    YoutubeLiteIdentity identity;
    YoutubeLiteBuildWork *build;
    YoutubeLiteDocument document;
    YoutubeLiteLoadMetrics metrics;
};

static void lite_load_release_unused_primary(YoutubeLiteLoadJob *job)
{
    if (job == NULL || job->route == YOUTUBE_LITE_ROUTE_WATCH
        || job->source.data == NULL) return;
    /* Home, search and channel builds consume only the bounded decoded
       initial-data buffer by this point. Keep response diagnostics and the
       logical source-byte metric in the job, but return the often-large raw
       HTML before cooperative HTML emission and DOM construction overlap. */
    fetch_result_destroy(&job->source);
}

static TilefinchRequestContext lite_primary_context(
    const char *page_url, const char *fetch_url)
{
    return (TilefinchRequestContext) {
        .target_url = fetch_url,
        .top_level_url = page_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = true
    };
}

static TilefinchRequestContext lite_supplemental_context(
    const char *page_url, const char *endpoint)
{
    return (TilefinchRequestContext) {
        .target_url = endpoint,
        .top_level_url = page_url,
        .method = "POST",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
}

static bool lite_prepare_supplemental_request(
    BrowserSession *session, const char *page_url,
    const YoutubeLiteIdentity *cached_identity,
    const char *language, bool comments, const char *prepared_token,
    YoutubeLiteSupplementalRequest *prepared)
{
    if (prepared == NULL || language == NULL || language[0] == '\0')
        return false;
    memset(prepared, 0, sizeof(*prepared));
    const YoutubeLiteIdentity *identity = cached_identity;
    if (!lite_identity_valid(identity)) return false;
    char token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
    if (comments) {
        if (prepared_token == NULL || prepared_token[0] == '\0') return false;
        snprintf(token, sizeof(token), "%s", prepared_token);
        char requested_token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
        if (lite_query_value(
                page_url, "tilefinch_token", requested_token,
                sizeof(requested_token))) {
            snprintf(token, sizeof(token), "%s", requested_token);
        }
    } else if (!lite_query_value(
                   page_url, "tilefinch_token", token, sizeof(token))) {
        return false;
    }
    int endpoint_length = snprintf(
        prepared->endpoint, sizeof(prepared->endpoint),
        "https://m.youtube.com/youtubei/v1/%s?key=%s&prettyPrint=false",
        comments ? "next" : "search", identity->api_key);
    char token_json[YOUTUBE_LITE_CONTINUATION_LIMIT * 2u];
    char visitor_json[2048] = "\"\"";
    if (endpoint_length < 0
        || (size_t) endpoint_length >= sizeof(prepared->endpoint)
        || !lite_json_quote(token, token_json, sizeof(token_json))
        || (identity->visitor[0] != '\0'
            && !lite_json_quote(
                identity->visitor, visitor_json,
                sizeof(visitor_json)))) return false;
    int body_length = snprintf(
        prepared->body, sizeof(prepared->body),
        "{\"context\":{\"client\":{\"clientName\":\"MWEB\","
        "\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"US\","
        "\"visitorData\":%s}},\"continuation\":%s}",
        identity->client_version, language, visitor_json, token_json);
    int extra_length = snprintf(
        prepared->extra_headers, sizeof(prepared->extra_headers),
        "X-YouTube-Client-Name: 2\n"
        "X-YouTube-Client-Version: %s%s%s",
        identity->client_version,
        identity->visitor[0] == '\0' ? ""
            : "\nX-Goog-Visitor-Id: ",
        identity->visitor[0] == '\0' ? "" : identity->visitor);
    if (body_length < 0
        || (size_t) body_length >= sizeof(prepared->body)
        || extra_length < 0
        || (size_t) extra_length >= sizeof(prepared->extra_headers)) {
        return false;
    }
    prepared->context =
        lite_supplemental_context(page_url, prepared->endpoint);
    (void) browser_session_cookie_header_context(
        session, &prepared->context, prepared->cookies,
        sizeof(prepared->cookies));
    prepared->request = (FetchRequest) {
        .method = "POST",
        .body = prepared->body,
        .body_length = (size_t) body_length,
        .content_type = "application/json",
        .extra_headers = prepared->extra_headers,
        .cookie = prepared->cookies,
        .accept = "application/json",
        .sec_fetch_dest = "empty",
        .sec_fetch_mode = "cors",
        .sec_fetch_site = "same-origin",
        .user_agent = YOUTUBE_LITE_MOBILE_UA,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = prepared->endpoint,
        .initiator_url = "https://m.youtube.com/",
        .redirect_same_origin_only = true,
        .cookie_session = session,
        .cookie_context = &prepared->context
    };
    return true;
}

static void lite_load_job_fail(YoutubeLiteLoadJob *job,
                               const char *format, ...)
{
    if (job == NULL) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(job->error, sizeof(job->error), format, arguments);
    va_end(arguments);
    job->status = YOUTUBE_LITE_LOAD_FAILED;
}

static void lite_load_after_identity(YoutubeLiteLoadJob *job)
{
    job->identity_available = lite_identity_valid(&job->identity);
    if (job->identity_available) {
        (void) lite_identity_cache_put(job->session, &job->identity);
    } else {
        /* The resolver only requires the API key and optional visitor id;
           retain that useful subset without rescanning the response. */
        lite_resolver_identity_cache_put(job->session, &job->identity);
    }
    bool search_continuation =
        job->route == YOUTUBE_LITE_ROUTE_SEARCH
        && job->supplemental_requested;
    job->phase = job->route != YOUTUBE_LITE_ROUTE_HOME
            && !search_continuation
        ? YOUTUBE_LITE_JOB_DECODE : YOUTUBE_LITE_JOB_PREPARE;
    job->fact_scan_offset = 0;
    job->fact_scan_limit = 0;
}

static bool lite_load_identity_pump(YoutubeLiteLoadJob *job)
{
    enum { SCAN_BYTES_PER_PUMP = 16u * 1024u };
    if (job == NULL || job->source.data == NULL) return false;
    if (job->phase != YOUTUBE_LITE_JOB_IDENTITY_API_KEY) return false;
    size_t stop = job->fact_scan_offset + SCAN_BYTES_PER_PUMP;
    if (stop < job->fact_scan_offset || stop > job->source.length)
        stop = job->source.length;
    for (size_t at = job->fact_scan_offset; at < stop; at++) {
        /* JSON key parsing is deliberately stricter than a substring scan,
           but only the two possible initial bytes merit that work. On a
           multi-megabyte response this prefilter avoids three strlen/memcmp
           attempts at essentially every ordinary payload byte. */
        char byte = job->source.data[at];
        if (byte == 'I' && job->identity.api_key[0] == '\0') {
            (void) lite_json_key_string_at(
                job->source.data, job->source.length, at,
                "INNERTUBE_API_KEY", job->identity.api_key,
                sizeof(job->identity.api_key));
        }
        if (byte == 'I' && job->identity.client_version[0] == '\0') {
            (void) lite_json_key_string_at(
                job->source.data, job->source.length, at,
                "INNERTUBE_CONTEXT_CLIENT_VERSION",
                job->identity.client_version,
                sizeof(job->identity.client_version));
        }
        if (byte == 'V' && job->identity.visitor[0] == '\0') {
            (void) lite_json_key_string_at(
                job->source.data, job->source.length, at,
                "VISITOR_DATA", job->identity.visitor,
                sizeof(job->identity.visitor));
        }
    }
    job->fact_scan_offset = stop;
    if (stop == job->source.length) lite_load_after_identity(job);
    return true;
}

static void lite_load_after_decode(YoutubeLiteLoadJob *job)
{
    job->fact_scan_offset = 0;
    job->fact_scan_limit = job->decoded_length;
    job->phase = job->supplemental_requested
            && job->route == YOUTUBE_LITE_ROUTE_WATCH
            && job->decoded != NULL
        ? YOUTUBE_LITE_JOB_COMMENTS_PANEL : YOUTUBE_LITE_JOB_PREPARE;
}

static bool lite_load_comments_scan_pump(YoutubeLiteLoadJob *job)
{
    enum {
        COMMENTS_COMMAND_WINDOW = 256u * 1024u,
        COMMENTS_TOKEN_WINDOW = 16u * 1024u
    };
    if (job == NULL || job->decoded == NULL) return false;
    YoutubeLiteScanStatus status = YOUTUBE_LITE_SCAN_EXHAUSTED;
    size_t found = 0;
    switch (job->phase) {
    case YOUTUBE_LITE_JOB_COMMENTS_PANEL:
        status = lite_bytes_scan_pump(
            job->decoded, job->decoded_length, job->fact_scan_limit,
            "\"engagementPanels\"", &job->fact_scan_offset, &found);
        if (status == YOUTUBE_LITE_SCAN_FOUND) {
            job->phase = YOUTUBE_LITE_JOB_COMMENTS_MARKER;
            job->fact_scan_limit = job->decoded_length;
        }
        break;
    case YOUTUBE_LITE_JOB_COMMENTS_MARKER:
        status = lite_bytes_scan_pump(
            job->decoded, job->decoded_length, job->fact_scan_limit,
            "engagement-panel-comments-section",
            &job->fact_scan_offset, &found);
        if (status == YOUTUBE_LITE_SCAN_FOUND) {
            job->phase = YOUTUBE_LITE_JOB_COMMENTS_COMMAND;
            job->fact_scan_limit = job->fact_scan_offset
                    > SIZE_MAX - COMMENTS_COMMAND_WINDOW
                ? job->decoded_length
                : job->fact_scan_offset + COMMENTS_COMMAND_WINDOW;
            if (job->fact_scan_limit > job->decoded_length)
                job->fact_scan_limit = job->decoded_length;
        }
        break;
    case YOUTUBE_LITE_JOB_COMMENTS_COMMAND:
        status = lite_bytes_scan_pump(
            job->decoded, job->decoded_length, job->fact_scan_limit,
            "\"continuationCommand\"",
            &job->fact_scan_offset, &found);
        if (status == YOUTUBE_LITE_SCAN_FOUND) {
            job->phase = YOUTUBE_LITE_JOB_COMMENTS_TOKEN;
            job->fact_scan_limit = job->fact_scan_offset
                    > SIZE_MAX - COMMENTS_TOKEN_WINDOW
                ? job->decoded_length
                : job->fact_scan_offset + COMMENTS_TOKEN_WINDOW;
            if (job->fact_scan_limit > job->decoded_length)
                job->fact_scan_limit = job->decoded_length;
        }
        break;
    case YOUTUBE_LITE_JOB_COMMENTS_TOKEN:
        status = lite_json_key_string_scan_pump(
            job->decoded, job->decoded_length, job->fact_scan_limit,
            "token", &job->fact_scan_offset, job->continuation_token,
            sizeof(job->continuation_token));
        if (status == YOUTUBE_LITE_SCAN_FOUND) {
            job->phase = YOUTUBE_LITE_JOB_PREPARE;
        }
        break;
    default:
        return false;
    }
    if (status == YOUTUBE_LITE_SCAN_EXHAUSTED)
        job->phase = YOUTUBE_LITE_JOB_PREPARE;
    return true;
}

static bool lite_load_decode_pump(YoutubeLiteLoadJob *job)
{
    enum { TRANSFORM_BYTES_PER_PUMP = 16u * 1024u };
    if (job == NULL || job->source.data == NULL) return false;
    const char *source = job->source.data;
    size_t length = job->source.length;
    if (job->decoded == NULL) {
        size_t stop = job->decode_search_offset + TRANSFORM_BYTES_PER_PUMP;
        if (stop > length) stop = length;
        size_t marker = SIZE_MAX;
        for (size_t at = job->decode_search_offset; at < stop; at++) {
            static const char spaced[] = "ytInitialData = ";
            static const char compact[] = "ytInitialData=";
            if ((sizeof(spaced) - 1u <= length - at
                 && memcmp(source + at, spaced, sizeof(spaced) - 1u) == 0)
                || (sizeof(compact) - 1u <= length - at
                    && memcmp(source + at, compact,
                              sizeof(compact) - 1u) == 0)) {
                marker = at;
                break;
            }
        }
        if (marker == SIZE_MAX) {
            if (stop == length) {
                job->decode_attempted = true;
                lite_load_after_decode(job);
                return true;
            }
            /* Retain enough overlap for a marker split at the pump edge. */
            job->decode_search_offset =
                stop > 16u ? stop - 16u : stop;
            return true;
        }
        size_t at = marker;
        while (at < length && source[at] != '=') at++;
        if (at == length) return false;
        at++;
        while (at < length && isspace((unsigned char) source[at])) at++;
        if (at == length || (source[at] != '\'' && source[at] != '"'))
            return false;
        job->decode_quote = source[at++];
        job->decode_input_offset = at;
        job->decoded = budget_malloc_category(
            job->budget, BUDGET_CATEGORY_RESOURCE, length + 1u);
        if (job->decoded == NULL) return false;
    }
    size_t input_stop =
        job->decode_input_offset + TRANSFORM_BYTES_PER_PUMP;
    if (input_stop > length) input_stop = length;
    const char *at = source + job->decode_input_offset;
    const char *end = source + length;
    while (at < end && (size_t) (at - source) < input_stop
           && *at != job->decode_quote) {
        unsigned char byte = (unsigned char) *at++;
        if (byte != '\\') {
            job->decoded[job->decoded_length++] = (char) byte;
            continue;
        }
        if (at >= end) return false;
        char escape = *at++;
        char simple = '\0';
        if (escape == 'n') simple = '\n';
        else if (escape == 'r') simple = '\r';
        else if (escape == 't') simple = '\t';
        else if (escape == 'b') simple = '\b';
        else if (escape == 'f') simple = '\f';
        else if (escape == '\n') continue;
        else if (escape != 'x' && escape != 'u') simple = escape;
        if (escape == 'x' || escape == 'u') {
            unsigned codepoint = 0;
            if (!lite_decode_hex_escape(
                    &at, end, escape == 'x' ? 2u : 4u,
                    &codepoint)) return false;
            if (escape == 'u' && codepoint >= 0xd800u
                && codepoint <= 0xdbffu) {
                if ((size_t) (end - at) < 6u
                    || at[0] != '\\' || at[1] != 'u') return false;
                at += 2;
                unsigned low = 0;
                if (!lite_decode_hex_escape(&at, end, 4u, &low)
                    || low < 0xdc00u || low > 0xdfffu) return false;
                codepoint = 0x10000u
                    + ((codepoint - 0xd800u) << 10u)
                    + (low - 0xdc00u);
            }
            if (!lite_append_utf8(
                    job->decoded, length + 1u,
                    &job->decoded_length, codepoint)) return false;
        } else {
            job->decoded[job->decoded_length++] = simple;
        }
    }
    job->decode_input_offset = (size_t) (at - source);
    if (at < end && *at == job->decode_quote) {
        job->decoded[job->decoded_length] = '\0';
        job->decode_attempted = true;
        lite_load_after_decode(job);
    } else if (at == end) {
        return false;
    }
    return true;
}

static long lite_load_remaining_timeout_ms(const YoutubeLiteLoadJob *job)
{
    long remaining_ms = job->timeout_ms;
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    if (now_us < job->started_us) return remaining_ms;
    uint64_t elapsed_ms = (now_us - job->started_us) / UINT64_C(1000);
    if (elapsed_ms >= (uint64_t) job->timeout_ms) return 0;
    return remaining_ms - (long) elapsed_ms;
}

static bool lite_load_enqueue_primary(YoutubeLiteLoadJob *job)
{
    TilefinchRequestContext context =
        lite_primary_context(job->url, job->fetch_url);
    char cookies[4096] = {0};
    (void) browser_session_cookie_header_context(
        job->session, &context, cookies, sizeof(cookies));
    FetchRequest request = {
        .method = "GET",
        .cookie = cookies,
        .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,"
                  "*/*;q=0.8",
        .sec_fetch_dest = "document",
        .sec_fetch_mode = "navigate",
        .sec_fetch_site = "none",
        .sec_fetch_user = true,
        .upgrade_insecure_requests = true,
        .user_agent = YOUTUBE_LITE_MOBILE_UA,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = job->fetch_url,
        .initiator_url = job->fetch_url,
        .cookie_session = job->session,
        .cookie_context = &context
    };
    long remaining_ms = lite_load_remaining_timeout_ms(job);
    if (remaining_ms <= 0) return false;
    job->request_id = fetch_scheduler_enqueue(
        job->scheduler, job->fetch_url, &request,
        job->maximum_source_bytes, remaining_ms);
    if (job->request_id == 0) return false;
    job->phase = YOUTUBE_LITE_JOB_PRIMARY;
    job->metrics.requests_started++;
    return true;
}

static bool lite_load_enqueue_supplemental(YoutubeLiteLoadJob *job);

YoutubeLiteLoadJob *youtube_lite_load_begin_configured(
    Budget *budget, BrowserSession *session, const char *url,
    bool compact_results, size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    YoutubeLiteRoute route = youtube_lite_route(url);
    if (budget == NULL || session == NULL || url == NULL
        || route == YOUTUBE_LITE_ROUTE_NONE || maximum_source_bytes == 0
        || timeout_ms <= 0
        || strlen(url) >= TILEFINCH_URL_SERIALIZED_LIMIT) {
        lite_error(error, error_size, "invalid YouTube lite load");
        return NULL;
    }
    if (maximum_source_bytes > YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES)
        maximum_source_bytes = YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES;
    YoutubeLiteLoadJob *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*job));
    if (job == NULL) {
        lite_error(error, error_size,
                   "YouTube lightweight load exceeded its memory bound");
        return NULL;
    }
    job->budget = budget;
    job->session = session;
    job->route = route;
    job->phase = YOUTUBE_LITE_JOB_PRIMARY;
    job->status = YOUTUBE_LITE_LOAD_PENDING;
    job->timeout_ms = timeout_ms;
    job->maximum_source_bytes = maximum_source_bytes;
    job->compact_results = compact_results;
    job->started_us = tilefinch_platform_monotonic_time_us();
    job->source.budget = budget;
    snprintf(job->url, sizeof(job->url), "%s", url);
    snprintf(job->language, sizeof(job->language), "%s",
             lite_preferred_language());
    if (!lite_fetch_url(url, route, job->fetch_url)) {
        lite_error(error, error_size, "invalid YouTube search query");
        youtube_lite_load_destroy(job);
        return NULL;
    }
    char preference_cookie[64];
    int preference_length = snprintf(
        preference_cookie, sizeof(preference_cookie),
        "PREF=hl=%s&tz=UTC; Domain=.youtube.com; Path=/", job->language);
    if (preference_length > 0
        && (size_t) preference_length < sizeof(preference_cookie)) {
        (void) browser_session_cookie_set(
            session, job->fetch_url, preference_cookie);
    }
    (void) browser_session_cookie_set(
        session, job->fetch_url,
        "SOCS=CAI; Domain=.youtube.com; Path=/; Secure");
    size_t reservation = maximum_source_bytes;
    if (reservation < YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES)
        reservation = YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES;
    char scheduler_error[96] = {0};
    job->scheduler = fetch_scheduler_create_ex(
        budget, 1, reservation, scheduler_error, sizeof(scheduler_error));
    if (job->scheduler == NULL) {
        char detail[160];
        snprintf(detail, sizeof(detail), "YouTube network startup failed: %s",
                 scheduler_error[0] == '\0'
                     ? "scheduler unavailable" : scheduler_error);
        lite_error(error, error_size, detail);
        youtube_lite_load_destroy(job);
        return NULL;
    }
    /* HTML/API parsing, identity caching and supplemental-request policy stay
       in the cooperative provider job. On PSP only the already-authorized
       hop's DNS/TLS/body production moves to the bounded transport worker. */
    (void) fetch_scheduler_enable_background_transport(
        job->scheduler, true);
    job->supplemental_requested =
        route == YOUTUBE_LITE_ROUTE_WATCH
        && lite_comments_view_requested(url);
    char search_token[YOUTUBE_LITE_CONTINUATION_LIMIT] = {0};
    job->supplemental_requested =
        job->supplemental_requested ||
        (route == YOUTUBE_LITE_ROUTE_SEARCH
         && lite_query_value(
             url, "tilefinch_token", search_token, sizeof(search_token)));
    bool can_skip_primary =
        route == YOUTUBE_LITE_ROUTE_SEARCH && search_token[0] != '\0'
        && lite_identity_cache_get(session, &job->identity);
    if (can_skip_primary) {
        job->identity_available = true;
        job->direct_continuation = true;
        if (lite_load_enqueue_supplemental(job)) {
            job->phase = YOUTUBE_LITE_JOB_SUPPLEMENTAL;
            return job;
        }
        browser_session_site_adapter_state_remove(
            session, YOUTUBE_LITE_IDENTITY_CACHE_KEY);
        job->identity_available = false;
        job->direct_continuation = false;
    }
    if (!lite_load_enqueue_primary(job)) {
        char detail[160];
        snprintf(
            detail, sizeof(detail), "YouTube request could not start: %s",
            fetch_scheduler_last_error(job->scheduler));
        lite_error(error, error_size, detail);
        youtube_lite_load_destroy(job);
        return NULL;
    }
    return job;
}

YoutubeLiteLoadJob *youtube_lite_load_begin(
    Budget *budget, BrowserSession *session, const char *url,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size)
{
    return youtube_lite_load_begin_configured(
        budget, session, url, false, maximum_source_bytes, timeout_ms,
        error, error_size);
}

static void lite_metrics_add_pump(
    YoutubeLiteLoadMetrics *metrics, const FetchPumpMetrics *pump)
{
    metrics->network_pumps++;
    metrics->body_bytes += pump->body_bytes;
    metrics->body_callbacks += pump->body_callbacks;
    metrics->network_us += pump->elapsed_us;
    if (pump->peak_buffered_bytes > metrics->peak_buffered_bytes)
        metrics->peak_buffered_bytes = pump->peak_buffered_bytes;
    if (pump->elapsed_us > metrics->maximum_pump_us)
        metrics->maximum_pump_us = pump->elapsed_us;
    if (pump->quota_yielded) metrics->quota_yields++;
}

static bool lite_load_enqueue_supplemental(YoutubeLiteLoadJob *job)
{
    bool comments = job->route == YOUTUBE_LITE_ROUTE_WATCH;
    YoutubeLiteSupplementalRequest *prepared = budget_calloc_category(
        job->budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*prepared));
    if (prepared == NULL) return false;
    if (!lite_prepare_supplemental_request(
            job->session, job->url,
            job->identity_available ? &job->identity : NULL,
            job->language, comments, job->continuation_token, prepared)) {
        budget_free(job->budget, prepared);
        return false;
    }
    job->supplemental = budget_calloc_category(
        job->budget, BUDGET_CATEGORY_RESOURCE,
        1, sizeof(*job->supplemental));
    if (job->supplemental == NULL) {
        budget_free(job->budget, prepared);
        return false;
    }
    job->supplemental->budget = job->budget;
    long remaining_ms = lite_load_remaining_timeout_ms(job);
    if (remaining_ms <= 0) {
        budget_free(job->budget, job->supplemental);
        job->supplemental = NULL;
        budget_free(job->budget, prepared);
        return false;
    }
    job->request_id = fetch_scheduler_enqueue(
        job->scheduler, prepared->endpoint, &prepared->request,
        YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES, remaining_ms);
    if (job->request_id == 0) {
        budget_free(job->budget, job->supplemental);
        job->supplemental = NULL;
        budget_free(job->budget, prepared);
        return false;
    }
    snprintf(job->supplemental_url, sizeof(job->supplemental_url), "%s",
             prepared->endpoint);
    budget_free(job->budget, prepared);
    job->metrics.requests_started++;
    return true;
}

static bool lite_load_fallback_to_primary(YoutubeLiteLoadJob *job)
{
    if (job == NULL || !job->direct_continuation
        || job->fallback_attempted) return false;
    if (job->supplemental != NULL) {
        fetch_result_destroy(job->supplemental);
        budget_free(job->budget, job->supplemental);
        job->supplemental = NULL;
    }
    lite_build_work_destroy(job->build);
    job->build = NULL;
    youtube_lite_document_destroy(&job->document);
    browser_session_site_adapter_state_remove(
        job->session, YOUTUBE_LITE_IDENTITY_CACHE_KEY);
    job->fallback_attempted = true;
    job->direct_continuation = false;
    job->supplemental_fetched = false;
    job->identity_available = false;
    job->identity = (YoutubeLiteIdentity) {0};
    job->fact_scan_offset = 0;
    job->fact_scan_limit = 0;
    job->continuation_token[0] = '\0';
    if (!lite_load_enqueue_primary(job)) {
        lite_load_job_fail(
            job, "YouTube continuation fallback request could not start");
    }
    return true;
}

YoutubeLiteLoadStatus youtube_lite_load_pump(
    YoutubeLiteLoadJob *job, const FetchPumpQuota *quota)
{
    if (job == NULL) return YOUTUBE_LITE_LOAD_FAILED;
    if (job->status != YOUTUBE_LITE_LOAD_PENDING) return job->status;
    job->metrics.pump_calls++;
    bool identity_scan =
        job->phase == YOUTUBE_LITE_JOB_IDENTITY_API_KEY;
    bool comments_scan =
        job->phase == YOUTUBE_LITE_JOB_COMMENTS_PANEL
        || job->phase == YOUTUBE_LITE_JOB_COMMENTS_MARKER
        || job->phase == YOUTUBE_LITE_JOB_COMMENTS_COMMAND
        || job->phase == YOUTUBE_LITE_JOB_COMMENTS_TOKEN;
    if (identity_scan || comments_scan) {
        uint64_t started_us = tilefinch_platform_monotonic_time_us();
        job->metrics.build_slices++;
        bool advanced = identity_scan
            ? lite_load_identity_pump(job)
            : lite_load_comments_scan_pump(job);
        uint64_t finished_us = tilefinch_platform_monotonic_time_us();
        uint64_t elapsed_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        job->metrics.build_us += elapsed_us;
        if (elapsed_us > job->metrics.maximum_transform_slice_us)
            job->metrics.maximum_transform_slice_us = elapsed_us;
        if (quota != NULL && quota->maximum_time_us != 0
            && elapsed_us > quota->maximum_time_us) {
            job->metrics.transform_quota_overruns++;
        }
        if (!advanced) {
            lite_load_job_fail(job, "YouTube response fact scan failed");
        }
        return job->status;
    }
    if (job->phase == YOUTUBE_LITE_JOB_BUILD) {
        uint64_t started_us = tilefinch_platform_monotonic_time_us();
        job->metrics.build_slices++;
        if (job->build == NULL) {
            job->build = lite_build_work_create(
                job->budget, job->url,
                job->source.data, job->source.length,
                job->primary_source_bytes,
                job->supplemental_fetched
                    ? job->supplemental->data : NULL,
                job->supplemental_fetched
                    ? job->supplemental->length : 0,
                job->decoded, job->decoded_length,
                job->compact_results,
                job->error, sizeof(job->error));
        } else {
            lite_build_work_pump(job->build);
        }
        uint64_t finished_us = tilefinch_platform_monotonic_time_us();
        uint64_t elapsed_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        job->metrics.build_us += elapsed_us;
        if (elapsed_us > job->metrics.maximum_transform_slice_us)
            job->metrics.maximum_transform_slice_us = elapsed_us;
        if (quota != NULL && quota->maximum_time_us != 0
            && elapsed_us > quota->maximum_time_us) {
            job->metrics.transform_quota_overruns++;
        }
        if (job->build != NULL
            && job->build->phase != YOUTUBE_LITE_BUILD_DONE
            && job->build->phase != YOUTUBE_LITE_BUILD_FAILED) {
            return YOUTUBE_LITE_LOAD_PENDING;
        }
        bool built = job->build != NULL
            && lite_build_work_take_document(
                job->build, &job->document);
        if (built) {
            const FetchResult *metadata =
                job->direct_continuation && job->supplemental_fetched
                    ? job->supplemental : NULL;
            job->document.status_code = metadata != NULL
                ? metadata->status_code : job->primary_status_code;
            snprintf(
                job->document.server, sizeof(job->document.server), "%s",
                metadata != NULL ? metadata->server : job->primary_server);
            snprintf(
                job->document.cf_mitigated,
                sizeof(job->document.cf_mitigated), "%s",
                metadata != NULL
                    ? metadata->cf_mitigated : job->primary_cf_mitigated);
            job->status = YOUTUBE_LITE_LOAD_SUCCEEDED;
        } else if (job->direct_continuation
                   && lite_load_fallback_to_primary(job)) {
            return job->status;
        } else {
            if (job->error[0] == '\0')
                snprintf(
                    job->error, sizeof(job->error),
                    "YouTube cooperative build failed");
            job->status = YOUTUBE_LITE_LOAD_FAILED;
        }
        lite_build_work_destroy(job->build);
        job->build = NULL;
        if (job->supplemental != NULL) {
            fetch_result_destroy(job->supplemental);
            budget_free(job->budget, job->supplemental);
            job->supplemental = NULL;
        }
        fetch_result_destroy(&job->source);
        return job->status;
    }
    if (job->phase == YOUTUBE_LITE_JOB_DECODE) {
        uint64_t started_us = tilefinch_platform_monotonic_time_us();
        bool advanced = lite_load_decode_pump(job);
        uint64_t finished_us = tilefinch_platform_monotonic_time_us();
        uint64_t elapsed_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        job->metrics.build_us += elapsed_us;
        if (elapsed_us > job->metrics.maximum_transform_slice_us)
            job->metrics.maximum_transform_slice_us = elapsed_us;
        if (!advanced) {
            lite_load_job_fail(
                job, "YouTube initial data decode failed");
            return job->status;
        }
        return YOUTUBE_LITE_LOAD_PENDING;
    }
    if (job->phase == YOUTUBE_LITE_JOB_PREPARE) {
        uint64_t started_us = tilefinch_platform_monotonic_time_us();
        bool supplemental_enqueued = job->supplemental_requested
            && lite_load_enqueue_supplemental(job);
        uint64_t finished_us = tilefinch_platform_monotonic_time_us();
        uint64_t elapsed_us = finished_us >= started_us
            ? finished_us - started_us : 0;
        if (elapsed_us > job->metrics.maximum_irreducible_unit_us)
            job->metrics.maximum_irreducible_unit_us = elapsed_us;
        if (supplemental_enqueued) {
            job->phase = YOUTUBE_LITE_JOB_SUPPLEMENTAL;
        } else {
            if (job->supplemental_requested) {
                fprintf(
                    stderr, "youtube-lite supplemental data unavailable: "
                    "continuation/configuration missing or request rejected\n");
            }
            job->phase =
                job->route != YOUTUBE_LITE_ROUTE_HOME
                && job->decoded == NULL
                && job->source.data != NULL
                && !job->decode_attempted
                    ? YOUTUBE_LITE_JOB_DECODE
                    : YOUTUBE_LITE_JOB_BUILD;
            if (job->phase == YOUTUBE_LITE_JOB_BUILD)
                lite_load_release_unused_primary(job);
        }
        return YOUTUBE_LITE_LOAD_PENDING;
    }

    FetchPumpMetrics pump = {0};
    (void) fetch_scheduler_pump_bounded(
        job->scheduler, 1, 1, quota, &pump);
    lite_metrics_add_pump(&job->metrics, &pump);
    bool fetched = false;
    FetchResult *result = job->phase == YOUTUBE_LITE_JOB_PRIMARY
        ? &job->source : job->supplemental;
    if (result == NULL) {
        lite_load_job_fail(job, "YouTube load result storage is missing");
        return job->status;
    }
    if (!fetch_scheduler_take(
            job->scheduler, job->request_id, &fetched, result)) {
        return YOUTUBE_LITE_LOAD_PENDING;
    }
    job->request_id = 0;
    job->metrics.requests_completed++;
    if (!fetched) {
        if (job->phase == YOUTUBE_LITE_JOB_SUPPLEMENTAL) {
            fprintf(stderr, "youtube-lite supplemental data unavailable: %s\n",
                    result->error[0] == '\0'
                        ? "transport failure" : result->error);
            if (lite_load_fallback_to_primary(job))
                return job->status;
            fetch_result_destroy(result);
            budget_free(job->budget, job->supplemental);
            job->supplemental = NULL;
            job->phase = YOUTUBE_LITE_JOB_BUILD;
            lite_load_release_unused_primary(job);
            return YOUTUBE_LITE_LOAD_PENDING;
        }
        lite_load_job_fail(
            job, "YouTube page fetch failed: %s",
            result->error[0] == '\0'
                ? "transport failure" : result->error);
        fetch_result_destroy(result);
        return job->status;
    }

    if (job->phase == YOUTUBE_LITE_JOB_PRIMARY) {
        TilefinchRequestContext context =
            lite_primary_context(job->url, job->fetch_url);
        lite_accept_cookies(
            job->session, &context, &job->source, job->fetch_url);
        job->primary_source_bytes = job->source.length;
        job->primary_status_code = job->source.status_code;
        snprintf(
            job->primary_server, sizeof(job->primary_server), "%s",
            job->source.server);
        snprintf(
            job->primary_cf_mitigated,
            sizeof(job->primary_cf_mitigated), "%s",
            job->source.cf_mitigated);
        job->identity = (YoutubeLiteIdentity) {
            .version = YOUTUBE_LITE_IDENTITY_CACHE_VERSION
        };
        job->fact_scan_offset = 0;
        job->fact_scan_limit = job->source.length;
        job->phase = YOUTUBE_LITE_JOB_IDENTITY_API_KEY;
        return YOUTUBE_LITE_LOAD_PENDING;
    }

    if (job->direct_continuation
        && (job->supplemental->status_code < 200
            || job->supplemental->status_code >= 300)) {
        if (lite_load_fallback_to_primary(job))
            return job->status;
        lite_load_job_fail(
            job, "YouTube continuation request was rejected");
        return job->status;
    }
    job->supplemental_fetched = true;
    TilefinchRequestContext context =
        lite_supplemental_context(job->url, job->supplemental_url);
    lite_accept_cookies(
        job->session, &context, job->supplemental,
        job->supplemental_url);
    if (job->identity_available)
        (void) lite_identity_cache_put(job->session, &job->identity);
    job->phase = YOUTUBE_LITE_JOB_BUILD;
    lite_load_release_unused_primary(job);
    return YOUTUBE_LITE_LOAD_PENDING;
}

YoutubeLiteLoadStatus youtube_lite_load_status(
    const YoutubeLiteLoadJob *job)
{
    return job == NULL ? YOUTUBE_LITE_LOAD_FAILED : job->status;
}

void youtube_lite_load_cancel(YoutubeLiteLoadJob *job,
                              const char *reason)
{
    if (job == NULL || job->status != YOUTUBE_LITE_LOAD_PENDING) return;
    const char *message = reason == NULL
        ? "YouTube lightweight load cancelled" : reason;
    if (job->request_id != 0) {
        (void) fetch_scheduler_cancel(
            job->scheduler, job->request_id, message);
    }
    snprintf(job->error, sizeof(job->error), "%s", message);
    job->status = YOUTUBE_LITE_LOAD_CANCELLED;
}

bool youtube_lite_load_take_document(
    YoutubeLiteLoadJob *job, YoutubeLiteDocument *document)
{
    if (job == NULL || document == NULL
        || job->status != YOUTUBE_LITE_LOAD_SUCCEEDED
        || job->document_taken) return false;
    *document = job->document;
    job->document = (YoutubeLiteDocument) {0};
    job->document_taken = true;
    return true;
}

bool youtube_lite_load_metrics(
    const YoutubeLiteLoadJob *job, YoutubeLiteLoadMetrics *metrics)
{
    if (job == NULL || metrics == NULL) return false;
    *metrics = job->metrics;
    if (job->status == YOUTUBE_LITE_LOAD_SUCCEEDED) {
        metrics->completion_per_mille = 1000;
        return true;
    }
    if (job->status != YOUTUBE_LITE_LOAD_PENDING) return true;
    switch (job->phase) {
    case YOUTUBE_LITE_JOB_PRIMARY: {
        size_t received = job->metrics.body_bytes;
        const size_t horizon = 256u * 1024u;
        size_t denominator = received > SIZE_MAX - horizon
            ? SIZE_MAX : received + horizon;
        size_t advance = received > SIZE_MAX / 400u
            ? 400u : received * 400u / denominator;
        metrics->completion_per_mille = 30u + advance;
        break;
    }
    case YOUTUBE_LITE_JOB_IDENTITY_API_KEY: {
        size_t advance = job->source.length == 0
            ? 0 : job->fact_scan_offset * 30u / job->source.length;
        metrics->completion_per_mille = 420u + advance;
        break;
    }
    case YOUTUBE_LITE_JOB_DECODE: {
        size_t offset = job->decode_input_offset > job->decode_search_offset
            ? job->decode_input_offset : job->decode_search_offset;
        if (offset > job->source.length) offset = job->source.length;
        size_t advance = job->source.length == 0
            ? 0 : offset * 150u / job->source.length;
        metrics->completion_per_mille = 450u + advance;
        break;
    }
    case YOUTUBE_LITE_JOB_COMMENTS_PANEL:
    case YOUTUBE_LITE_JOB_COMMENTS_MARKER:
    case YOUTUBE_LITE_JOB_COMMENTS_COMMAND:
    case YOUTUBE_LITE_JOB_COMMENTS_TOKEN:
        metrics->completion_per_mille = 610;
        break;
    case YOUTUBE_LITE_JOB_PREPARE:
        metrics->completion_per_mille = 620;
        break;
    case YOUTUBE_LITE_JOB_SUPPLEMENTAL: {
        size_t supplemental =
            job->metrics.body_bytes > job->primary_source_bytes
            ? job->metrics.body_bytes - job->primary_source_bytes : 0;
        const size_t horizon = 64u * 1024u;
        size_t denominator = supplemental > SIZE_MAX - horizon
            ? SIZE_MAX : supplemental + horizon;
        size_t advance = supplemental > SIZE_MAX / 150u
            ? 150u : supplemental * 150u / denominator;
        metrics->completion_per_mille = 650u + advance;
        break;
    }
    case YOUTUBE_LITE_JOB_BUILD: {
        size_t phase = job->build == NULL
            ? 0u : (size_t) job->build->phase;
        if (phase > (size_t) YOUTUBE_LITE_BUILD_DONE)
            phase = (size_t) YOUTUBE_LITE_BUILD_DONE;
        metrics->completion_per_mille =
            820u + phase * 170u / (size_t) YOUTUBE_LITE_BUILD_DONE;
        break;
    }
    default:
        break;
    }
    return true;
}

const char *youtube_lite_load_error(const YoutubeLiteLoadJob *job)
{
    return job == NULL ? "YouTube lightweight load is null" : job->error;
}

void youtube_lite_load_destroy(YoutubeLiteLoadJob *job)
{
    if (job == NULL) return;
    Budget *budget = job->budget;
    if (job->status == YOUTUBE_LITE_LOAD_PENDING)
        youtube_lite_load_cancel(job, "YouTube lightweight load destroyed");
    fetch_scheduler_destroy(job->scheduler);
    if (job->supplemental != NULL) {
        fetch_result_destroy(job->supplemental);
        budget_free(job->budget, job->supplemental);
    }
    fetch_result_destroy(&job->source);
    if (job->decoded != NULL) budget_free(job->budget, job->decoded);
    lite_build_work_destroy(job->build);
    youtube_lite_document_destroy(&job->document);
    budget_free(budget, job);
}

bool youtube_lite_load(
    Budget *budget, BrowserSession *session, const char *url,
    size_t maximum_source_bytes, long timeout_ms,
    YoutubeLiteDocument *document, char *error, size_t error_size)
{
    if (document == NULL) {
        lite_error(error, error_size, "invalid YouTube lite load");
        return false;
    }
    *document = (YoutubeLiteDocument) {0};
    YoutubeLiteLoadJob *job = youtube_lite_load_begin(
        budget, session, url, maximum_source_bytes, timeout_ms,
        error, error_size);
    if (job == NULL) return false;
    YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
    while (status == YOUTUBE_LITE_LOAD_PENDING)
        status = youtube_lite_load_pump(job, NULL);
    bool loaded = status == YOUTUBE_LITE_LOAD_SUCCEEDED
        && youtube_lite_load_take_document(job, document);
    if (!loaded) {
        lite_error(error, error_size, "%s",
                   youtube_lite_load_error(job));
    }
    youtube_lite_load_destroy(job);
    return loaded;
}

void youtube_lite_document_destroy(YoutubeLiteDocument *document)
{
    if (document == NULL) return;
    if (document->budget != NULL && document->html != NULL)
        budget_free(document->budget, document->html);
    *document = (YoutubeLiteDocument) {0};
}
