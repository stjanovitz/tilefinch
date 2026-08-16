#include "tilefinch/omnibox.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/url.h"

static const char *search_prefix(BrowserSearchEngine engine)
{
    switch (engine) {
        case BROWSER_SEARCH_BING:
            return "https://www.bing.com/search?q=";
        case BROWSER_SEARCH_DUCKDUCKGO:
            return "https://duckduckgo.com/?q=";
        case BROWSER_SEARCH_GOOGLE:
        default:
            return "https://www.google.com/search?q=";
    }
}

const char *browser_search_engine_name(BrowserSearchEngine engine)
{
    switch (engine) {
        case BROWSER_SEARCH_GOOGLE: return "GOOGLE";
        case BROWSER_SEARCH_BING: return "BING";
        case BROWSER_SEARCH_DUCKDUCKGO: return "DUCKDUCKGO";
        default: return "GOOGLE";
    }
}

bool browser_search_engine_valid(BrowserSearchEngine engine)
{
    return engine >= BROWSER_SEARCH_GOOGLE
        && engine <= BROWSER_SEARCH_DUCKDUCKGO;
}

static bool ascii_prefix(const char *text, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    if (length < prefix_length) return false;
    for (size_t i = 0; i < prefix_length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) prefix[i])) return false;
    }
    return true;
}

static bool address_candidate(const char *text, size_t length)
{
    if (length == 0) return false;
    size_t host_end = strcspn(text, "/?#");
    if (host_end > length) host_end = length;
    if (host_end == 0) return false;
    if (text[0] == '[') return true;
    if (host_end >= 9 && strncasecmp(text, "localhost", 9) == 0
        && (host_end == 9 || text[9] == ':')) return true;
    return memchr(text, '.', host_end) != NULL;
}

static bool copy_span(
    char *output, size_t capacity, const char *prefix,
    const char *text, size_t length)
{
    size_t prefix_length = strlen(prefix);
    if (prefix_length + length >= capacity) return false;
    memcpy(output, prefix, prefix_length);
    memcpy(output + prefix_length, text, length);
    output[prefix_length + length] = '\0';
    return true;
}

static bool append_encoded_query(
    char *output, size_t capacity, const char *prefix,
    const char *text, size_t length)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = strlen(prefix);
    if (used >= capacity) return false;
    memcpy(output, prefix, used);
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) text[i];
        bool unreserved = isalnum(byte) || byte == '-'
            || byte == '_' || byte == '.' || byte == '~';
        if (unreserved) {
            if (used + 1 >= capacity) return false;
            output[used++] = (char) byte;
        } else {
            if (used + 3 >= capacity) return false;
            output[used++] = '%';
            output[used++] = hex[byte >> 4];
            output[used++] = hex[byte & 15u];
        }
    }
    output[used] = '\0';
    return true;
}

bool browser_omnibox_resolve_kind(
    const char *input, BrowserSearchEngine engine,
    char *output, size_t output_capacity,
    BrowserOmniboxResolutionKind *kind)
{
    if (output == NULL || output_capacity == 0) return false;
    output[0] = '\0';
    if (input == NULL || !browser_search_engine_valid(engine)) return false;

    while (isspace((unsigned char) *input)) input++;
    size_t length = strlen(input);
    while (length > 0
           && isspace((unsigned char) input[length - 1])) length--;
    if (length == 0) return false;

    char value[TILEFINCH_URL_SERIALIZED_LIMIT];
    bool bounded = length < sizeof(value);
    if (bounded) {
        memcpy(value, input, length);
        value[length] = '\0';
    }

    bool explicit_http = ascii_prefix(input, length, "https://")
        || ascii_prefix(input, length, "http://");
    bool contains_space = false;
    for (size_t i = 0; i < length; i++) {
        if (isspace((unsigned char) input[i])) {
            contains_space = true;
            break;
        }
    }

    if (bounded && !contains_space && explicit_http) {
        TilefinchUrl parsed;
        if (tilefinch_url_parse(value, &parsed)) {
            bool resolved =
                tilefinch_url_normalize(value, output, output_capacity);
            if (!resolved) output[0] = '\0';
            if (resolved && kind != NULL)
                *kind = BROWSER_OMNIBOX_NAVIGATION;
            return resolved;
        }
    }

    if (bounded && !contains_space
        && !explicit_http && address_candidate(value, length)) {
        char candidate[TILEFINCH_URL_SERIALIZED_LIMIT];
        if (copy_span(candidate, sizeof(candidate), "https://", value, length)) {
            TilefinchUrl parsed;
            if (tilefinch_url_parse(candidate, &parsed)) {
                bool resolved =
                    tilefinch_url_normalize(candidate, output, output_capacity);
                if (!resolved) output[0] = '\0';
                if (resolved && kind != NULL)
                    *kind = BROWSER_OMNIBOX_NAVIGATION;
                return resolved;
            }
        }
    }

    bool resolved = append_encoded_query(
        output, output_capacity, search_prefix(engine), input, length);
    if (!resolved) output[0] = '\0';
    if (resolved && kind != NULL) *kind = BROWSER_OMNIBOX_SEARCH;
    return resolved;
}

bool browser_omnibox_resolve(
    const char *input, BrowserSearchEngine engine,
    char *output, size_t output_capacity)
{
    return browser_omnibox_resolve_kind(
        input, engine, output, output_capacity, NULL);
}
