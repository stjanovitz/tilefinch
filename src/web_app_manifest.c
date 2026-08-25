#include "tilefinch/web_app_manifest.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/fetch.h"
#include "tilefinch/style.h"
#include "tilefinch/url.h"

#define MANIFEST_JSON_DEPTH_LIMIT 16u

typedef struct { const char *at; const char *end; } ManifestJson;

static void manifest_error(char *out, size_t size, const char *message)
{
    if (out != NULL && size != 0)
        snprintf(out, size, "%s", message == NULL
                 ? "invalid web app manifest" : message);
}

const char *tilefinch_web_app_display_mode_name(
    TilefinchWebAppDisplayMode mode)
{
    switch (mode) {
        case TILEFINCH_WEB_APP_DISPLAY_MINIMAL_UI: return "Minimal UI";
        case TILEFINCH_WEB_APP_DISPLAY_STANDALONE: return "Standalone";
        case TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN: return "Fullscreen";
        case TILEFINCH_WEB_APP_DISPLAY_BROWSER:
        default: return "Browser";
    }
}

static TilefinchWebAppDisplayMode manifest_display_mode(const char *value)
{
    if (strcmp(value, "minimal-ui") == 0)
        return TILEFINCH_WEB_APP_DISPLAY_MINIMAL_UI;
    if (strcmp(value, "standalone") == 0)
        return TILEFINCH_WEB_APP_DISPLAY_STANDALONE;
    if (strcmp(value, "fullscreen") == 0)
        return TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN;
    return TILEFINCH_WEB_APP_DISPLAY_BROWSER;
}

static void json_space(ManifestJson *json)
{
    while (json->at < json->end && isspace((unsigned char) *json->at))
        json->at++;
}

static bool json_hex(char value, unsigned *digit)
{
    if (value >= '0' && value <= '9') *digit = (unsigned) (value - '0');
    else if (value >= 'a' && value <= 'f') *digit = (unsigned) (value - 'a') + 10u;
    else if (value >= 'A' && value <= 'F') *digit = (unsigned) (value - 'A') + 10u;
    else return false;
    return true;
}

static bool json_string(ManifestJson *json, char *out, size_t capacity)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '"') return false;
    size_t used = 0;
    while (json->at < json->end) {
        unsigned char value = (unsigned char) *json->at++;
        if (value == '"') {
            if (out != NULL) {
                if (used >= capacity) return false;
                out[used] = '\0';
            }
            return true;
        }
        if (value < 0x20u) return false;
        if (value == '\\') {
            if (json->at >= json->end) return false;
            value = (unsigned char) *json->at++;
            switch (value) {
                case '"': case '\\': case '/': break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u': {
                    unsigned codepoint = 0;
                    for (unsigned at = 0; at < 4u; at++) {
                        unsigned digit = 0;
                        if (json->at >= json->end
                            || !json_hex(*json->at++, &digit)) return false;
                        codepoint = codepoint * 16u + digit;
                    }
                    /* The native chrome's bounded name buffer cannot grow a
                       UTF-16 escape into arbitrary UTF-8 here. The live page
                       title remains the localized fallback. */
                    value = codepoint < 0x80u ? (unsigned char) codepoint : '?';
                    break;
                }
                default: return false;
            }
        }
        if (out != NULL) {
            if (used + 1u >= capacity) return false;
            out[used++] = (char) value;
        }
    }
    return false;
}

static bool json_skip_value(ManifestJson *json, unsigned depth);

static bool json_skip_compound(ManifestJson *json, char open, char close,
                               unsigned depth)
{
    if (depth >= MANIFEST_JSON_DEPTH_LIMIT || json->at >= json->end
        || *json->at++ != open) return false;
    json_space(json);
    if (json->at < json->end && *json->at == close) {
        json->at++;
        return true;
    }
    for (;;) {
        if (open == '{') {
            if (!json_string(json, NULL, 0)) return false;
            json_space(json);
            if (json->at >= json->end || *json->at++ != ':') return false;
        }
        if (!json_skip_value(json, depth + 1u)) return false;
        json_space(json);
        if (json->at >= json->end) return false;
        char separator = *json->at++;
        if (separator == close) return true;
        if (separator != ',') return false;
    }
}

static bool json_skip_value(ManifestJson *json, unsigned depth)
{
    json_space(json);
    if (json->at >= json->end) return false;
    if (*json->at == '"') return json_string(json, NULL, 0);
    if (*json->at == '{') return json_skip_compound(json, '{', '}', depth);
    if (*json->at == '[') return json_skip_compound(json, '[', ']', depth);
    const char *start = json->at;
    while (json->at < json->end && *json->at != ',' && *json->at != '}'
           && *json->at != ']' && !isspace((unsigned char) *json->at))
        json->at++;
    return json->at != start;
}

static unsigned icon_declared_size(const char *sizes)
{
    unsigned best = 0;
    for (const char *at = sizes == NULL ? "" : sizes; *at != '\0';) {
        while (*at == ' ') at++;
        unsigned width = 0, height = 0;
        while (isdigit((unsigned char) *at)) {
            if (width <= 4096u) width = width * 10u + (unsigned) (*at - '0');
            at++;
        }
        if ((*at == 'x' || *at == 'X') && width != 0) {
            at++;
            while (isdigit((unsigned char) *at)) {
                if (height <= 4096u) height = height * 10u + (unsigned) (*at - '0');
                at++;
            }
            if (width == height && width <= 4096u && width > best) best = width;
        }
        while (*at != '\0' && *at != ' ') at++;
    }
    return best;
}

static bool parse_icon_object(ManifestJson *json, char src[1024],
                              char type[64], unsigned *size)
{
    src[0] = type[0] = '\0';
    *size = 0;
    char sizes[128] = {0};
    json_space(json);
    if (json->at >= json->end || *json->at++ != '{') return false;
    json_space(json);
    if (json->at < json->end && *json->at == '}') {
        json->at++;
        return true;
    }
    for (;;) {
        char key[32];
        if (!json_string(json, key, sizeof(key))) return false;
        json_space(json);
        if (json->at >= json->end || *json->at++ != ':') return false;
        if (strcmp(key, "src") == 0) {
            if (!json_string(json, src, 1024)) return false;
        } else if (strcmp(key, "type") == 0) {
            if (!json_string(json, type, 64)) return false;
        } else if (strcmp(key, "sizes") == 0) {
            if (!json_string(json, sizes, sizeof(sizes))) return false;
        } else if (!json_skip_value(json, 1u)) return false;
        json_space(json);
        if (json->at >= json->end) return false;
        char separator = *json->at++;
        if (separator == '}') break;
        if (separator != ',') return false;
    }
    *size = icon_declared_size(sizes);
    return true;
}

static unsigned icon_score(unsigned size)
{
    if (size == 0) return 1u;
    return size >= 32u ? 8192u - (size > 256u ? 256u : size) : size;
}

static bool parse_icons(ManifestJson *json, const char *manifest_url,
                        TilefinchWebAppManifest *manifest)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '[') return false;
    json_space(json);
    if (json->at < json->end && *json->at == ']') {
        json->at++;
        return true;
    }
    for (unsigned count = 0; count < 32u; count++) {
        char src[1024] = {0}, type[64] = {0};
        unsigned size = 0;
        json_space(json);
        if (count < 16u && json->at < json->end && *json->at == '{') {
            if (!parse_icon_object(json, src, type, &size)) return false;
            char resolved[1024];
            bool supported = type[0] == '\0'
                || strcasecmp(type, "image/png") == 0
                || strcasecmp(type, "image/jpeg") == 0
                || strcasecmp(type, "image/webp") == 0;
            if (src[0] != '\0' && supported
                && fetch_resolve_url(manifest_url, src, resolved,
                                     sizeof(resolved))
                && (manifest->icon_url[0] == '\0'
                    || icon_score(size) > icon_score(manifest->icon_size))) {
                snprintf(manifest->icon_url, sizeof(manifest->icon_url),
                         "%s", resolved);
                snprintf(manifest->icon_type, sizeof(manifest->icon_type),
                         "%s", type);
                manifest->icon_size = size;
            }
        } else if (!json_skip_value(json, 1u)) return false;
        json_space(json);
        if (json->at >= json->end) return false;
        char separator = *json->at++;
        if (separator == ']') return true;
        if (separator != ',') return false;
    }
    return false;
}

bool tilefinch_web_app_manifest_parse(
    const char *bytes, size_t length, const char *manifest_url,
    const char *document_url, TilefinchWebAppManifest *manifest,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (bytes == NULL || length == 0 || length > TILEFINCH_WEB_APP_MANIFEST_LIMIT
        || manifest_url == NULL || document_url == NULL || manifest == NULL) {
        manifest_error(error, error_size, "web app manifest is unavailable");
        return false;
    }
    *manifest = (TilefinchWebAppManifest) {0};
    ManifestJson json = {.at = bytes, .end = bytes + length};
    json_space(&json);
    if (json.at >= json.end || *json.at++ != '{') goto malformed;
    json_space(&json);
    if (json.at < json.end && *json.at == '}') json.at++;
    while (json.at < json.end && *json.at != '}') {
        char key[32];
        if (!json_string(&json, key, sizeof(key))) goto malformed;
        json_space(&json);
        if (json.at >= json.end || *json.at++ != ':') goto malformed;
        if (strcmp(key, "name") == 0) {
            if (!json_string(&json, manifest->name,
                             sizeof(manifest->name))) goto malformed;
        } else if (strcmp(key, "short_name") == 0) {
            if (!json_string(&json, manifest->short_name,
                             sizeof(manifest->short_name))) goto malformed;
        } else if (strcmp(key, "start_url") == 0
                   || strcmp(key, "scope") == 0) {
            char relative[1024], *out = strcmp(key, "scope") == 0
                ? manifest->scope : manifest->start_url;
            if (!json_string(&json, relative, sizeof(relative))
                || !fetch_resolve_url(manifest_url, relative, out,
                                      TILEFINCH_WEB_APP_URL_LIMIT)) goto malformed;
        } else if (strcmp(key, "icons") == 0) {
            if (!parse_icons(&json, manifest_url, manifest)) goto malformed;
        } else if (strcmp(key, "theme_color") == 0) {
            char value[64];
            if (!json_string(&json, value, sizeof(value))) goto malformed;
            uint32_t color = 0;
            uint8_t alpha = 0;
            if (style_color_parse(
                    value, strlen(value), &color, &alpha)) {
                manifest->theme_color = color;
                manifest->theme_alpha = alpha;
                manifest->theme_color_valid = true;
            }
        } else if (strcmp(key, "display") == 0) {
            char value[32];
            if (!json_string(&json, value, sizeof(value))) goto malformed;
            manifest->display_mode = manifest_display_mode(value);
        } else if (!json_skip_value(&json, 0u)) goto malformed;
        json_space(&json);
        if (json.at >= json.end) goto malformed;
        char separator = *json.at++;
        if (separator == '}') break;
        if (separator != ',') goto malformed;
        json_space(&json);
    }
    json_space(&json);
    if (json.at != json.end) goto malformed;
    char page_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char manifest_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(document_url, page_origin, sizeof(page_origin))
        || !tilefinch_url_origin(manifest_url, manifest_origin,
                                 sizeof(manifest_origin))
        || strcmp(page_origin, manifest_origin) != 0) {
        manifest_error(error, error_size, "manifest must share the page origin");
        return false;
    }
    char *urls[] = {manifest->start_url, manifest->scope, manifest->icon_url};
    for (size_t at = 0; at < sizeof(urls) / sizeof(urls[0]); at++) {
        if (urls[at][0] == '\0') continue;
        char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        if (!tilefinch_url_origin(urls[at], origin, sizeof(origin))
            || strcmp(origin, page_origin) != 0) {
            if (at == 2u) manifest->icon_url[0] = '\0';
            else {
                manifest_error(error, error_size,
                               "app start and scope must share the page origin");
                return false;
            }
        }
    }
    return true;

malformed:
    manifest_error(error, error_size,
                   "web app manifest is malformed or too complex");
    return false;
}
