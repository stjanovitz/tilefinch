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
#define MEDIA_STRUCTURED_JSON_DEPTH_LIMIT 16u

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
        } else if (remaining >= 4u
                   && strncasecmp(value + i, ".m4a", 4u) == 0) {
            kind = MEDIA_DISCOVERY_AUDIO_MP4;
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
    /* Generic video discovery must not reinterpret an unrelated audio URL
       from a data blob. Structured audio has its own context-aware pass. */
    if (kind == MEDIA_DISCOVERY_NONE
        || kind == MEDIA_DISCOVERY_AUDIO_MP4) return;
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

static bool media_script_is_data(lxb_dom_node_t *node, bool *strict_json)
{
    if (strict_json != NULL) *strict_json = false;
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    if (type != NULL && length != 0) {
        bool json = (length == sizeof("application/json") - 1u
                     && strncasecmp(type, "application/json", length) == 0)
            || (length == sizeof("application/ld+json") - 1u
                && strncasecmp(type, "application/ld+json", length) == 0);
        if (strict_json != NULL) *strict_json = json;
        return json;
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
        if (!media_name_is(node, "script")
            || !media_script_is_data(node, NULL)) {
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

typedef enum {
    MEDIA_JSON_KEY_NONE = 0,
    MEDIA_JSON_KEY_TYPE,
    MEDIA_JSON_KEY_AUDIO,
    MEDIA_JSON_KEY_CONTENT_URL,
    MEDIA_JSON_KEY_NAME,
    MEDIA_JSON_KEY_URL,
    MEDIA_JSON_KEY_THUMBNAIL,
    MEDIA_JSON_KEY_DURATION
} MediaJsonKey;

typedef struct {
    bool object;
    bool expect_key;
    bool audio_object;
    bool music_recording;
    bool forced_audio;
    bool nested_audio_value;
    bool nested_audio_pending;
    MediaJsonKey pending_key;
    /* Arrays retain the object key whose value they contain. This admits
       standard JSON-LD shapes such as `@type:[...]` and `audio:[{...}]`
       without building an object graph. */
    MediaJsonKey array_value_key;
    uint32_t content_begin, content_end;
    uint32_t name_begin, name_end;
    uint32_t page_begin, page_end;
    uint32_t thumbnail_begin, thumbnail_end;
    uint32_t duration_begin, duration_end;
    uint32_t nested_content_begin, nested_content_end;
    uint32_t nested_name_begin, nested_name_end;
    uint32_t nested_page_begin, nested_page_end;
    uint32_t nested_thumbnail_begin, nested_thumbnail_end;
    uint32_t nested_duration_begin, nested_duration_end;
} MediaJsonFrame;

static MediaJsonFrame *media_json_nearest_object(
    MediaJsonFrame *stack, size_t depth)
{
    while (depth != 0) {
        MediaJsonFrame *frame = &stack[--depth];
        if (frame->object) return frame;
    }
    return NULL;
}

static bool media_json_span_is(
    const char *source, size_t begin, size_t end, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return end >= begin && end - begin == wanted_length
        && strncasecmp(source + begin, wanted, wanted_length) == 0;
}

static void media_json_note_type(
    MediaJsonFrame *frame, const char *source, uint32_t begin, uint32_t end)
{
    if (frame == NULL) return;
    if (media_json_span_is(source, begin, end, "AudioObject")) {
        frame->audio_object = true;
    } else if (media_json_span_is(
                   source, begin, end, "MusicRecording")) {
        frame->music_recording = true;
    }
}

static MediaJsonKey media_json_key(
    const char *source, size_t begin, size_t end)
{
    if (media_json_span_is(source, begin, end, "@type"))
        return MEDIA_JSON_KEY_TYPE;
    if (media_json_span_is(source, begin, end, "audio"))
        return MEDIA_JSON_KEY_AUDIO;
    if (media_json_span_is(source, begin, end, "contentUrl"))
        return MEDIA_JSON_KEY_CONTENT_URL;
    if (media_json_span_is(source, begin, end, "name"))
        return MEDIA_JSON_KEY_NAME;
    if (media_json_span_is(source, begin, end, "url"))
        return MEDIA_JSON_KEY_URL;
    if (media_json_span_is(source, begin, end, "thumbnailUrl")
        || media_json_span_is(source, begin, end, "thumbnail"))
        return MEDIA_JSON_KEY_THUMBNAIL;
    if (media_json_span_is(source, begin, end, "duration"))
        return MEDIA_JSON_KEY_DURATION;
    return MEDIA_JSON_KEY_NONE;
}

static bool media_json_string_span(
    const char *source, size_t length, size_t *at,
    uint32_t *begin, uint32_t *end)
{
    if (source == NULL || at == NULL || *at >= length
        || source[*at] != '"') return false;
    size_t cursor = *at + 1u;
    size_t start = cursor;
    bool escaped = false;
    for (; cursor < length; cursor++) {
        unsigned char value = (unsigned char) source[cursor];
        if (value < 0x20u) return false;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '"') {
            if (start > UINT32_MAX || cursor > UINT32_MAX) return false;
            *begin = (uint32_t) start;
            *end = (uint32_t) cursor;
            *at = cursor + 1u;
            return true;
        }
    }
    return false;
}

static void media_structured_commit(
    MediaStructuredAudioIndex *index, const char *source,
    const MediaJsonFrame *frame)
{
    if (index == NULL || source == NULL || frame == NULL
        || (!frame->audio_object && !frame->forced_audio)
        || frame->content_end <= frame->content_begin) return;
    MediaDiscoveryKind kind = media_discovery_reference_kind(
        source + frame->content_begin,
        frame->content_end - frame->content_begin);
    /* AudioObject may publish an audio-only MP4 with the generic .mp4
       spelling. Its schema context, not the extension, supplies the kind. */
    if (kind != MEDIA_DISCOVERY_AUDIO_MP4
        && kind != MEDIA_DISCOVERY_MP4) return;
    size_t url_length = frame->content_end - frame->content_begin;
    for (size_t i = 0; i < index->candidate_count; i++) {
        const MediaStructuredAudioCandidate *prior = &index->candidates[i];
        size_t prior_length = prior->url_end - prior->url_begin;
        if (prior_length == url_length
            && memcmp(prior->source + prior->url_begin,
                      source + frame->content_begin, url_length) == 0) {
            return;
        }
    }
    if (index->candidate_count == MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT) {
        index->candidate_overflow++;
        return;
    }
    MediaStructuredAudioCandidate *candidate =
        &index->candidates[index->candidate_count++];
    *candidate = (MediaStructuredAudioCandidate) {
        .source = source,
        .url_begin = frame->content_begin,
        .url_end = frame->content_end,
        .name_begin = frame->name_begin,
        .name_end = frame->name_end,
        .page_begin = frame->page_begin,
        .page_end = frame->page_end,
        .thumbnail_begin = frame->thumbnail_begin,
        .thumbnail_end = frame->thumbnail_end,
        .duration_begin = frame->duration_begin,
        .duration_end = frame->duration_end
    };
}

static bool media_scan_structured_json(
    const char *source, size_t length, MediaStructuredAudioIndex *index)
{
    MediaJsonFrame stack[MEDIA_STRUCTURED_JSON_DEPTH_LIMIT] = {0};
    size_t depth = 0;
    bool malformed = false;
    for (size_t at = 0; at < length;) {
        unsigned char value = (unsigned char) source[at];
        if (isspace(value) || value == ':' || value == ',') {
            if (value == ',' && depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].expect_key = true;
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            at++;
            continue;
        }
        if (value == '{' || value == '[') {
            if (depth == MEDIA_STRUCTURED_JSON_DEPTH_LIMIT) {
                malformed = true;
                break;
            }
            bool nested_audio_value = false;
            MediaJsonKey inherited_key = MEDIA_JSON_KEY_NONE;
            if (depth != 0) {
                MediaJsonFrame *parent = &stack[depth - 1u];
                inherited_key = parent->object
                    ? parent->pending_key : parent->array_value_key;
                nested_audio_value = value == '{'
                    && inherited_key == MEDIA_JSON_KEY_AUDIO;
                if (parent->object) {
                    parent->pending_key = MEDIA_JSON_KEY_NONE;
                }
            }
            stack[depth++] = (MediaJsonFrame) {
                .object = value == '{',
                .expect_key = value == '{',
                .nested_audio_value = nested_audio_value,
                .array_value_key = value == '['
                    ? inherited_key : MEDIA_JSON_KEY_NONE
            };
            index->inspected_nodes++;
            at++;
            continue;
        }
        if (value == '}' || value == ']') {
            bool object = value == '}';
            if (depth == 0 || stack[depth - 1u].object != object) {
                malformed = true;
                break;
            }
            if (object) {
                MediaJsonFrame *closed = &stack[depth - 1u];
                if (closed->audio_object && !closed->nested_audio_value) {
                    media_structured_commit(index, source, closed);
                }
                if (closed->nested_audio_value) {
                    MediaJsonFrame *parent = media_json_nearest_object(
                        stack, depth - 1u);
                    if (parent != NULL) {
                        parent->nested_audio_pending = true;
                        parent->nested_content_begin = closed->content_begin;
                        parent->nested_content_end = closed->content_end;
                        parent->nested_name_begin = closed->name_begin;
                        parent->nested_name_end = closed->name_end;
                        parent->nested_page_begin = closed->page_begin;
                        parent->nested_page_end = closed->page_end;
                        parent->nested_thumbnail_begin =
                            closed->thumbnail_begin;
                        parent->nested_thumbnail_end = closed->thumbnail_end;
                        parent->nested_duration_begin = closed->duration_begin;
                        parent->nested_duration_end = closed->duration_end;
                    }
                }
                if (closed->music_recording
                    && closed->nested_audio_pending) {
                    MediaJsonFrame nested = {
                        .forced_audio = true,
                        .content_begin = closed->nested_content_begin,
                        .content_end = closed->nested_content_end,
                        .name_begin = closed->nested_name_end
                                > closed->nested_name_begin
                            ? closed->nested_name_begin : closed->name_begin,
                        .name_end = closed->nested_name_end
                                > closed->nested_name_begin
                            ? closed->nested_name_end : closed->name_end,
                        .page_begin = closed->nested_page_end
                                > closed->nested_page_begin
                            ? closed->nested_page_begin : closed->page_begin,
                        .page_end = closed->nested_page_end
                                > closed->nested_page_begin
                            ? closed->nested_page_end : closed->page_end,
                        .thumbnail_begin = closed->nested_thumbnail_end
                                > closed->nested_thumbnail_begin
                            ? closed->nested_thumbnail_begin
                            : closed->thumbnail_begin,
                        .thumbnail_end = closed->nested_thumbnail_end
                                > closed->nested_thumbnail_begin
                            ? closed->nested_thumbnail_end
                            : closed->thumbnail_end,
                        .duration_begin = closed->nested_duration_end
                                > closed->nested_duration_begin
                            ? closed->nested_duration_begin
                            : closed->duration_begin,
                        .duration_end = closed->nested_duration_end
                                > closed->nested_duration_begin
                            ? closed->nested_duration_end
                            : closed->duration_end
                    };
                    media_structured_commit(index, source, &nested);
                }
            }
            depth--;
            if (depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            at++;
            continue;
        }
        if (value == '"') {
            uint32_t begin = 0, end = 0;
            if (!media_json_string_span(
                    source, length, &at, &begin, &end)) {
                malformed = true;
                break;
            }
            if (depth == 0) continue;
            MediaJsonFrame *frame = &stack[depth - 1u];
            if (!frame->object) {
                if (frame->array_value_key == MEDIA_JSON_KEY_TYPE) {
                    media_json_note_type(
                        media_json_nearest_object(stack, depth - 1u),
                        source, begin, end);
                }
                continue;
            }
            if (frame->expect_key) {
                frame->pending_key = media_json_key(
                    source, begin, end);
                frame->expect_key = false;
                continue;
            }
            switch (frame->pending_key) {
                case MEDIA_JSON_KEY_TYPE:
                    media_json_note_type(frame, source, begin, end);
                    break;
                case MEDIA_JSON_KEY_CONTENT_URL:
                    frame->content_begin = begin;
                    frame->content_end = end;
                    break;
                case MEDIA_JSON_KEY_NAME:
                    frame->name_begin = begin;
                    frame->name_end = end;
                    break;
                case MEDIA_JSON_KEY_URL:
                    frame->page_begin = begin;
                    frame->page_end = end;
                    break;
                case MEDIA_JSON_KEY_THUMBNAIL:
                    frame->thumbnail_begin = begin;
                    frame->thumbnail_end = end;
                    break;
                case MEDIA_JSON_KEY_DURATION:
                    frame->duration_begin = begin;
                    frame->duration_end = end;
                    break;
                default:
                    break;
            }
            frame->pending_key = MEDIA_JSON_KEY_NONE;
            continue;
        }
        /* Numbers, booleans and null are irrelevant metadata values. Skip
           one bounded primitive token; any other punctuation is malformed. */
        if (value == '-' || isdigit(value)
            || value == 't' || value == 'f' || value == 'n') {
            while (at < length && !isspace((unsigned char) source[at])
                   && source[at] != ',' && source[at] != ']'
                   && source[at] != '}') at++;
            if (depth != 0 && stack[depth - 1u].object) {
                stack[depth - 1u].pending_key = MEDIA_JSON_KEY_NONE;
            }
            continue;
        }
        malformed = true;
        break;
    }
    if (depth != 0) malformed = true;
    if (malformed) index->malformed_scripts++;
    return !malformed;
}

bool media_discover_structured_audio(
    const PocDocument *document, MediaStructuredAudioIndex *index)
{
    if (index == NULL) return false;
    memset(index, 0, sizeof(*index));
    if (document == NULL || document->html == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t dom_nodes = 0;
    for (lxb_dom_node_t *node = root;
         node != NULL && dom_nodes < MEDIA_DISCOVERY_NODE_LIMIT
         && index->inspected_bytes < MEDIA_DISCOVERY_TEXT_LIMIT;
         node = media_walk_next(root, node)) {
        dom_nodes++;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !media_name_is(node, "script")) continue;
        bool strict_json = false;
        if (!media_script_is_data(node, &strict_json)) continue;
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text == NULL || length == 0) continue;
            size_t remaining = MEDIA_DISCOVERY_TEXT_LIMIT
                - index->inspected_bytes;
            size_t admitted = length < remaining ? length : remaining;
            size_t malformed_before = index->malformed_scripts;
            (void) media_scan_structured_json(text, admitted, index);
            /* Assignment-only object literals are useful data carriers but
               are not JSON and may legally contain unquoted identifiers.
               Keep them fail-soft without calling that expected syntax a
               malformed JSON document in diagnostics. */
            if (!strict_json) {
                index->malformed_scripts = malformed_before;
            }
            index->inspected_bytes += admitted;
            if (admitted != length) index->truncated_scripts++;
            if (index->inspected_bytes == MEDIA_DISCOVERY_TEXT_LIMIT) break;
        }
    }
    /* DOM visits and JSON containers are separate useful diagnostics. Keep
       the public node count as their bounded sum. */
    index->inspected_nodes += dom_nodes;
    return index->candidate_count != 0;
}

static bool media_structured_copy_span(
    const MediaStructuredAudioCandidate *candidate,
    uint32_t begin, uint32_t end, char *output, size_t capacity)
{
    if (output != NULL && capacity != 0) output[0] = '\0';
    if (candidate == NULL || candidate->source == NULL || output == NULL
        || capacity < 2u || end <= begin) return false;
    return media_copy_json_string(
        output, capacity, candidate->source, begin, end) != 0;
}

bool media_structured_audio_copy_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->url_begin, candidate->url_end,
        output, capacity);
}

bool media_structured_audio_copy_name(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->name_begin, candidate->name_end,
        output, capacity);
}

bool media_structured_audio_copy_page_url(
    const MediaStructuredAudioCandidate *candidate,
    char *output, size_t capacity)
{
    return candidate != NULL && media_structured_copy_span(
        candidate, candidate->page_begin, candidate->page_end,
        output, capacity);
}
