#include "tilefinch/youtube_resolver.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/request_context.h"
#include "tilefinch/url.h"
#include "tilefinch/youtube_lite.h"

/*
 * The watch document is fetched only to extract three early config strings --
 * VISITOR_DATA, INNERTUBE_API_KEY and STS -- which sit in the initial ytcfg
 * block, before the large ytInitialPlayerResponse JSON (that comes from a
 * separate player fetch). This cap is a runaway-response guard, not a page-size
 * budget: it must clear the real page with room, not fit it snugly.
 *
 * Two MiB did not. A device cycle observed watch HTML at ~1.84 MiB, and
 * YouTube's A/B experiments vary the embedded JSON enough to cross 2 MiB
 * intermittently, so both resolve attempts in one session lost the coin flip
 * and playback never started. Six MiB is roughly three times the observed size
 * and well clear of that variance. The buffer is freed the moment the three
 * strings are read (below, before the player fetch), so this bounds a transient
 * peak, not steady state, and does not overlap the player response.
 *
 * A streaming scan that stopped once the three keys were found would cap this
 * regardless of page size and retire the class -- the values are first
 * occurrence and early -- but it rests on STS never moving after the stop
 * point across A/B variants, which cannot be proven off-device. The cap is the
 * unconditional fix; streaming stays a backstopped follow-up for when there is
 * device evidence of where the three land.
 */
#define YOUTUBE_WATCH_MAXIMUM_BYTES (6u * 1024u * 1024u)
#define YOUTUBE_PLAYER_MAXIMUM_BYTES (2u * 1024u * 1024u)
#define YOUTUBE_JSON_MAXIMUM_DEPTH 48u
#define YOUTUBE_UNATTESTED_DELIVERY_MAXIMUM_BYTES \
    (UINT64_C(2) * 1024u * 1024u)

#define YOUTUBE_BROWSER_UA \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " \
    "AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/136.0.0.0 Safari/537.36"
#define YOUTUBE_RESOLVER_MAXIMUM_CLIENTS 3u
#define YOUTUBE_EXPIRY_MARGIN_SECONDS 300u
/*
 * The ladder is up to five fetches (three client profiles, the watch
 * document, one enriched player request) under a single phase deadline. Each
 * one used to inherit the whole remaining phase budget, so the first attempt
 * could spend all thirty seconds and every later attempt then found the
 * deadline gone. Give each attempt its own ceiling: the ladder still ends at
 * the phase deadline, but no single attempt can consume it, and the request
 * deadline that curl derives its own bounds from stays proportionate to what
 * one attempt is actually worth. */
#define YOUTUBE_RESOLVER_ATTEMPT_TIMEOUT_MS 10000L
/*
 * Resolution runs on the interactive thread. Bound the connect phase, in
 * which libcurl never calls the progress callback and therefore never polls
 * our cancellation callback, to the same three seconds media range reads use.
 */
#define YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS 3000L

static long youtube_attempt_timeout_ms(long remaining_ms)
{
    return remaining_ms > YOUTUBE_RESOLVER_ATTEMPT_TIMEOUT_MS
        ? YOUTUBE_RESOLVER_ATTEMPT_TIMEOUT_MS : remaining_ms;
}

typedef struct {
    const char *name;
    const char *version;
    const char *header_id;
    const char *user_agent;
    const char *context_fields;
    const char *context_extra;
    bool direct_delivery_requires_enrichment;
} YoutubeClientProfile;

/*
 * Ordered data, deliberately separate from the request machinery. The iOS
 * profile remains a cheap first attempt for clips whose complete selected
 * resources fit inside its current unattested delivery allowance. Larger
 * iOS and Android VR resources are admitted only after the bounded watch-page
 * fallback supplies the current visitor/key/STS context. Profile changes ship
 * as part of a signed Tilefinch release; mutable page data cannot rewrite
 * native request identities.
 */
static const YoutubeClientProfile youtube_client_profiles[] = {
    {
        "IOS", "20.10.4", "5",
        "com.google.ios.youtube/20.10.4 "
        "(iPhone16,2; U; CPU iOS 18_3 like Mac OS X;)",
        "\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\","
        "\"osName\":\"iPhone\",\"osVersion\":\"18.3.1.22D72\"",
        "",
        true
    },
    {
        "WEB_EMBEDDED_PLAYER", "2.20260708.00.00", "56",
        YOUTUBE_BROWSER_UA,
        "\"clientScreen\":\"EMBED\"",
        ",\"thirdParty\":{\"embedUrl\":\"https://www.reddit.com/\"}",
        false
    },
    {
        "ANDROID_VR", "1.65.10", "28",
        "com.google.android.apps.youtube.vr.oculus/1.65.10 "
        "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
        "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\","
        "\"androidSdkVersion\":32,\"osName\":\"Android\","
        "\"osVersion\":\"12L\"",
        "",
        true
    }
};
_Static_assert(
    sizeof(youtube_client_profiles) / sizeof(youtube_client_profiles[0])
        <= YOUTUBE_RESOLVER_MAXIMUM_CLIENTS,
    "YouTube client ladder exceeds its bounded attempt policy");

typedef struct {
    const char *at;
    const char *end;
} YoutubeJson;

typedef struct {
    int itag;
    int width;
    int height;
    int audio_channels;
    uint64_t duration_ms;
    uint64_t content_length;
    uint64_t bitrate;
    char url[YOUTUBE_MEDIA_URL_CAPACITY];
    char mime[YOUTUBE_MIME_CAPACITY];
} YoutubeFormat;

typedef struct {
    YoutubeStream parsed;
    YoutubeFormat selected;
    YoutubeFormat selected_audio;
    YoutubeFormat adaptive_video;
} YoutubePlayerParseScratch;

static bool youtube_header_value_safe(const char *value)
{
    if (value == NULL) return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (*at < 0x20u || *at >= 0x7fu) return false;
    }
    return true;
}

bool youtube_direct_delivery_admitted(
    const char *client_name, const YoutubeStream *stream)
{
    if (client_name == NULL || stream == NULL) return false;
    const YoutubeClientProfile *profile = NULL;
    for (size_t i = 0;
         i < sizeof(youtube_client_profiles)
               / sizeof(youtube_client_profiles[0]);
         i++) {
        if (strcmp(youtube_client_profiles[i].name, client_name) == 0) {
            profile = &youtube_client_profiles[i];
            break;
        }
    }
    if (profile == NULL) return false;
    if (!profile->direct_delivery_requires_enrichment) return true;
    if (stream->content_length == 0
        || stream->content_length
             > YOUTUBE_UNATTESTED_DELIVERY_MAXIMUM_BYTES) {
        return false;
    }
    return !stream->split_streams
        || (stream->audio_content_length != 0
            && stream->audio_content_length
                 <= YOUTUBE_UNATTESTED_DELIVERY_MAXIMUM_BYTES);
}

static bool youtube_api_key_safe(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (!isalnum(*at) && *at != '-' && *at != '_') return false;
    }
    return true;
}

static void youtube_error(char *error, size_t error_size,
                          const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void json_space(YoutubeJson *json)
{
    while (json->at < json->end
           && isspace((unsigned char) *json->at)) json->at++;
}

static bool json_hex(char digit, unsigned *value)
{
    if (digit >= '0' && digit <= '9') *value = (unsigned) (digit - '0');
    else if (digit >= 'a' && digit <= 'f')
        *value = (unsigned) (digit - 'a' + 10);
    else if (digit >= 'A' && digit <= 'F')
        *value = (unsigned) (digit - 'A' + 10);
    else return false;
    return true;
}

static bool json_append_utf8(char *output, size_t output_size,
                             size_t *length, unsigned codepoint)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7fu) {
        bytes[count++] = (unsigned char) codepoint;
    } else if (codepoint <= 0x7ffu) {
        bytes[count++] = (unsigned char) (0xc0u | (codepoint >> 6));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0xffffu) {
        if (codepoint >= 0xd800u && codepoint <= 0xdfffu) return false;
        bytes[count++] = (unsigned char) (0xe0u | (codepoint >> 12));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0x10ffffu) {
        bytes[count++] = (unsigned char) (0xf0u | (codepoint >> 18));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else {
        return false;
    }
    if (*length > output_size - 1u
        || count > output_size - 1u - *length) return false;
    for (size_t i = 0; i < count; i++) output[(*length)++] = (char) bytes[i];
    return true;
}

static bool json_string(YoutubeJson *json, char *output, size_t output_size)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '"'
        || output == NULL || output_size == 0) return false;
    size_t length = 0;
    while (json->at < json->end) {
        unsigned char byte = (unsigned char) *json->at++;
        if (byte == '"') {
            output[length] = '\0';
            return true;
        }
        if (byte < 0x20u) return false;
        if (byte != '\\') {
            if (length >= output_size - 1u) return false;
            output[length++] = (char) byte;
            continue;
        }
        if (json->at >= json->end) return false;
        char escape = *json->at++;
        char decoded = '\0';
        switch (escape) {
        case '"': decoded = '"'; break;
        case '\\': decoded = '\\'; break;
        case '/': decoded = '/'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case 'n': decoded = '\n'; break;
        case 'r': decoded = '\r'; break;
        case 't': decoded = '\t'; break;
        case 'u': {
            if ((size_t) (json->end - json->at) < 4u) return false;
            unsigned codepoint = 0;
            for (size_t i = 0; i < 4; i++) {
                unsigned nibble = 0;
                if (!json_hex(json->at[i], &nibble)) return false;
                codepoint = (codepoint << 4) | nibble;
            }
            json->at += 4;
            if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                if ((size_t) (json->end - json->at) < 6u
                    || json->at[0] != '\\' || json->at[1] != 'u') return false;
                unsigned low = 0;
                for (size_t i = 0; i < 4; i++) {
                    unsigned nibble = 0;
                    if (!json_hex(json->at[i + 2], &nibble)) return false;
                    low = (low << 4) | nibble;
                }
                if (low < 0xdc00u || low > 0xdfffu) return false;
                json->at += 6;
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10)
                          + (low - 0xdc00u);
            }
            if (!json_append_utf8(
                    output, output_size, &length, codepoint)) return false;
            continue;
        }
        default:
            return false;
        }
        if (length >= output_size - 1u) return false;
        output[length++] = decoded;
    }
    return false;
}

static bool json_skip_value(YoutubeJson *json, unsigned depth);
static bool json_skip_string(YoutubeJson *json);

static bool json_skip_compound(YoutubeJson *json, char open, char close,
                               unsigned depth)
{
    if (depth >= YOUTUBE_JSON_MAXIMUM_DEPTH
        || json->at >= json->end || *json->at++ != open) return false;
    json_space(json);
    if (json->at < json->end && *json->at == close) {
        json->at++;
        return true;
    }
    while (json->at < json->end) {
        if (open == '{') {
            if (!json_skip_string(json)) return false;
            json_space(json);
            if (json->at >= json->end || *json->at++ != ':') return false;
        }
        if (!json_skip_value(json, depth + 1u)) return false;
        json_space(json);
        if (json->at < json->end && *json->at == close) {
            json->at++;
            return true;
        }
        if (json->at >= json->end || *json->at++ != ',') return false;
        json_space(json);
    }
    return false;
}

static bool json_skip_string(YoutubeJson *json)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '"') return false;
    while (json->at < json->end) {
        unsigned char byte = (unsigned char) *json->at++;
        if (byte == '"') return true;
        if (byte < 0x20u) return false;
        if (byte != '\\') continue;
        if (json->at >= json->end) return false;
        char escape = *json->at++;
        if (strchr("\"\\/bfnrt", escape) != NULL) continue;
        if (escape != 'u' || (size_t) (json->end - json->at) < 4u)
            return false;
        for (size_t i = 0; i < 4; i++) {
            unsigned value = 0;
            if (!json_hex(json->at[i], &value)) return false;
        }
        json->at += 4;
    }
    return false;
}

static bool json_skip_value(YoutubeJson *json, unsigned depth)
{
    json_space(json);
    if (depth >= YOUTUBE_JSON_MAXIMUM_DEPTH || json->at >= json->end)
        return false;
    if (*json->at == '"') return json_skip_string(json);
    if (*json->at == '{')
        return json_skip_compound(json, '{', '}', depth);
    if (*json->at == '[')
        return json_skip_compound(json, '[', ']', depth);
    const char *start = json->at;
    while (json->at < json->end
           && strchr(",]} \t\r\n", *json->at) == NULL) json->at++;
    if (json->at == start) return false;
    size_t length = (size_t) (json->at - start);
    if ((length == 4 && memcmp(start, "true", 4) == 0)
        || (length == 5 && memcmp(start, "false", 5) == 0)
        || (length == 4 && memcmp(start, "null", 4) == 0)) return true;
    char number[64];
    if (length >= sizeof(number)) return false;
    memcpy(number, start, length);
    number[length] = '\0';
    char *end = NULL;
    (void) strtod(number, &end);
    return end != NULL && *end == '\0';
}

static bool json_unsigned_value(YoutubeJson *json, uint64_t *value)
{
    json_space(json);
    char text[32];
    if (json->at < json->end && *json->at == '"') {
        if (!json_string(json, text, sizeof(text))) return false;
    } else {
        const char *start = json->at;
        while (json->at < json->end
               && isdigit((unsigned char) *json->at)) json->at++;
        size_t length = (size_t) (json->at - start);
        if (length == 0 || length >= sizeof(text)) return false;
        memcpy(text, start, length);
        text[length] = '\0';
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == NULL || *end != '\0') return false;
    *value = (uint64_t) parsed;
    return true;
}

static bool json_member_key(YoutubeJson *json, char *key, size_t key_size)
{
    if (!json_string(json, key, key_size)) return false;
    json_space(json);
    return json->at < json->end && *json->at++ == ':';
}

static bool youtube_parse_format(YoutubeJson *json, YoutubeFormat *format)
{
    memset(format, 0, sizeof(*format));
    json_space(json);
    if (json->at >= json->end || *json->at++ != '{') return false;
    json_space(json);
    if (json->at < json->end && *json->at == '}') {
        json->at++;
        return true;
    }
    while (json->at < json->end) {
        char key[64];
        if (!json_member_key(json, key, sizeof(key))) return false;
        uint64_t number = 0;
        if (strcmp(key, "url") == 0) {
            if (!json_string(json, format->url, sizeof(format->url)))
                return false;
        } else if (strcmp(key, "mimeType") == 0) {
            if (!json_string(json, format->mime, sizeof(format->mime)))
                return false;
        } else if (strcmp(key, "itag") == 0
                   || strcmp(key, "width") == 0
                   || strcmp(key, "height") == 0
                   || strcmp(key, "bitrate") == 0
                   || strcmp(key, "audioChannels") == 0
                   || strcmp(key, "contentLength") == 0
                   || strcmp(key, "approxDurationMs") == 0) {
            if (!json_unsigned_value(json, &number)) return false;
            if (strcmp(key, "itag") == 0 && number <= INT_MAX)
                format->itag = (int) number;
            else if (strcmp(key, "width") == 0 && number <= INT_MAX)
                format->width = (int) number;
            else if (strcmp(key, "height") == 0 && number <= INT_MAX)
                format->height = (int) number;
            else if (strcmp(key, "bitrate") == 0)
                format->bitrate = number;
            else if (strcmp(key, "audioChannels") == 0
                     && number <= INT_MAX)
                format->audio_channels = (int) number;
            else if (strcmp(key, "contentLength") == 0)
                format->content_length = number;
            else if (strcmp(key, "approxDurationMs") == 0)
                format->duration_ms = number;
        } else if (!json_skip_value(json, 1)) {
            return false;
        }
        json_space(json);
        if (json->at < json->end && *json->at == '}') {
            json->at++;
            return true;
        }
        if (json->at >= json->end || *json->at++ != ',') return false;
        json_space(json);
    }
    return false;
}

static bool youtube_mime_has_exact_codec(
    const char *mime, const char *codec)
{
    if (mime == NULL || codec == NULL || codec[0] == '\0') return false;
    size_t length = strlen(codec);
    const char *at = mime;
    while ((at = strstr(at, codec)) != NULL) {
        unsigned char before = at == mime
            ? '\0' : (unsigned char) at[-1];
        unsigned char after = (unsigned char) at[length];
        bool left_boundary = at == mime || before == '"' || before == ','
            || before == ';' || isspace(before);
        bool right_boundary = after == '\0' || after == '"'
            || after == ',' || after == ';' || isspace(after);
        if (left_boundary && right_boundary) return true;
        at += length;
    }
    return false;
}

static int youtube_ascii_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int) (value - '0');
    value = (unsigned char) tolower(value);
    if (value >= 'a' && value <= 'f') return (int) (value - 'a') + 10;
    return -1;
}

/*
 * The PSP Media Engine path is intentionally narrower than generic AVC:
 * Baseline is admitted only at PSP-screen geometry, Main is admitted through
 * 640x360, and both stop at level 3.0.  YouTube can advertise High-profile or
 * wide Baseline renditions at the same requested height. Selecting one here
 * merely defers a deterministic rejection until after range/demux work and
 * can hide a compatible Main rendition later in the inventory.
 */
static bool youtube_psp_avc_codec_supported(
    const char *mime, int width, int height)
{
    if (mime == NULL || width <= 0 || height <= 0) return false;
    for (const char *at = mime; (at = strstr(at, "avc1.")) != NULL; at++) {
        unsigned char before = at == mime ? '\0' : (unsigned char) at[-1];
        bool left_boundary = at == mime || before == '"' || before == ','
            || before == ';' || isspace(before);
        if (!left_boundary) continue;
        unsigned value = 0;
        bool valid = true;
        for (size_t i = 0; i < 6u; i++) {
            int digit = youtube_ascii_hex((unsigned char) at[5u + i]);
            if (digit < 0) {
                valid = false;
                break;
            }
            value = (value << 4u) | (unsigned) digit;
        }
        if (!valid) continue;
        unsigned char after = (unsigned char) at[11];
        bool right_boundary = after == '\0' || after == '"' || after == ','
            || after == ';' || isspace(after);
        if (!right_boundary) continue;
        unsigned profile = value >> 16u;
        unsigned level = value & 0xffu;
        bool wide = width > 480 || height > 272;
        return level != 0 && level <= 30u
            && (profile == 77u || (profile == 66u && !wide));
    }
    return false;
}

static bool youtube_format_supported(const YoutubeFormat *format,
                                     int maximum_height)
{
    return format->url[0] != '\0'
        && format->content_length != 0
        && format->width > 0 && format->height > 0
        && format->height <= maximum_height
        && strstr(format->mime, "video/mp4") != NULL
        && youtube_psp_avc_codec_supported(
            format->mime, format->width, format->height)
        && youtube_mime_has_exact_codec(
            format->mime, "mp4a.40.2");
}

static bool youtube_video_format_supported(
    const YoutubeFormat *format, int maximum_height)
{
    return format->url[0] != '\0'
        && format->content_length != 0
        && format->width > 0 && format->height > 0
        && format->height <= maximum_height
        && strstr(format->mime, "video/mp4") != NULL
        && youtube_psp_avc_codec_supported(
            format->mime, format->width, format->height)
        && strstr(format->mime, "mp4a") == NULL;
}

static bool youtube_audio_format_supported(const YoutubeFormat *format)
{
    return format->url[0] != '\0'
        && format->content_length != 0
        && strstr(format->mime, "audio/mp4") != NULL
        && youtube_mime_has_exact_codec(
            format->mime, "mp4a.40.2")
        && (format->audio_channels == 0
            || format->audio_channels <= 2);
}

static bool youtube_parse_formats(
    YoutubeJson *json, int maximum_height, bool adaptive,
    YoutubeFormat *selected, YoutubeFormat *selected_audio)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '[') return false;
    json_space(json);
    if (json->at < json->end && *json->at == ']') {
        json->at++;
        return true;
    }
    while (json->at < json->end) {
        YoutubeFormat candidate;
        if (!youtube_parse_format(json, &candidate)) return false;
        if (!adaptive
            && youtube_format_supported(&candidate, maximum_height)
            && (selected->url[0] == '\0'
                || candidate.height > selected->height
                || (candidate.height == selected->height
                    && candidate.bitrate > selected->bitrate))) {
            *selected = candidate;
        } else if (adaptive
                   && youtube_video_format_supported(
                       &candidate, maximum_height)
                   && (selected->url[0] == '\0'
                       || candidate.height > selected->height
                       || (candidate.height == selected->height
                           && candidate.bitrate > selected->bitrate))) {
            *selected = candidate;
        } else if (adaptive && selected_audio != NULL
                   && youtube_audio_format_supported(&candidate)
                   && (selected_audio->url[0] == '\0'
                       || candidate.bitrate > selected_audio->bitrate)) {
            *selected_audio = candidate;
        }
        json_space(json);
        if (json->at < json->end && *json->at == ']') {
            json->at++;
            return true;
        }
        if (json->at >= json->end || *json->at++ != ',') return false;
        json_space(json);
    }
    return false;
}

static bool youtube_find_key(const char *json, size_t length,
                             const char *wanted, YoutubeJson *value)
{
    YoutubeJson cursor = {json, json + length};
    size_t wanted_length = strlen(wanted);
    while (cursor.at < cursor.end) {
        const char *quote = memchr(
            cursor.at, '"', (size_t) (cursor.end - cursor.at));
        if (quote == NULL) return false;
        cursor.at = quote;
        YoutubeJson probe = cursor;
        char key[96];
        if (json_string(&probe, key, sizeof(key))
            && strlen(key) == wanted_length
            && memcmp(key, wanted, wanted_length) == 0) {
            json_space(&probe);
            if (probe.at < probe.end && *probe.at++ == ':') {
                *value = probe;
                return true;
            }
        }
        cursor.at = quote + 1;
    }
    return false;
}

static bool youtube_object_span(YoutubeJson value,
                                const char **start, size_t *length)
{
    if (start == NULL || length == NULL) return false;
    json_space(&value);
    if (value.at >= value.end || *value.at != '{') return false;
    const char *object_start = value.at;
    if (!json_skip_value(&value, 0)) return false;
    *start = object_start;
    *length = (size_t) (value.at - object_start);
    return true;
}

static bool youtube_ascii_contains(const char *text, const char *wanted)
{
    if (text == NULL || wanted == NULL || wanted[0] == '\0') return false;
    size_t wanted_length = strlen(wanted);
    for (const char *at = text; *at != '\0'; at++) {
        size_t i = 0;
        while (i < wanted_length && at[i] != '\0'
               && tolower((unsigned char) at[i])
                    == tolower((unsigned char) wanted[i])) {
            i++;
        }
        if (i == wanted_length) return true;
    }
    return false;
}

const char *youtube_playability_name(YoutubePlayability playability)
{
    switch (playability) {
    case YOUTUBE_PLAYABILITY_OK: return "ok";
    case YOUTUBE_PLAYABILITY_LOGIN_REQUIRED: return "login-required";
    case YOUTUBE_PLAYABILITY_AGE_RESTRICTED: return "age-restricted";
    case YOUTUBE_PLAYABILITY_REGION_BLOCKED: return "region-blocked";
    case YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED: return "live-unsupported";
    case YOUTUBE_PLAYABILITY_UPCOMING_UNSUPPORTED:
        return "upcoming-unsupported";
    case YOUTUBE_PLAYABILITY_UNAVAILABLE: return "unavailable";
    case YOUTUBE_PLAYABILITY_CLIENT_REJECTED: return "client-rejected";
    case YOUTUBE_PLAYABILITY_UNKNOWN:
    default:
        return "unknown";
    }
}

bool youtube_playability_is_globally_terminal(
    YoutubePlayability playability)
{
    return playability == YOUTUBE_PLAYABILITY_REGION_BLOCKED
        || playability == YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED
        || playability == YOUTUBE_PLAYABILITY_UPCOMING_UNSUPPORTED;
}

bool youtube_media_url_supported(const char *url)
{
    TilefinchUrl media_url;
    if (!tilefinch_url_parse(url, &media_url)
        || media_url.scheme != TILEFINCH_URL_SCHEME_HTTPS
        || media_url.port != 443
        || media_url.has_fragment
        || media_url.ipv6_literal
        || media_url.host_length < strlen("googlevideo.com")) {
        return false;
    }
    const char *media_host =
        media_url.value + media_url.host_offset;
    const char *media_suffix =
        media_host + media_url.host_length - strlen("googlevideo.com");
    return strncasecmp(
               media_suffix, "googlevideo.com",
               strlen("googlevideo.com")) == 0
        && (media_suffix == media_host || media_suffix[-1] == '.');
}

static uint64_t youtube_url_expiry(const char *url)
{
    if (url == NULL) return 0;
    const char *query = strchr(url, '?');
    if (query == NULL) return 0;
    query++;
    while (*query != '\0' && *query != '#') {
        const char *end = query;
        while (*end != '\0' && *end != '&' && *end != '#') end++;
        static const char prefix[] = "expire=";
        if ((size_t) (end - query) > sizeof(prefix) - 1u
            && memcmp(query, prefix, sizeof(prefix) - 1u) == 0) {
            uint64_t value = 0;
            const char *digit = query + sizeof(prefix) - 1u;
            if (digit == end) return 0;
            while (digit < end) {
                if (!isdigit((unsigned char) *digit)
                    || value > (UINT64_MAX
                                - (uint64_t) (*digit - '0')) / 10u) {
                    return 0;
                }
                value = value * 10u + (uint64_t) (*digit - '0');
                digit++;
            }
            return value;
        }
        query = *end == '&' ? end + 1 : end;
    }
    return 0;
}

static bool youtube_parse_player_response_diagnostic_with_scratch(
    const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubePlayability *playability,
    YoutubeStream *stream, YoutubePlayerParseScratch *scratch,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (playability != NULL) *playability = YOUTUBE_PLAYABILITY_UNKNOWN;
    if (json == NULL || length == 0 || stream == NULL || scratch == NULL
        || maximum_height <= 0 || length > YOUTUBE_PLAYER_MAXIMUM_BYTES) {
        youtube_error(error, error_size, "invalid bounded player response");
        return false;
    }
    memset(scratch, 0, sizeof(*scratch));
    YoutubeStream *parsed = &scratch->parsed;
    if (video_id != NULL) {
        snprintf(parsed->video_id, sizeof(parsed->video_id), "%s", video_id);
    }
    YoutubeJson value;
    const char *playability_json = NULL;
    size_t playability_length = 0;
    char status[64] = {0};
    if (!youtube_find_key(json, length, "playabilityStatus", &value)
        || !youtube_object_span(
            value, &playability_json, &playability_length)
        || !youtube_find_key(
            playability_json, playability_length, "status", &value)
        || !json_string(&value, status, sizeof(status))) {
        youtube_error(
            error, error_size,
            "malformed YouTube playability status");
        return false;
    }
    if (status[0] != '\0' && strcmp(status, "OK") != 0) {
        char reason[192] = {0};
        if (youtube_find_key(
                playability_json, playability_length, "reason", &value)) {
            (void) json_string(&value, reason, sizeof(reason));
        }
        YoutubePlayability classified =
            YOUTUBE_PLAYABILITY_CLIENT_REJECTED;
        if (youtube_ascii_contains(reason, "age")
            || youtube_ascii_contains(reason, "confirm your age")) {
            classified = YOUTUBE_PLAYABILITY_AGE_RESTRICTED;
        } else if (youtube_ascii_contains(reason, "country")
                   || youtube_ascii_contains(reason, "region")) {
            classified = YOUTUBE_PLAYABILITY_REGION_BLOCKED;
        } else if (strcmp(status, "LOGIN_REQUIRED") == 0) {
            classified = YOUTUBE_PLAYABILITY_LOGIN_REQUIRED;
        } else if (strcmp(status, "LIVE_STREAM_OFFLINE") == 0
                   || youtube_ascii_contains(reason, "premiere")
                   || youtube_ascii_contains(reason, "upcoming")) {
            classified = YOUTUBE_PLAYABILITY_UPCOMING_UNSUPPORTED;
        } else if (youtube_ascii_contains(reason, "unavailable")
                   || youtube_ascii_contains(reason, "private")
                   || youtube_ascii_contains(reason, "removed")) {
            classified = YOUTUBE_PLAYABILITY_UNAVAILABLE;
        }
        if (playability != NULL) *playability = classified;
        youtube_error(
            error, error_size, "playability: %s: %s%s%s",
            youtube_playability_name(classified), status,
            reason[0] == '\0' ? "" : " (",
            reason[0] == '\0' ? "" : reason);
        if (reason[0] != '\0' && error != NULL && error_size != 0) {
            size_t used = strlen(error);
            if (used + 1u < error_size)
                snprintf(error + used, error_size - used, ")");
        }
        return false;
    }
    const char *details_json = NULL;
    size_t details_length = 0;
    bool have_details =
        youtube_find_key(json, length, "videoDetails", &value)
        && youtube_object_span(value, &details_json, &details_length);
    /*
     * isLiveContent describes the video's provenance, not necessarily its
     * current delivery mode. Archived broadcasts retain it while exposing a
     * finite MP4 inventory and are ordinary playable VODs. Reject the active
     * live contract below when an HLS manifest is actually present.
     */
    if (playability != NULL) *playability = YOUTUBE_PLAYABILITY_OK;
    if (have_details
        && youtube_find_key(
            details_json, details_length, "title", &value)) {
        (void) json_string(&value, parsed->title, sizeof(parsed->title));
    }
    const char *streaming_json = NULL;
    size_t streaming_length = 0;
    if (!youtube_find_key(json, length, "streamingData", &value)
        || !youtube_object_span(
            value, &streaming_json, &streaming_length)) {
        youtube_error(error, error_size, "format: no stream inventory");
        return false;
    }
    bool have_hls_manifest = youtube_find_key(
        streaming_json, streaming_length, "hlsManifestUrl", &value);
    YoutubeFormat *selected = &scratch->selected;
    bool have_progressive = youtube_find_key(
        streaming_json, streaming_length, "formats", &value);
    if (have_progressive
        && !youtube_parse_formats(
            &value, maximum_height, false, selected, NULL)) {
        youtube_error(
            error, error_size,
            "format: malformed YouTube format inventory");
        return false;
    }
    YoutubeFormat *selected_audio = &scratch->selected_audio;
    YoutubeFormat *adaptive_video = &scratch->adaptive_video;
    bool have_adaptive = youtube_find_key(
        streaming_json, streaming_length, "adaptiveFormats", &value);
    if (have_adaptive) {
        if (!youtube_parse_formats(
                &value, maximum_height, true,
                adaptive_video, selected_audio)) {
            youtube_error(
                error, error_size,
                "format: malformed YouTube adaptive inventory");
            return false;
        }
    }
    if (!have_progressive && !have_adaptive) {
        if (have_hls_manifest) {
            if (playability != NULL)
                *playability = YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED;
            youtube_error(
                error, error_size,
                "playability: live-unsupported: live streams are not supported");
            return false;
        }
        youtube_error(error, error_size, "format: no stream inventory");
        return false;
    }
    bool progressive_delivery_is_sized =
        selected->url[0] != '\0' && selected->content_length != 0;
    if (adaptive_video->url[0] != '\0'
        && selected_audio->url[0] != '\0'
        && (!progressive_delivery_is_sized
            || adaptive_video->height >= selected->height)) {
        *selected = *adaptive_video;
    } else {
        memset(selected_audio, 0, sizeof(*selected_audio));
    }
    if (selected->url[0] == '\0') {
        if (have_hls_manifest) {
            if (playability != NULL)
                *playability = YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED;
            youtube_error(
                error, error_size,
                "playability: live-unsupported: live streams are not supported");
            return false;
        }
        youtube_error(
            error, error_size,
            "format: no unciphered AVC video and AAC audio within %dp",
            maximum_height);
        return false;
    }
    if (!youtube_media_url_supported(selected->url)
        || (selected_audio->url[0] != '\0'
            && !youtube_media_url_supported(selected_audio->url))) {
        youtube_error(
            error, error_size,
            "format: YouTube returned an untrusted media host");
        return false;
    }
    parsed->itag = selected->itag;
    parsed->width = selected->width;
    parsed->height = selected->height;
    parsed->duration_ms = selected->duration_ms;
    parsed->content_length = selected->content_length;
    parsed->bitrate = selected->bitrate;
    snprintf(
        parsed->media_url, sizeof(parsed->media_url), "%s", selected->url);
    snprintf(
        parsed->mime_type, sizeof(parsed->mime_type), "%s", selected->mime);
    if (selected_audio->url[0] != '\0') {
        parsed->split_streams = true;
        parsed->audio_itag = selected_audio->itag;
        parsed->audio_content_length = selected_audio->content_length;
        parsed->audio_bitrate = selected_audio->bitrate;
        if (selected_audio->duration_ms > parsed->duration_ms)
            parsed->duration_ms = selected_audio->duration_ms;
        snprintf(
            parsed->audio_url, sizeof(parsed->audio_url), "%s",
            selected_audio->url);
        snprintf(
            parsed->audio_mime_type, sizeof(parsed->audio_mime_type), "%s",
            selected_audio->mime);
    }
    *stream = *parsed;
    return true;
}

bool youtube_parse_player_response_diagnostic(
    const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubePlayability *playability,
    YoutubeStream *stream,
    char *error, size_t error_size)
{
    YoutubePlayerParseScratch scratch;
    return youtube_parse_player_response_diagnostic_with_scratch(
        json, length, video_id, maximum_height, playability,
        stream, &scratch, error, error_size);
}

static bool youtube_parse_player_response_diagnostic_budget(
    Budget *budget, const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubePlayability *playability,
    YoutubeStream *stream, char *error, size_t error_size)
{
    YoutubePlayerParseScratch *scratch = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*scratch));
    if (scratch == NULL) {
        youtube_error(error, error_size, "player: parse admission failed");
        return false;
    }
    bool parsed = youtube_parse_player_response_diagnostic_with_scratch(
        json, length, video_id, maximum_height, playability,
        stream, scratch, error, error_size);
    budget_free(budget, scratch);
    return parsed;
}

bool youtube_parse_player_response(
    const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubeStream *stream,
    char *error, size_t error_size)
{
    return youtube_parse_player_response_diagnostic(
        json, length, video_id, maximum_height, NULL,
        stream, error, error_size);
}

static bool youtube_host_is(const TilefinchUrl *url, const char *host)
{
    return strlen(host) == url->host_length
        && strncasecmp(url->value + url->host_offset,
                       host, url->host_length) == 0;
}

bool youtube_watch_url_video_id(
    const char *watch_url, char output[YOUTUBE_VIDEO_ID_CAPACITY])
{
    if (watch_url == NULL || output == NULL) return false;
    TilefinchUrl url;
    if (!tilefinch_url_parse(watch_url, &url)
        || url.scheme != TILEFINCH_URL_SCHEME_HTTPS) return false;
    const char *start = NULL;
    const char *end = NULL;
    if (youtube_host_is(&url, "youtu.be")) {
        start = url.value + url.path_offset;
        end = start + url.path_length;
        while (start < end && *start == '/') start++;
    } else if (youtube_host_is(&url, "youtube.com")
               || youtube_host_is(&url, "www.youtube.com")
               || youtube_host_is(&url, "m.youtube.com")) {
        if (!url.has_query) return false;
        const char *query = url.value + url.query_offset;
        const char *query_end = query + url.query_length;
        while (query < query_end) {
            const char *field_end = memchr(
                query, '&', (size_t) (query_end - query));
            if (field_end == NULL) field_end = query_end;
            if ((size_t) (field_end - query) > 2u
                && query[0] == 'v' && query[1] == '=') {
                start = query + 2;
                end = field_end;
                break;
            }
            query = field_end < query_end ? field_end + 1 : query_end;
        }
    }
    if (start == NULL || end == NULL || start == end
        || (size_t) (end - start) >= YOUTUBE_VIDEO_ID_CAPACITY) return false;
    size_t length = (size_t) (end - start);
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) start[i];
        if (!isalnum(byte) && byte != '-' && byte != '_') return false;
    }
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

bool youtube_watch_url_supported(const char *url)
{
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    if (!youtube_watch_url_video_id(url, video_id)) return false;
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(url, &parsed) || !parsed.has_query) return true;
    const char *at = parsed.value + parsed.query_offset;
    const char *end = at + parsed.query_length;
    static const char internal_view[] = "tilefinch_view=";
    while (at < end) {
        const char *field_end = memchr(at, '&', (size_t) (end - at));
        if (field_end == NULL) field_end = end;
        if ((size_t) (field_end - at) > sizeof(internal_view) - 1u
            && memcmp(at, internal_view, sizeof(internal_view) - 1u) == 0) {
            return false;
        }
        at = field_end < end ? field_end + 1 : end;
    }
    return true;
}

static bool youtube_watch_string(const char *html, size_t length,
                                 const char *key, char *output,
                                 size_t output_size)
{
    YoutubeJson value;
    return youtube_find_key(html, length, key, &value)
        && json_string(&value, output, output_size);
}

static bool youtube_watch_unsigned(const char *html, size_t length,
                                   const char *key, uint64_t *output)
{
    YoutubeJson value;
    return youtube_find_key(html, length, key, &value)
        && json_unsigned_value(&value, output);
}

static bool youtube_json_escape(char *output, size_t output_size,
                                const char *value)
{
    if (output == NULL || output_size < 3u || value == NULL) return false;
    size_t used = 0;
    output[used++] = '"';
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        const char *escape = NULL;
        if (*at == '"') escape = "\\\"";
        else if (*at == '\\') escape = "\\\\";
        if (escape != NULL) {
            if (used + 2u >= output_size) return false;
            output[used++] = escape[0];
            output[used++] = escape[1];
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

static void youtube_accept_response_cookies(
    BrowserSession *session, const TilefinchRequestContext *request_context,
    const FetchResult *fetch, const char *fallback_url)
{
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        TilefinchRequestContext response_context = *request_context;
        response_context.target_url = fetch_set_cookie_url(
            fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            session, &response_context, fetch->set_cookies[i]);
    }
}

static long youtube_remaining_timeout_ms(uint64_t deadline_ns)
{
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    if (now >= deadline_ns) return 0;
    uint64_t remaining_ms = (deadline_ns - now + UINT64_C(999999))
                          / UINT64_C(1000000);
    return remaining_ms > (uint64_t) LONG_MAX
        ? LONG_MAX : (long) remaining_ms;
}

typedef struct {
    char visitor_json[2048];
    char visitor_field[2112];
    char playback_context[384];
    char body[8192];
    char url[512];
    char extra_headers[1536];
    char cookies[4096];
    TilefinchRequestContext context;
    FetchRequest request;
} YoutubePreparedPlayerRequest;

static bool youtube_resolver_url_supported(const char *value)
{
    TilefinchUrl url;
    return value != NULL && tilefinch_url_parse(value, &url)
        && url.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && (youtube_host_is(&url, "youtube.com")
            || youtube_host_is(&url, "www.youtube.com")
            || youtube_host_is(&url, "m.youtube.com"));
}

static bool youtube_prepare_player_request(
    BrowserSession *session, const YoutubeClientProfile *profile,
    const char *canonical_watch, const char *video_id,
    const char *api_key, const char *visitor,
    uint64_t signature_timestamp, YoutubePreparedPlayerRequest *prepared,
    char *error, size_t error_size)
{
    if (session == NULL || profile == NULL || canonical_watch == NULL
        || video_id == NULL || prepared == NULL) return false;
    memset(prepared, 0, sizeof(*prepared));
    if (visitor != NULL && visitor[0] != '\0') {
        if (!youtube_header_value_safe(visitor)
            || !youtube_json_escape(
                prepared->visitor_json, sizeof(prepared->visitor_json),
                visitor)) {
            youtube_error(error, error_size,
                          "player: visitor identifier exceeded bound");
            return false;
        }
        int length = snprintf(
            prepared->visitor_field, sizeof(prepared->visitor_field),
            ",\"visitorData\":%s", prepared->visitor_json);
        if (length < 0
            || (size_t) length >= sizeof(prepared->visitor_field)) {
            youtube_error(error, error_size,
                          "player: visitor identifier exceeded bound");
            return false;
        }
    }
    if (signature_timestamp != 0) {
        int length = snprintf(
            prepared->playback_context,
            sizeof(prepared->playback_context),
            ",\"playbackContext\":{\"contentPlaybackContext\":{"
            "\"html5Preference\":\"HTML5_PREF_WANTS\","
            "\"signatureTimestamp\":%llu}}",
            (unsigned long long) signature_timestamp);
        if (length < 0
            || (size_t) length >= sizeof(prepared->playback_context)) {
            youtube_error(error, error_size,
                          "player: playback context exceeded bound");
            return false;
        }
    }
    int body_length = snprintf(
        prepared->body, sizeof(prepared->body),
        "{\"context\":{\"client\":{\"clientName\":\"%s\","
        "\"clientVersion\":\"%s\",%s,\"hl\":\"en\","
        "\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0%s}%s},"
        "\"videoId\":\"%s\"%s,"
        "\"contentCheckOk\":true,\"racyCheckOk\":true}",
        profile->name, profile->version, profile->context_fields,
        prepared->visitor_field, profile->context_extra,
        video_id, prepared->playback_context);
    int url_length = api_key == NULL || api_key[0] == '\0'
        ? snprintf(
            prepared->url, sizeof(prepared->url),
            "https://www.youtube.com/youtubei/v1/player?prettyPrint=false")
        : snprintf(
            prepared->url, sizeof(prepared->url),
            "https://www.youtube.com/youtubei/v1/player?"
            "key=%s&prettyPrint=false", api_key);
    int extra_length = visitor == NULL || visitor[0] == '\0'
        ? snprintf(
            prepared->extra_headers, sizeof(prepared->extra_headers),
            "X-YouTube-Client-Name: %s\n"
            "X-YouTube-Client-Version: %s",
            profile->header_id, profile->version)
        : snprintf(
            prepared->extra_headers, sizeof(prepared->extra_headers),
            "X-YouTube-Client-Name: %s\n"
            "X-YouTube-Client-Version: %s\n"
            "X-Goog-Visitor-Id: %s",
            profile->header_id, profile->version, visitor);
    if (body_length < 0 || (size_t) body_length >= sizeof(prepared->body)
        || url_length < 0 || (size_t) url_length >= sizeof(prepared->url)
        || extra_length < 0
        || (size_t) extra_length >= sizeof(prepared->extra_headers)) {
        youtube_error(error, error_size, "player: request exceeded bound");
        return false;
    }
    prepared->context = (TilefinchRequestContext) {
        .target_url = prepared->url,
        .initiator_url = canonical_watch,
        .top_level_url = canonical_watch,
        .method = "POST",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
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
        .allow_http_errors = true,
        .referer = canonical_watch,
        .origin = "https://www.youtube.com",
        .accept = "*/*",
        .sec_fetch_dest = "empty",
        .sec_fetch_mode = "same-origin",
        .sec_fetch_site = "same-origin",
        .user_agent = profile->user_agent,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = canonical_watch,
        .initiator_url = canonical_watch,
        .referrer_source = canonical_watch,
        .connect_timeout_ms = YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS,
        .redirect_same_origin_only = true,
        .redirect_url_validator = youtube_resolver_url_supported,
        .cookie_session = session,
        .cookie_context = &prepared->context
    };
    return true;
}

static bool youtube_fetch_player_profile(
    Budget *budget, BrowserSession *session,
    const YoutubeClientProfile *profile,
    const char *canonical_watch, const char *video_id,
    const char *api_key, const char *visitor,
    uint64_t signature_timestamp, long timeout_ms,
    YoutubeResolverCancelCallback cancel, void *cancel_opaque,
    FetchResult *player, char *error, size_t error_size)
{
    if (budget == NULL || session == NULL || profile == NULL
        || canonical_watch == NULL || video_id == NULL || player == NULL
        || timeout_ms <= 0) {
        youtube_error(error, error_size, "player: invalid request");
        return false;
    }
    YoutubePreparedPlayerRequest *prepared = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*prepared));
    if (prepared == NULL) {
        youtube_error(error, error_size, "player: request admission failed");
        return false;
    }
    if (!youtube_prepare_player_request(
            session, profile, canonical_watch, video_id,
            api_key, visitor, signature_timestamp, prepared,
            error, error_size)) {
        budget_free(budget, prepared);
        return false;
    }
    bool ok = fetch_request_cancelable(
        budget, prepared->url, &prepared->request,
        YOUTUBE_PLAYER_MAXIMUM_BYTES, timeout_ms,
        cancel, cancel_opaque, player);
    if (!ok) {
        youtube_error(
            error, error_size, "player: fetch failed: %s",
            player->error[0] == '\0'
                ? "transport failure" : player->error);
        budget_free(budget, prepared);
        return false;
    }
    youtube_accept_response_cookies(
        session, &prepared->context, player, prepared->url);
    budget_free(budget, prepared);
    return true;
}

static bool youtube_finish_resolved_stream(
    YoutubeStream *resolved, const YoutubeClientProfile *profile,
    unsigned attempts, size_t player_bytes, long player_status,
    size_t watch_bytes, long watch_status,
    YoutubeStream *stream, char *error, size_t error_size)
{
    uint64_t video_expiry = youtube_url_expiry(resolved->media_url);
    uint64_t audio_expiry = resolved->split_streams
        ? youtube_url_expiry(resolved->audio_url) : video_expiry;
    uint64_t expiry = video_expiry;
    if (expiry == 0 || (audio_expiry != 0 && audio_expiry < expiry))
        expiry = audio_expiry;
    uint64_t now = tilefinch_platform_wall_time_ns()
                 / UINT64_C(1000000000);
    if (expiry != 0 && expiry <= now + YOUTUBE_EXPIRY_MARGIN_SECONDS) {
        youtube_error(
            error, error_size,
            "format: media URL expires too soon");
        return false;
    }
    resolved->expires_unix = expiry;
    resolved->client_attempts = attempts;
    snprintf(
        resolved->client_name, sizeof(resolved->client_name),
        "%s", profile->name);
    resolved->player_bytes = player_bytes;
    resolved->player_status = player_status;
    resolved->watch_bytes = watch_bytes;
    resolved->watch_status = watch_status;
    *stream = *resolved;
    return true;
}

typedef enum {
    YOUTUBE_RESOLVE_PHASE_DIRECT_START = 0,
    YOUTUBE_RESOLVE_PHASE_DIRECT_WAIT,
    YOUTUBE_RESOLVE_PHASE_DIRECT_PARSE,
    YOUTUBE_RESOLVE_PHASE_WATCH_START,
    YOUTUBE_RESOLVE_PHASE_WATCH_WAIT,
    YOUTUBE_RESOLVE_PHASE_WATCH_PARSE,
    YOUTUBE_RESOLVE_PHASE_ENRICHED_START,
    YOUTUBE_RESOLVE_PHASE_ENRICHED_WAIT,
    YOUTUBE_RESOLVE_PHASE_ENRICHED_PARSE,
    YOUTUBE_RESOLVE_PHASE_COMPLETE,
    YOUTUBE_RESOLVE_PHASE_FAILED
} YoutubeResolvePhase;

struct YoutubeResolveJob {
    Budget *budget;
    BrowserSession *session;
    YoutubeResolverCancelCallback cancel;
    void *cancel_opaque;
    int maximum_height;
    uint64_t deadline_ns;
    YoutubeResolvePhase phase;
    size_t client_index;
    unsigned attempts;
    size_t total_player_bytes;
    size_t watch_bytes;
    long watch_status;
    bool response_ok;
    bool enriched_from_cached_identity;
    uint64_t request_id;
    uint64_t request_started_ns;
    size_t request_pumps;
    size_t request_chunks;
    YoutubePlayability last_playability;
    YoutubePreparedPlayerRequest prepared;
    FetchResult response;
    YoutubeStream stream;
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    char canonical_watch[256];
    char visitor[1024];
    char api_key[128];
    uint64_t signature_timestamp;
    TilefinchRequestContext watch_context;
    char watch_cookies[4096];
    char last_error[256];
    char actionable_error[256];
    char error[256];
};

static void youtube_resolve_job_fail(
    YoutubeResolveJob *job, const char *format, ...)
{
    if (job == NULL || job->phase == YOUTUBE_RESOLVE_PHASE_FAILED) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(job->error, sizeof(job->error), format, arguments);
    va_end(arguments);
    if (job->request_id != 0) {
        (void) fetch_background_transport_cancel(
            job->request_id, "YouTube resolver failed");
        job->request_id = 0;
    }
    job->phase = YOUTUBE_RESOLVE_PHASE_FAILED;
}

static bool youtube_resolve_job_reserve(
    YoutubeResolveJob *job, size_t additional, size_t maximum_bytes)
{
    if (job == NULL || additional > maximum_bytes - job->response.length)
        return false;
    size_t required = job->response.length + additional + 1u;
    if (required <= job->response.capacity) return true;
    size_t capacity = job->response.capacity == 0
        ? 64u * 1024u : job->response.capacity;
    while (capacity < required) {
        size_t next = capacity > maximum_bytes / 2u
            ? maximum_bytes + 1u : capacity * 2u;
        if (next <= capacity) return false;
        capacity = next;
    }
    if (capacity > maximum_bytes + 1u) capacity = maximum_bytes + 1u;
    char *data = job->response.data == NULL
        ? budget_malloc_category(
              job->budget, BUDGET_CATEGORY_RESOURCE, capacity)
        : budget_realloc_category(
              job->budget, BUDGET_CATEGORY_RESOURCE,
              job->response.data, capacity);
    if (data == NULL) return false;
    job->response.data = data;
    job->response.capacity = capacity;
    return true;
}

static bool youtube_resolve_job_start_request(
    YoutubeResolveJob *job, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, YoutubeResolvePhase wait_phase)
{
    fetch_result_destroy(&job->response);
    job->response = (FetchResult) {.budget = job->budget};
    job->request_started_ns = tilefinch_platform_monotonic_time_ns();
    job->request_pumps = 0;
    job->request_chunks = 0;
    /* The cookie header was materialized on the browser thread when the
       prepared request was built. The transport worker captures response
       Set-Cookie fields and this job applies them after take, so neither of
       these browser-owned pointers belongs in the worker descriptor. Keeping
       them set makes the PSP transport correctly reject an otherwise safe
       request shape before it can even queue. */
    FetchRequest transport_request = *request;
    transport_request.cookie_session = NULL;
    transport_request.cookie_context = NULL;
    job->request_id = fetch_background_transport_enqueue_stream(
        url, &transport_request, maximum_bytes, timeout_ms);
    if (job->request_id == 0) {
        youtube_resolve_job_fail(
            job, "player: background transport queue unavailable");
        return false;
    }
    job->phase = wait_phase;
    return true;
}

static bool youtube_resolve_job_poll_response(
    YoutubeResolveJob *job, size_t maximum_bytes,
    YoutubeResolvePhase parse_phase)
{
    job->request_pumps++;
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(
            job->request_id, &progress)) {
        youtube_resolve_job_fail(
            job, "player: background request retired unexpectedly");
        return false;
    }
    if (progress.available_body_bytes != 0) {
        if (!youtube_resolve_job_reserve(
                job, progress.available_body_bytes, maximum_bytes)) {
            youtube_resolve_job_fail(
                job, "player: response exceeded shared memory budget");
            return false;
        }
        size_t taken = 0;
        if (fetch_background_transport_take_chunk(
                job->request_id,
                (unsigned char *) job->response.data + job->response.length,
                job->response.capacity - job->response.length, &taken)) {
            job->response.length += taken;
            job->response.data[job->response.length] = '\0';
            job->request_chunks++;
        }
        return true;
    }
    if (!progress.complete) return true;
    if (!youtube_resolve_job_reserve(job, 0, maximum_bytes)) {
        youtube_resolve_job_fail(
            job, "player: empty response allocation failed");
        return false;
    }
    job->response.data[job->response.length] = '\0';
    job->response_ok = fetch_background_transport_take_fetch_result(
        job->request_id, job->budget, &job->response);
    job->request_id = 0;
    job->phase = parse_phase;
    return true;
}

static void youtube_resolve_job_log_response(
    const YoutubeResolveJob *job, const char *phase, const char *client)
{
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed_us = now >= job->request_started_ns
        ? (now - job->request_started_ns) / UINT64_C(1000) : 0;
    printf("tilefinch-youtube-resolver: phase=%s client=%s ok=%d "
           "status=%ld bytes=%zu received=%zu chunks=%zu pumps=%zu "
           "elapsed=%lluus cached=%d\n",
           phase, client == NULL ? "none" : client,
           job->response_ok ? 1 : 0, job->response.status_code,
           job->response.length, job->response.received_body_bytes,
           job->request_chunks, job->request_pumps,
           (unsigned long long) elapsed_us,
           job->enriched_from_cached_identity ? 1 : 0);
}

static bool youtube_resolve_job_take_watch_prefix(YoutubeResolveJob *job)
{
    if (job == NULL || job->response.data == NULL
        || job->response.length == 0) return false;
    char visitor[sizeof(job->visitor)] = {0};
    char api_key[sizeof(job->api_key)] = {0};
    uint64_t signature_timestamp = 0;
    if (!youtube_watch_string(
            job->response.data, job->response.length, "VISITOR_DATA",
            visitor, sizeof(visitor))
        || !youtube_watch_string(
            job->response.data, job->response.length,
            "INNERTUBE_API_KEY", api_key, sizeof(api_key))
        || !youtube_watch_unsigned(
            job->response.data, job->response.length, "STS",
            &signature_timestamp)
        || !youtube_header_value_safe(visitor)
        || !youtube_api_key_safe(api_key)) return false;
    snprintf(job->visitor, sizeof(job->visitor), "%s", visitor);
    snprintf(job->api_key, sizeof(job->api_key), "%s", api_key);
    job->signature_timestamp = signature_timestamp;
    job->watch_bytes = job->response.length;
    job->watch_status = 0;
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed_us = now >= job->request_started_ns
        ? (now - job->request_started_ns) / UINT64_C(1000) : 0;
    printf("tilefinch-youtube-resolver: phase=watch-prefix ok=1 "
           "bytes=%zu chunks=%zu pumps=%zu elapsed=%lluus\n",
           job->response.length, job->request_chunks, job->request_pumps,
           (unsigned long long) elapsed_us);
    (void) fetch_background_transport_cancel(
        job->request_id, "watch configuration prefix complete");
    job->request_id = 0;
    fetch_result_destroy(&job->response);
    job->enriched_from_cached_identity = false;
    job->phase = YOUTUBE_RESOLVE_PHASE_ENRICHED_START;
    return true;
}

static bool youtube_resolve_job_prepare_watch(YoutubeResolveJob *job)
{
    job->watch_context = (TilefinchRequestContext) {
        .target_url = job->canonical_watch,
        .top_level_url = job->canonical_watch,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = true
    };
    job->watch_cookies[0] = '\0';
    (void) browser_session_cookie_header_context(
        job->session, &job->watch_context, job->watch_cookies,
        sizeof(job->watch_cookies));
    FetchRequest request = {
        .method = "GET",
        .cookie = job->watch_cookies,
        .accept =
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        .sec_fetch_dest = "document",
        .sec_fetch_mode = "navigate",
        .sec_fetch_site = "none",
        .sec_fetch_user = true,
        .upgrade_insecure_requests = true,
        .user_agent = YOUTUBE_BROWSER_UA,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = job->canonical_watch,
        .initiator_url = job->canonical_watch,
        .connect_timeout_ms = YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS,
        .redirect_same_origin_only = true,
        .redirect_url_validator = youtube_resolver_url_supported,
        .cookie_session = job->session,
        .cookie_context = &job->watch_context
    };
    long remaining_ms = youtube_attempt_timeout_ms(
        youtube_remaining_timeout_ms(job->deadline_ns));
    if (remaining_ms <= 0) {
        youtube_resolve_job_fail(job, "watch: resolution timed out");
        return false;
    }
    return youtube_resolve_job_start_request(
        job, job->canonical_watch, &request,
        YOUTUBE_WATCH_MAXIMUM_BYTES, remaining_ms,
        YOUTUBE_RESOLVE_PHASE_WATCH_WAIT);
}

YoutubeResolveJob *youtube_resolve_job_begin(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms,
    YoutubeResolverCancelCallback cancel, void *cancel_opaque)
{
    if (budget == NULL || session == NULL || watch_url == NULL
        || maximum_height <= 0 || timeout_ms <= 0
        || !fetch_background_transport_available()
        || !fetch_background_transport_initialize(budget)) return NULL;
    YoutubeResolveJob *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->session = session;
    job->cancel = cancel;
    job->cancel_opaque = cancel_opaque;
    job->maximum_height = maximum_height;
    job->phase = YOUTUBE_RESOLVE_PHASE_DIRECT_START;
    job->response.budget = budget;
    if (!youtube_watch_url_video_id(watch_url, job->video_id)) {
        youtube_resolve_job_fail(job, "unsupported YouTube watch URL");
        return job;
    }
    int length = snprintf(
        job->canonical_watch, sizeof(job->canonical_watch),
        "https://www.youtube.com/watch?v=%s&bpctr=9999999999&has_verified=1",
        job->video_id);
    if (length < 0 || (size_t) length >= sizeof(job->canonical_watch)) {
        youtube_resolve_job_fail(job, "YouTube watch URL exceeded bound");
        return job;
    }
    (void) browser_session_cookie_set(
        session, job->canonical_watch,
        "PREF=hl=en&tz=UTC; Domain=.youtube.com; Path=/");
    (void) browser_session_cookie_set(
        session, job->canonical_watch,
        "SOCS=CAI; Domain=.youtube.com; Path=/; Secure");
    YoutubeLiteResolverIdentity identity = {0};
    if (youtube_lite_resolver_identity_get(session, &identity)) {
        snprintf(job->api_key, sizeof(job->api_key), "%s",
                 identity.api_key);
        snprintf(job->visitor, sizeof(job->visitor), "%s",
                 identity.visitor);
        job->enriched_from_cached_identity = true;
        job->phase = YOUTUBE_RESOLVE_PHASE_ENRICHED_START;
    }
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    uint64_t duration = (uint64_t) timeout_ms > UINT64_MAX / UINT64_C(1000000)
        ? UINT64_MAX : (uint64_t) timeout_ms * UINT64_C(1000000);
    job->deadline_ns = duration > UINT64_MAX - now
        ? UINT64_MAX : now + duration;
    return job;
}

YoutubeResolveJobStatus youtube_resolve_job_pump(YoutubeResolveJob *job)
{
    if (job == NULL) return YOUTUBE_RESOLVE_JOB_FAILED;
    if (job->phase == YOUTUBE_RESOLVE_PHASE_COMPLETE)
        return YOUTUBE_RESOLVE_JOB_COMPLETE;
    if (job->phase == YOUTUBE_RESOLVE_PHASE_FAILED)
        return YOUTUBE_RESOLVE_JOB_FAILED;
    if (job->cancel != NULL && job->cancel(job->cancel_opaque)) {
        youtube_resolve_job_fail(job, "YouTube resolution cancelled");
        return YOUTUBE_RESOLVE_JOB_FAILED;
    }
    if (youtube_remaining_timeout_ms(job->deadline_ns) <= 0) {
        youtube_resolve_job_fail(job, "player: resolution timed out");
        return YOUTUBE_RESOLVE_JOB_FAILED;
    }
    switch (job->phase) {
    case YOUTUBE_RESOLVE_PHASE_DIRECT_START: {
        if (job->client_index >= sizeof(youtube_client_profiles)
                                     / sizeof(youtube_client_profiles[0])
            || job->client_index >= YOUTUBE_RESOLVER_MAXIMUM_CLIENTS) {
            job->phase = YOUTUBE_RESOLVE_PHASE_WATCH_START;
            return YOUTUBE_RESOLVE_JOB_PENDING;
        }
        const YoutubeClientProfile *profile =
            &youtube_client_profiles[job->client_index];
        if (!youtube_prepare_player_request(
                job->session, profile, job->canonical_watch,
                job->video_id, NULL, NULL, 0, &job->prepared,
                job->last_error, sizeof(job->last_error))) {
            youtube_resolve_job_fail(job, "%s", job->last_error);
            break;
        }
        long remaining_ms = youtube_attempt_timeout_ms(
            youtube_remaining_timeout_ms(job->deadline_ns));
        job->attempts++;
        (void) youtube_resolve_job_start_request(
            job, job->prepared.url, &job->prepared.request,
            YOUTUBE_PLAYER_MAXIMUM_BYTES, remaining_ms,
            YOUTUBE_RESOLVE_PHASE_DIRECT_WAIT);
        break;
    }
    case YOUTUBE_RESOLVE_PHASE_DIRECT_WAIT:
        (void) youtube_resolve_job_poll_response(
            job, YOUTUBE_PLAYER_MAXIMUM_BYTES,
            YOUTUBE_RESOLVE_PHASE_DIRECT_PARSE);
        break;
    case YOUTUBE_RESOLVE_PHASE_DIRECT_PARSE: {
        job->total_player_bytes += job->response.received_body_bytes != 0
            ? job->response.received_body_bytes : job->response.length;
        bool terminal = false;
        youtube_resolve_job_log_response(
            job, "direct", youtube_client_profiles[job->client_index].name);
        if (job->response_ok) {
            youtube_accept_response_cookies(
                job->session, &job->prepared.context,
                &job->response, job->prepared.url);
            YoutubeStream resolved = {0};
            YoutubePlayability classified = YOUTUBE_PLAYABILITY_UNKNOWN;
            bool parsed = youtube_parse_player_response_diagnostic_budget(
                job->budget, job->response.data, job->response.length,
                job->video_id,
                job->maximum_height, &classified, &resolved,
                job->last_error, sizeof(job->last_error));
            job->last_playability = classified;
            if (classified == YOUTUBE_PLAYABILITY_AGE_RESTRICTED
                && job->actionable_error[0] == '\0') {
                snprintf(job->actionable_error,
                         sizeof(job->actionable_error), "%s",
                         job->last_error);
            }
            const YoutubeClientProfile *profile =
                &youtube_client_profiles[job->client_index];
            if (parsed && youtube_direct_delivery_admitted(
                    profile->name, &resolved)
                && youtube_finish_resolved_stream(
                    &resolved, profile, job->attempts,
                    job->total_player_bytes, job->response.status_code,
                    0, 0, &job->stream,
                    job->last_error, sizeof(job->last_error))) {
                job->phase = YOUTUBE_RESOLVE_PHASE_COMPLETE;
                fetch_result_destroy(&job->response);
                break;
            }
            if (parsed) {
                youtube_error(
                    job->last_error, sizeof(job->last_error),
                    "player: %s direct delivery requires enriched context",
                    profile->name);
            }
            terminal = youtube_playability_is_globally_terminal(classified);
        } else {
            youtube_error(
                job->last_error, sizeof(job->last_error),
                "player: fetch failed: %s",
                job->response.error[0] == '\0'
                    ? "transport failure" : job->response.error);
        }
        fetch_result_destroy(&job->response);
        if (terminal) {
            youtube_resolve_job_fail(job, "%s", job->last_error);
        } else {
            job->client_index++;
            job->phase = YOUTUBE_RESOLVE_PHASE_DIRECT_START;
        }
        break;
    }
    case YOUTUBE_RESOLVE_PHASE_WATCH_START:
        (void) youtube_resolve_job_prepare_watch(job);
        break;
    case YOUTUBE_RESOLVE_PHASE_WATCH_WAIT:
        (void) youtube_resolve_job_poll_response(
            job, YOUTUBE_WATCH_MAXIMUM_BYTES,
            YOUTUBE_RESOLVE_PHASE_WATCH_PARSE);
        if (job->phase == YOUTUBE_RESOLVE_PHASE_WATCH_WAIT)
            (void) youtube_resolve_job_take_watch_prefix(job);
        break;
    case YOUTUBE_RESOLVE_PHASE_WATCH_PARSE:
        youtube_resolve_job_log_response(job, "watch", NULL);
        if (!job->response_ok) {
            youtube_resolve_job_fail(
                job, "watch: fetch failed after %u client attempts: %s",
                job->attempts,
                job->response.error[0] == '\0'
                    ? (job->last_error[0] == '\0'
                          ? "transport failure" : job->last_error)
                    : job->response.error);
            break;
        }
        youtube_accept_response_cookies(
            job->session, &job->watch_context,
            &job->response, job->canonical_watch);
        if (!youtube_watch_string(
                job->response.data, job->response.length, "VISITOR_DATA",
                job->visitor, sizeof(job->visitor))
            || !youtube_watch_string(
                job->response.data, job->response.length,
                "INNERTUBE_API_KEY", job->api_key, sizeof(job->api_key))
            || !youtube_watch_unsigned(
                job->response.data, job->response.length, "STS",
                &job->signature_timestamp)) {
            youtube_resolve_job_fail(
                job, "watch: page omitted bounded player configuration"
                     " after %u client attempts (%s)",
                job->attempts,
                youtube_playability_name(job->last_playability));
            break;
        }
        if (!youtube_header_value_safe(job->visitor)
            || !youtube_api_key_safe(job->api_key)) {
            youtube_resolve_job_fail(
                job, "watch: player configuration was unsafe");
            break;
        }
        job->watch_bytes = job->response.length;
        job->watch_status = job->response.status_code;
        job->enriched_from_cached_identity = false;
        fetch_result_destroy(&job->response);
        job->phase = YOUTUBE_RESOLVE_PHASE_ENRICHED_START;
        break;
    case YOUTUBE_RESOLVE_PHASE_ENRICHED_START: {
        const YoutubeClientProfile *profile =
            &youtube_client_profiles[
                sizeof(youtube_client_profiles)
                    / sizeof(youtube_client_profiles[0]) - 1u];
        if (!youtube_prepare_player_request(
                job->session, profile, job->canonical_watch,
                job->video_id, job->api_key, job->visitor,
                job->signature_timestamp, &job->prepared,
                job->last_error, sizeof(job->last_error))) {
            youtube_resolve_job_fail(job, "%s", job->last_error);
            break;
        }
        long remaining_ms = youtube_attempt_timeout_ms(
            youtube_remaining_timeout_ms(job->deadline_ns));
        job->attempts++;
        (void) youtube_resolve_job_start_request(
            job, job->prepared.url, &job->prepared.request,
            YOUTUBE_PLAYER_MAXIMUM_BYTES, remaining_ms,
            YOUTUBE_RESOLVE_PHASE_ENRICHED_WAIT);
        break;
    }
    case YOUTUBE_RESOLVE_PHASE_ENRICHED_WAIT:
        (void) youtube_resolve_job_poll_response(
            job, YOUTUBE_PLAYER_MAXIMUM_BYTES,
            YOUTUBE_RESOLVE_PHASE_ENRICHED_PARSE);
        break;
    case YOUTUBE_RESOLVE_PHASE_ENRICHED_PARSE: {
        job->total_player_bytes += job->response.received_body_bytes != 0
            ? job->response.received_body_bytes : job->response.length;
        youtube_resolve_job_log_response(
            job, "enriched", youtube_client_profiles[
                sizeof(youtube_client_profiles)
                    / sizeof(youtube_client_profiles[0]) - 1u].name);
        if (!job->response_ok) {
            if (job->enriched_from_cached_identity) {
                snprintf(job->last_error, sizeof(job->last_error),
                         "player: cached-context fetch failed: %.200s",
                         job->response.error[0] == '\0'
                             ? "transport failure" : job->response.error);
                fetch_result_destroy(&job->response);
                job->enriched_from_cached_identity = false;
                job->client_index = 0;
                job->phase = YOUTUBE_RESOLVE_PHASE_DIRECT_START;
            } else {
                youtube_resolve_job_fail(
                    job, "player: enriched fetch failed: %s",
                    job->response.error[0] == '\0'
                        ? "transport failure" : job->response.error);
            }
            break;
        }
        youtube_accept_response_cookies(
            job->session, &job->prepared.context,
            &job->response, job->prepared.url);
        YoutubeStream resolved = {0};
        YoutubePlayability classified = YOUTUBE_PLAYABILITY_UNKNOWN;
        bool parsed = youtube_parse_player_response_diagnostic_budget(
            job->budget, job->response.data, job->response.length,
            job->video_id,
            job->maximum_height, &classified, &resolved,
            job->error, sizeof(job->error));
        const YoutubeClientProfile *profile =
            &youtube_client_profiles[
                sizeof(youtube_client_profiles)
                    / sizeof(youtube_client_profiles[0]) - 1u];
        if (parsed) {
            parsed = youtube_finish_resolved_stream(
                &resolved, profile, job->attempts,
                job->total_player_bytes, job->response.status_code,
                job->watch_bytes, job->watch_status,
                &job->stream, job->error, sizeof(job->error));
        }
        if (!parsed && job->actionable_error[0] != '\0'
            && (classified == YOUTUBE_PLAYABILITY_UNKNOWN
                || classified == YOUTUBE_PLAYABILITY_LOGIN_REQUIRED
                || classified == YOUTUBE_PLAYABILITY_UNAVAILABLE
                || classified == YOUTUBE_PLAYABILITY_CLIENT_REJECTED)) {
            snprintf(job->error, sizeof(job->error), "%s",
                     job->actionable_error);
        }
        fetch_result_destroy(&job->response);
        if (!parsed && job->enriched_from_cached_identity
            && !youtube_playability_is_globally_terminal(classified)) {
            job->enriched_from_cached_identity = false;
            job->client_index = 0;
            job->phase = YOUTUBE_RESOLVE_PHASE_DIRECT_START;
        } else {
            job->phase = parsed
                ? YOUTUBE_RESOLVE_PHASE_COMPLETE
                : YOUTUBE_RESOLVE_PHASE_FAILED;
        }
        break;
    }
    case YOUTUBE_RESOLVE_PHASE_COMPLETE:
    case YOUTUBE_RESOLVE_PHASE_FAILED:
        break;
    }
    return job->phase == YOUTUBE_RESOLVE_PHASE_COMPLETE
        ? YOUTUBE_RESOLVE_JOB_COMPLETE
        : job->phase == YOUTUBE_RESOLVE_PHASE_FAILED
            ? YOUTUBE_RESOLVE_JOB_FAILED : YOUTUBE_RESOLVE_JOB_PENDING;
}

bool youtube_resolve_job_take(
    YoutubeResolveJob *job, YoutubeStream *stream)
{
    if (job == NULL || stream == NULL
        || job->phase != YOUTUBE_RESOLVE_PHASE_COMPLETE) return false;
    *stream = job->stream;
    return true;
}

const char *youtube_resolve_job_error(const YoutubeResolveJob *job)
{
    return job == NULL || job->error[0] == '\0'
        ? "YouTube resolution failed" : job->error;
}

void youtube_resolve_job_cancel(YoutubeResolveJob *job, const char *reason)
{
    if (job == NULL || job->phase == YOUTUBE_RESOLVE_PHASE_COMPLETE
        || job->phase == YOUTUBE_RESOLVE_PHASE_FAILED) return;
    if (job->request_id != 0) {
        (void) fetch_background_transport_cancel(job->request_id, reason);
        job->request_id = 0;
    }
    snprintf(job->error, sizeof(job->error), "%s",
             reason == NULL ? "YouTube resolution cancelled" : reason);
    job->phase = YOUTUBE_RESOLVE_PHASE_FAILED;
}

void youtube_resolve_job_destroy(YoutubeResolveJob *job)
{
    if (job == NULL) return;
    Budget *budget = job->budget;
    youtube_resolve_job_cancel(job, "YouTube resolver destroyed");
    fetch_result_destroy(&job->response);
    budget_free(budget, job);
}

typedef struct {
    FetchResult response;
    YoutubeStream resolved;
} YoutubeResolveSynchronousScratch;

static void youtube_resolve_synchronous_scratch_free(
    Budget *budget, YoutubeResolveSynchronousScratch *scratch)
{
    if (scratch == NULL) return;
    fetch_result_destroy(&scratch->response);
    budget_free(budget, scratch);
}

bool youtube_resolve_progressive_mp4_cancelable(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms,
    YoutubeResolverCancelCallback cancel, void *cancel_opaque,
    YoutubeStream *stream, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || session == NULL || watch_url == NULL
        || stream == NULL || maximum_height <= 0 || timeout_ms <= 0) {
        youtube_error(error, error_size, "invalid YouTube resolution request");
        return false;
    }
    if (cancel != NULL && cancel(cancel_opaque)) {
        youtube_error(error, error_size, "YouTube resolution cancelled");
        return false;
    }
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    if (!youtube_watch_url_video_id(watch_url, video_id)) {
        youtube_error(error, error_size, "unsupported YouTube watch URL");
        return false;
    }
    char canonical_watch[256];
    int watch_length = snprintf(
        canonical_watch, sizeof(canonical_watch),
        "https://www.youtube.com/watch?v=%s&bpctr=9999999999&has_verified=1",
        video_id);
    if (watch_length < 0 || (size_t) watch_length >= sizeof(canonical_watch)) {
        youtube_error(error, error_size, "YouTube watch URL exceeded bound");
        return false;
    }
    (void) browser_session_cookie_set(
        session, canonical_watch,
        "PREF=hl=en&tz=UTC; Domain=.youtube.com; Path=/");
    (void) browser_session_cookie_set(
        session, canonical_watch,
        "SOCS=CAI; Domain=.youtube.com; Path=/; Secure");
    uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t timeout_ns =
        (uint64_t) timeout_ms > UINT64_MAX / UINT64_C(1000000)
            ? UINT64_MAX
            : (uint64_t) timeout_ms * UINT64_C(1000000);
    uint64_t deadline_ns = started_ns > UINT64_MAX - timeout_ns
        ? UINT64_MAX : started_ns + timeout_ns;
    unsigned attempts = 0;
    size_t total_player_bytes = 0;
    char last_error[256] = {0};
    char actionable_error[256] = {0};
    YoutubePlayability last_playability = YOUTUBE_PLAYABILITY_UNKNOWN;
    /* FetchResult carries bounded header/cookie arrays large enough to be a
       material PSP stack frame. One budget-owned response is reset and reused
       across the serial client ladder, watch fallback, and enriched request;
       these phases never overlap their response ownership. */
    YoutubeResolveSynchronousScratch *scratch = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*scratch));
    if (scratch == NULL) {
        youtube_error(error, error_size, "player: response admission failed");
        return false;
    }
    FetchResult *response = &scratch->response;
    response->budget = budget;

    /*
     * The player endpoint is the happy path. It avoids downloading and
     * scanning a multi-megabyte watch page merely to obtain optional context.
     * Client fallbacks share one deadline. Region, live, and upcoming
     * classifications are global. Age and ordinary unavailable responses can
     * be client-specific, so the bounded ladder may still produce an
     * accessible response without weakening any authentication boundary.
     */
    for (size_t i = 0;
         i < sizeof(youtube_client_profiles)
               / sizeof(youtube_client_profiles[0])
             && i < YOUTUBE_RESOLVER_MAXIMUM_CLIENTS;
         i++) {
        long remaining_ms = youtube_remaining_timeout_ms(deadline_ns);
        if (remaining_ms <= 0) {
            youtube_error(error, error_size, "player: resolution timed out");
            youtube_resolve_synchronous_scratch_free(budget, scratch);
            return false;
        }
        fetch_result_destroy(response);
        attempts++;
        bool fetched = youtube_fetch_player_profile(
            budget, session, &youtube_client_profiles[i],
            canonical_watch, video_id, NULL, NULL, 0,
            youtube_attempt_timeout_ms(remaining_ms),
            cancel, cancel_opaque,
            response, last_error, sizeof(last_error));
        total_player_bytes += response->received_body_bytes != 0
            ? response->received_body_bytes : response->length;
        if (fetched) {
            YoutubeStream *resolved = &scratch->resolved;
            memset(resolved, 0, sizeof(*resolved));
            YoutubePlayability classified = YOUTUBE_PLAYABILITY_UNKNOWN;
            bool parsed = youtube_parse_player_response_diagnostic_budget(
                budget, response->data, response->length, video_id,
                maximum_height,
                &classified, resolved, last_error, sizeof(last_error));
            last_playability = classified;
            if (classified == YOUTUBE_PLAYABILITY_AGE_RESTRICTED
                && actionable_error[0] == '\0') {
                snprintf(
                    actionable_error, sizeof(actionable_error),
                    "%s", last_error);
            }
            if (parsed && youtube_direct_delivery_admitted(
                    youtube_client_profiles[i].name, resolved)
                && youtube_finish_resolved_stream(
                    resolved, &youtube_client_profiles[i], attempts,
                    total_player_bytes, response->status_code, 0, 0,
                    stream, last_error, sizeof(last_error))) {
                youtube_resolve_synchronous_scratch_free(budget, scratch);
                return true;
            }
            if (parsed) {
                youtube_error(
                    last_error, sizeof(last_error),
                    "player: %s direct delivery requires enriched context",
                    youtube_client_profiles[i].name);
            }
            if (youtube_playability_is_globally_terminal(classified)) {
                youtube_error(error, error_size, "%s", last_error);
                youtube_resolve_synchronous_scratch_free(budget, scratch);
                return false;
            }
        }
        fetch_result_destroy(response);
        if (cancel != NULL && cancel(cancel_opaque)) {
            youtube_error(error, error_size, "player: resolution cancelled");
            youtube_resolve_synchronous_scratch_free(budget, scratch);
            return false;
        }
    }

    /*
     * Compatibility fallback only: obtain visitor/key/STS from the watch
     * document after every direct profile failed. This keeps page-size drift
     * out of ordinary playback while retaining the older enriched request.
     */
    TilefinchRequestContext watch_context = {
        .target_url = canonical_watch,
        .top_level_url = canonical_watch,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = true
    };
    char watch_cookies[4096] = {0};
    (void) browser_session_cookie_header_context(
        session, &watch_context, watch_cookies, sizeof(watch_cookies));
    FetchRequest watch_request = {
        .method = "GET",
        .cookie = watch_cookies,
        .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        .sec_fetch_dest = "document",
        .sec_fetch_mode = "navigate",
        .sec_fetch_site = "none",
        .sec_fetch_user = true,
        .upgrade_insecure_requests = true,
        .user_agent = YOUTUBE_BROWSER_UA,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = canonical_watch,
        .initiator_url = canonical_watch,
        .connect_timeout_ms = YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS,
        .cookie_session = session,
        .cookie_context = &watch_context
    };
    fetch_result_destroy(response);
    long watch_timeout_ms = youtube_attempt_timeout_ms(
        youtube_remaining_timeout_ms(deadline_ns));
    if (watch_timeout_ms <= 0) {
        youtube_error(error, error_size, "watch: resolution timed out");
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    bool watch_ok = fetch_request_cancelable(
        budget, canonical_watch, &watch_request,
        YOUTUBE_WATCH_MAXIMUM_BYTES, watch_timeout_ms,
        cancel, cancel_opaque, response);
    if (!watch_ok) {
        youtube_error(
            error, error_size,
            "watch: fetch failed after %u client attempts: %s",
            attempts,
            response->error[0] == '\0'
                ? (last_error[0] == '\0'
                    ? "transport failure" : last_error)
                : response->error);
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    if (cancel != NULL && cancel(cancel_opaque)) {
        youtube_error(error, error_size, "watch: resolution cancelled");
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    youtube_accept_response_cookies(
        session, &watch_context, response, canonical_watch);
    char visitor[1024];
    char api_key[128];
    uint64_t signature_timestamp = 0;
    if (!youtube_watch_string(
            response->data, response->length, "VISITOR_DATA",
            visitor, sizeof(visitor))
        || !youtube_watch_string(
            response->data, response->length, "INNERTUBE_API_KEY",
            api_key, sizeof(api_key))
        || !youtube_watch_unsigned(
            response->data, response->length, "STS", &signature_timestamp)) {
        youtube_error(
            error, error_size,
            "watch: page omitted bounded player configuration"
            " after %u client attempts (%s)",
            attempts, youtube_playability_name(last_playability));
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    if (!youtube_header_value_safe(visitor)
        || !youtube_api_key_safe(api_key)) {
        youtube_error(
            error, error_size,
            "watch: player configuration was unsafe");
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    /*
     * The watch HTML has given up everything it holds -- three early config
     * strings -- so free it before the player fetch rather than carrying both
     * responses at once. Only its length and status are wanted later, for the
     * resolved-stream log, and those are scalars. The later destroys become
     * no-ops on the zeroed struct, which is safe by fetch_result_destroy's
     * contract; keeping them means no return path has to know this happened.
     * This is what keeps a 6 MiB watch page from overlapping the player fetch
     * in the peak.
     */
    size_t watch_body_length = response->length;
    long watch_status_code = response->status_code;
    fetch_result_destroy(response);
    long player_timeout_ms = youtube_attempt_timeout_ms(
        youtube_remaining_timeout_ms(deadline_ns));
    const YoutubeClientProfile *enriched_profile =
        &youtube_client_profiles[
            sizeof(youtube_client_profiles)
                / sizeof(youtube_client_profiles[0]) - 1u];
    attempts++;
    bool player_ok = player_timeout_ms > 0
        && youtube_fetch_player_profile(
            budget, session, enriched_profile,
            canonical_watch, video_id, api_key, visitor,
            signature_timestamp, player_timeout_ms,
            cancel, cancel_opaque, response, last_error,
            sizeof(last_error));
    total_player_bytes += response->received_body_bytes != 0
        ? response->received_body_bytes : response->length;
    if (!player_ok) {
        youtube_error(
            error, error_size,
            "player: enriched fetch failed: %s",
            player_timeout_ms <= 0 ? "resolution timed out"
                : (last_error[0] == '\0'
                    ? "transport failure" : last_error));
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    if (cancel != NULL && cancel(cancel_opaque)) {
        youtube_error(error, error_size, "player: resolution cancelled");
        youtube_resolve_synchronous_scratch_free(budget, scratch);
        return false;
    }
    YoutubeStream *resolved = &scratch->resolved;
    memset(resolved, 0, sizeof(*resolved));
    YoutubePlayability classified = YOUTUBE_PLAYABILITY_UNKNOWN;
    bool parsed = youtube_parse_player_response_diagnostic_budget(
        budget, response->data, response->length, video_id, maximum_height,
        &classified, resolved, error, error_size);
    if (parsed && !youtube_finish_resolved_stream(
            resolved, enriched_profile, attempts,
            total_player_bytes, response->status_code,
            watch_body_length, watch_status_code,
            stream, error, error_size)) {
        parsed = false;
    }
    if (!parsed && actionable_error[0] != '\0'
        && (classified == YOUTUBE_PLAYABILITY_UNKNOWN
            || classified == YOUTUBE_PLAYABILITY_LOGIN_REQUIRED
            || classified == YOUTUBE_PLAYABILITY_UNAVAILABLE
            || classified == YOUTUBE_PLAYABILITY_CLIENT_REJECTED)) {
        youtube_error(error, error_size, "%s", actionable_error);
    }
    youtube_resolve_synchronous_scratch_free(budget, scratch);
    return parsed;
}

bool youtube_resolve_progressive_mp4(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms, YoutubeStream *stream,
    char *error, size_t error_size)
{
    return youtube_resolve_progressive_mp4_cancelable(
        budget, session, watch_url, maximum_height, timeout_ms,
        NULL, NULL, stream, error, error_size);
}
