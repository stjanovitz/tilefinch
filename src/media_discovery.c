#include "tilefinch/media_discovery.h"

#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define MEDIA_DISCOVERY_NODE_LIMIT 16384u
#define MEDIA_DISCOVERY_TEXT_LIMIT (256u * 1024u)
#define MEDIA_DISCOVERY_WINDOW 96u

typedef struct {
    char *url;
    size_t capacity;
    MediaDiscoveryKind kind;
    unsigned quality;
    bool found;
} MediaDiscoveryBest;

static bool media_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
        && memcmp(name, wanted, length) == 0;
}

MediaDiscoveryKind media_discovery_reference_kind(
    const char *value, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (value[i] != '.') continue;
        size_t remaining = length - i;
        MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
        size_t suffix = 0;
        if (remaining >= 4u && strncasecmp(value + i, ".mp4", 4u) == 0) {
            kind = MEDIA_DISCOVERY_MP4;
            suffix = 4u;
        } else if (remaining >= 5u
                   && strncasecmp(value + i, ".m3u8", 5u) == 0) {
            kind = MEDIA_DISCOVERY_HLS;
            suffix = 5u;
        } else if (remaining >= 5u
                   && strncasecmp(value + i, ".webm", 5u) == 0) {
            kind = MEDIA_DISCOVERY_WEBM;
            suffix = 5u;
        }
        if (kind == MEDIA_DISCOVERY_NONE) continue;
        if (i + suffix == length || value[i + suffix] == '?'
            || value[i + suffix] == '#' || value[i + suffix] == '&') {
            return kind;
        }
    }
    return MEDIA_DISCOVERY_NONE;
}

static unsigned media_quality_hint(
    const char *source, size_t length, size_t begin, size_t end)
{
    size_t from = begin > MEDIA_DISCOVERY_WINDOW
        ? begin - MEDIA_DISCOVERY_WINDOW : 0;
    size_t to = length - end > MEDIA_DISCOVERY_WINDOW
        ? end + MEDIA_DISCOVERY_WINDOW : length;
    unsigned best = 0;
    size_t best_distance = SIZE_MAX;
    for (size_t i = from; i < to;) {
        if (!isdigit((unsigned char) source[i])) {
            i++;
            continue;
        }
        size_t number_begin = i;
        unsigned value = 0;
        size_t digits = 0;
        while (i < to && isdigit((unsigned char) source[i]) && digits < 5u) {
            value = value * 10u + (unsigned) (source[i++] - '0');
            digits++;
        }
        if (digits >= 3u && value >= 144u && value <= 4320u) {
            size_t distance = number_begin < begin ? begin - number_begin
                : number_begin > end ? number_begin - end : 0;
            if (best == 0u || distance < best_distance) {
                best = value;
                best_distance = distance;
            }
        }
    }
    return best;
}

static bool media_candidate_better(
    const MediaDiscoveryBest *best, MediaDiscoveryKind kind,
    unsigned quality)
{
    if (!best->found) return true;
    if (kind != best->kind) return kind < best->kind;
    if (quality == best->quality) return false;
    if (quality >= 240u) {
        return best->quality < 240u || quality < best->quality;
    }
    return best->quality < 240u && quality > best->quality;
}

static size_t media_copy_json_string(
    char *output, size_t capacity, const char *source,
    size_t begin, size_t end)
{
    if (capacity == 0) return 0;
    size_t written = 0;
    for (size_t i = begin; i < end; i++) {
        unsigned char value = (unsigned char) source[i];
        if (value == '\\' && i + 1u < end) {
            unsigned char escaped = (unsigned char) source[++i];
            if (escaped == '/' || escaped == '\\' || escaped == '"') {
                value = escaped;
            } else if (escaped == 'u' && end - i >= 5u
                       && source[i + 1u] == '0'
                       && source[i + 2u] == '0') {
                const char *hex = source + i + 3u;
                if (strncasecmp(hex, "2f", 2u) == 0) value = '/';
                else if (strncasecmp(hex, "26", 2u) == 0) value = '&';
                else if (strncasecmp(hex, "3d", 2u) == 0) value = '=';
                else if (strncasecmp(hex, "3f", 2u) == 0) value = '?';
                else return 0;
                i += 4u;
            } else return 0;
        }
        if (value < 0x20u || written + 1u >= capacity) return 0;
        output[written++] = (char) value;
    }
    output[written] = '\0';
    return written;
}

static void media_consider_span(
    MediaDiscoveryBest *best, const char *source, size_t length,
    size_t begin, size_t end)
{
    if (best == NULL || source == NULL || begin >= end || end > length) return;
    size_t raw_length = end - begin;
    MediaDiscoveryKind kind = media_discovery_reference_kind(
        source + begin, raw_length);
    if (kind == MEDIA_DISCOVERY_NONE) return;
    unsigned quality = media_quality_hint(source, length, begin, end);
    if (!media_candidate_better(best, kind, quality)
        || raw_length >= best->capacity) return;
    size_t candidate_length = media_copy_json_string(
        best->url, best->capacity, source, begin, end);
    if (candidate_length == 0) return;
    best->kind = kind;
    best->quality = quality;
    best->found = true;
}

static void media_scan_text(
    MediaDiscoveryBest *best, const char *source, size_t length)
{
    if (source == NULL || length == 0) return;
    for (size_t i = 0; i < length;) {
        char quote = source[i];
        if (quote != '"' && quote != '\'') {
            i++;
            continue;
        }
        size_t begin = ++i;
        bool escaped = false;
        while (i < length) {
            char value = source[i];
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) break;
            i++;
        }
        if (i >= length) break;
        media_consider_span(best, source, length, begin, i);
        i++;
    }
}

static bool media_script_is_data(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    if (type != NULL && length != 0) {
        return (length == sizeof("application/json") - 1u
                && strncasecmp(type, "application/json", length) == 0)
            || (length == sizeof("application/ld+json") - 1u
                && strncasecmp(type, "application/ld+json", length) == 0);
    }
    return script_runtime_inline_data_candidate(node, NULL);
}

static lxb_dom_node_t *media_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

bool media_discover_document_candidate(
    const PocDocument *document, char *url, size_t url_capacity,
    MediaDiscoveryResult *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (url != NULL && url_capacity != 0) url[0] = '\0';
    if (document == NULL || document->html == NULL || url == NULL
        || url_capacity < 2u) return false;
    MediaDiscoveryBest best = {.url = url, .capacity = url_capacity};
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t bytes = 0, nodes = 0;
    static const char *const attributes[] = {
        "src", "data-src", "data-url", "data-video-url", "content"
    };
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes < MEDIA_DISCOVERY_NODE_LIMIT;
         node = media_walk_next(root, node)) {
        nodes++;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
            size_t length = 0;
            const char *value = document_attribute(
                node, attributes[i], &length);
            if (value == NULL || length == 0) continue;
            media_consider_span(&best, value, length, 0, length);
        }
        if (!media_name_is(node, "script") || !media_script_is_data(node)) {
            continue;
        }
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL || length == 0) continue;
            size_t remaining = MEDIA_DISCOVERY_TEXT_LIMIT - bytes;
            size_t admitted = length < remaining ? length : remaining;
            media_scan_text(&best, text, admitted);
            bytes += admitted;
            if (bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
        }
        if (bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
    }
    if (result != NULL) {
        result->kind = best.kind;
        result->quality = best.quality;
        result->inspected_bytes = bytes;
        result->inspected_nodes = nodes;
    }
    return best.found;
}
