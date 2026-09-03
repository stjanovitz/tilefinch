#include "tilefinch/reader_mode.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/platform.h"
#include "tilefinch/media_discovery.h"

#include <lexbor/dom/interfaces/element.h>

#define READER_NODE_LIMIT 8192u
#define READER_DEPTH_LIMIT 128u
#define READER_ENTRY_LIMIT 64u
#define READER_PART_SCAN_LIMIT 256u
#define READER_MARKER_LIMIT 640u
#define READER_LABEL_LIMIT 192u
#define READER_EXTRACT_NODE_LIMIT 512u
#define READER_EXTRACT_VISIT_LIMIT 4096u
#define READER_EXTRACT_DEPTH_LIMIT 64u
#define READER_EXTRACT_BYTE_LIMIT (256u * 1024u)
#define READER_EXTRACT_TAIL_RESERVE 2048u
#define READER_TEXT_WORK_SLICE 4096u
#define READER_INLINE_STYLE_SCAN_LIMIT 2048u
#define READER_BASIC_FORM_LIMIT 8u
#define READER_BASIC_CONTROL_LIMIT 32u
#define READER_BASIC_FORM_SCAN_LIMIT 192u
#define READER_BASIC_SELECT_OPTION_LIMIT 128u
#define READER_BASIC_SELECT_SCAN_LIMIT 1024u
#define READER_NODE_WORK_SLICE 128u
#define READER_ANCHOR_VALUE_LIMIT 192u
#define READER_EXTRACTED_ANCHOR_PREFIX "tilefinch-extracted-"
#define READER_INVALID_INDEX UINT16_MAX

enum {
    READER_STAT_EXCLUDED = 1u << 0,
    READER_STAT_IMAGE = 1u << 1,
    READER_STAT_META = 1u << 2,
    READER_STAT_HEADING = 1u << 3,
    READER_STAT_OWN_HIDDEN = 1u << 4,
    READER_STAT_OWN_REGION = 1u << 5,
    READER_STAT_OWN_LINK = 1u << 6,
    READER_STAT_OWN_STYLE_HIDDEN = 1u << 7
};

typedef struct {
    lxb_dom_node_t *node;
    uint32_t text_bytes;
    uint32_t link_bytes;
    uint16_t parent;
    uint16_t paragraphs;
    uint8_t flags;
    uint8_t own_flags;
} ReaderNodeStat;

typedef struct {
    uint16_t title;
    uint16_t container;
    const char *href;
    const char *label;
    uint16_t href_length;
    uint16_t label_length;
    uint16_t title_quality;
} ReaderEntry;

typedef struct {
    lxb_dom_node_t *node;
    const char *name;
} ReaderMarkerUndo;

typedef struct {
    ReaderMarkerUndo *items;
    size_t count;
    size_t capacity;
    bool dry_run;
} ReaderMutationJournal;

typedef struct {
    const PocDocument *document;
    char *data;
    size_t length;
    size_t capacity;
    size_t content_limit;
    size_t nodes;
    size_t visited_nodes;
    size_t escaped_input_bytes;
    size_t next_cooperate_at;
    size_t next_node_cooperate_at;
    uint32_t visible_text_bytes;
    uint16_t retained_forms;
    uint16_t retained_controls;
    uint16_t mapped_anchors;
    bool truncated;
    bool tail_mode;
    bool basic_mode;
    bool bounded_out;
} ReaderExtractBuffer;

typedef struct {
    uint32_t bytes;
    bool pending_space;
} ReaderVisibleTextScan;

typedef struct {
    bool seen;
    bool hidden;
    bool important;
} ReaderInlineVisibility;

static bool reader_name_is(const lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *actual = document_element_name(
        (lxb_dom_node_t *) node, &length);
    size_t wanted = strlen(name);
    return actual != NULL && length == wanted
        && strncasecmp(actual, name, wanted) == 0;
}

static bool reader_has_attribute(lxb_dom_node_t *node, const char *name)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && name != NULL
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name));
}

static bool reader_slice_contains_ci(const char *text, size_t length,
                                     const char *needle)
{
    if (text == NULL || needle == NULL) return false;
    size_t wanted = strlen(needle);
    if (wanted == 0 || wanted > length) return false;
    for (size_t i = 0; i <= length - wanted; i++) {
        if (strncasecmp(text + i, needle, wanted) == 0) return true;
    }
    return false;
}

static uint32_t reader_add_u32(uint32_t left, uint32_t right)
{
    return right > UINT32_MAX - left ? UINT32_MAX : left + right;
}

static void reader_visible_text_scan(ReaderVisibleTextScan *scan,
                                     const char *text, size_t length)
{
    if (scan == NULL) return;
    for (size_t i = 0; text != NULL && i < length; i++) {
        unsigned char value = (unsigned char) text[i];
        if (isspace(value)) {
            scan->pending_space = scan->bytes != 0;
        } else {
            if (scan->pending_space && scan->bytes != UINT32_MAX)
                scan->bytes++;
            if (scan->bytes != UINT32_MAX) scan->bytes++;
            scan->pending_space = false;
        }
    }
}

static uint32_t reader_visible_text_bytes(const char *text, size_t length)
{
    ReaderVisibleTextScan scan = {0};
    reader_visible_text_scan(&scan, text, length);
    return scan.bytes;
}

static bool reader_text_has_duration(const char *text, size_t length)
{
    for (size_t i = 0; text != NULL && i + 3u < length; i++) {
        if (!isdigit((unsigned char) text[i])) continue;
        size_t at = i;
        while (at < length && isdigit((unsigned char) text[at])
               && at - i < 3u) at++;
        if (at == length || text[at] != ':') continue;
        at++;
        if (at + 1u < length && isdigit((unsigned char) text[at])
            && isdigit((unsigned char) text[at + 1u])) return true;
    }
    return false;
}

static bool reader_text_has_meta(const char *text, size_t length)
{
    return reader_text_has_duration(text, length)
        || reader_slice_contains_ci(text, length, " views")
        || reader_slice_contains_ci(text, length, " view")
        || reader_slice_contains_ci(text, length, " watched");
}

static bool reader_css_name_is(const char *text, size_t length,
                               const char *wanted)
{
    while (length != 0 && isspace((unsigned char) text[0])) {
        text++;
        length--;
    }
    while (length != 0 && isspace((unsigned char) text[length - 1u]))
        length--;
    size_t wanted_length = strlen(wanted);
    return length == wanted_length
        && strncasecmp(text, wanted, wanted_length) == 0;
}

/* Normalize one deliberately small inline-style value. Whitespace and CSS
   comments are insignificant for the simple keywords/numbers admitted here;
   quotes and functions are left unsupported so a content string containing
   "display:none" cannot hide an otherwise visible subtree. */
static bool reader_css_simple_value(const char *text, size_t length,
                                    char *normalized, size_t capacity,
                                    bool *important)
{
    if (normalized == NULL || capacity == 0 || important == NULL)
        return false;
    size_t used = 0;
    for (size_t at = 0; text != NULL && at < length;) {
        if (isspace((unsigned char) text[at])) {
            at++;
            continue;
        }
        if (at + 1u < length && text[at] == '/' && text[at + 1u] == '*') {
            at += 2u;
            while (at + 1u < length
                   && !(text[at] == '*' && text[at + 1u] == '/')) at++;
            if (at + 1u >= length) return false;
            at += 2u;
            continue;
        }
        unsigned char value = (unsigned char) text[at++];
        if (value == '\'' || value == '"' || value == '(' || value == ')'
            || value == ';' || value == ':') return false;
        if (used + 1u >= capacity) return false;
        normalized[used++] = (char) tolower(value);
    }
    normalized[used] = '\0';
    static const char suffix[] = "!important";
    size_t suffix_length = sizeof(suffix) - 1u;
    *important = used >= suffix_length
        && memcmp(normalized + used - suffix_length,
                  suffix, suffix_length) == 0;
    if (*important) {
        used -= suffix_length;
        normalized[used] = '\0';
    }
    return used != 0;
}

static bool reader_css_value_is_any(const char *value,
                                    const char *const *wanted,
                                    size_t wanted_count)
{
    for (size_t i = 0; i < wanted_count; i++)
        if (strcmp(value, wanted[i]) == 0) return true;
    return false;
}

static bool reader_css_opacity(const char *value, bool *hidden)
{
    if (hidden == NULL) return false;
    static const char *const visible_keywords[] = {
        "initial", "unset", "revert", "revert-layer"
    };
    if (reader_css_value_is_any(
            value, visible_keywords,
            sizeof(visible_keywords) / sizeof(visible_keywords[0]))) {
        *hidden = false;
        return true;
    }
    size_t at = 0;
    bool negative = value[at] == '-';
    if (value[at] == '+' || value[at] == '-') at++;
    bool digit = false;
    bool dot = false;
    bool zero = true;
    for (; value[at] != '\0'; at++) {
        if (value[at] == '%' && value[at + 1u] == '\0') break;
        if (value[at] == '.') {
            if (dot) return false;
            dot = true;
            continue;
        }
        if (!isdigit((unsigned char) value[at])) return false;
        if (value[at] != '0') zero = false;
        digit = true;
    }
    if (!digit) return false;
    /* CSS clamps an admitted negative <alpha-value> to zero. */
    *hidden = zero || negative;
    return true;
}

static void reader_inline_visibility_update(ReaderInlineVisibility *state,
                                            bool hidden, bool important)
{
    if (state == NULL) return;
    if (!state->seen || important || !state->important) {
        state->seen = true;
        state->hidden = hidden;
        state->important = important;
    }
}

static bool reader_inline_style_hidden(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *style = document_attribute(node, "style", &length);
    if (style == NULL || length == 0) return false;
    if (length > READER_INLINE_STYLE_SCAN_LIMIT)
        length = READER_INLINE_STYLE_SCAN_LIMIT;
    ReaderInlineVisibility display = {0};
    ReaderInlineVisibility visibility = {0};
    ReaderInlineVisibility opacity = {0};
    for (size_t at = 0; at < length;) {
        size_t start = at;
        size_t colon = SIZE_MAX;
        unsigned char quote = 0;
        unsigned int parentheses = 0;
        bool escaped = false;
        bool comment = false;
        for (; at < length; at++) {
            unsigned char value = (unsigned char) style[at];
            if (comment) {
                if (value == '*' && at + 1u < length
                    && style[at + 1u] == '/') {
                    comment = false;
                    at++;
                }
                continue;
            }
            if (quote != 0) {
                if (escaped) escaped = false;
                else if (value == '\\') escaped = true;
                else if (value == quote) quote = 0;
                continue;
            }
            if (value == '/' && at + 1u < length
                && style[at + 1u] == '*') {
                comment = true;
                at++;
            } else if (value == '\'' || value == '"') {
                quote = value;
            } else if (value == '(') {
                if (parentheses != UINT_MAX) parentheses++;
            } else if (value == ')' && parentheses != 0) {
                parentheses--;
            } else if (value == ':' && parentheses == 0
                       && colon == SIZE_MAX) {
                colon = at;
            } else if (value == ';' && parentheses == 0) {
                break;
            }
        }
        size_t end = at;
        if (at < length) at++;
        if (colon == SIZE_MAX || colon <= start || colon >= end) continue;
        char value[40];
        bool important = false;
        if (!reader_css_simple_value(
                style + colon + 1u, end - colon - 1u,
                value, sizeof(value), &important)) continue;
        if (reader_css_name_is(style + start, colon - start, "display")) {
            static const char *const visible_values[] = {
                "block", "inline", "inline-block", "inlineblock",
                "flex", "inline-flex", "inlineflex", "grid",
                "inline-grid", "inlinegrid", "table", "contents",
                "list-item", "listitem", "flow-root", "flowroot",
                "initial", "unset", "revert", "revert-layer"
            };
            if (strcmp(value, "none") == 0) {
                reader_inline_visibility_update(
                    &display, true, important);
            } else if (reader_css_value_is_any(
                           value, visible_values,
                           sizeof(visible_values)
                               / sizeof(visible_values[0]))) {
                reader_inline_visibility_update(
                    &display, false, important);
            }
        } else if (reader_css_name_is(
                       style + start, colon - start, "visibility")) {
            static const char *const visible_values[] = {
                "visible", "initial", "unset", "revert", "revert-layer"
            };
            if (strcmp(value, "hidden") == 0
                || strcmp(value, "collapse") == 0) {
                reader_inline_visibility_update(
                    &visibility, true, important);
            } else if (reader_css_value_is_any(
                           value, visible_values,
                           sizeof(visible_values)
                               / sizeof(visible_values[0]))) {
                reader_inline_visibility_update(
                    &visibility, false, important);
            }
        } else if (reader_css_name_is(
                       style + start, colon - start, "opacity")) {
            bool hidden = false;
            if (reader_css_opacity(value, &hidden))
                reader_inline_visibility_update(
                    &opacity, hidden, important);
        }
    }
    return (display.seen && display.hidden)
        || (visibility.seen && visibility.hidden)
        || (opacity.seen && opacity.hidden);
}

static bool reader_hidden_element(lxb_dom_node_t *node)
{
    if (reader_name_is(node, "head") || reader_name_is(node, "script")
        || reader_name_is(node, "style") || reader_name_is(node, "template")
        || reader_name_is(node, "noscript")) return true;
    size_t length = 0;
    if (reader_has_attribute(node, "hidden")) return true;
    const char *aria_hidden = document_attribute(node, "aria-hidden", &length);
    if (aria_hidden != NULL && length == 4u
        && strncasecmp(aria_hidden, "true", 4u) == 0) return true;
    /* The style fallback can deliberately reveal a useful server-rendered
       html/body shell after failed hydration. Do not countermand that policy
       here; inline visibility only filters descendant duplicates. */
    if (reader_name_is(node, "html") || reader_name_is(node, "body"))
        return false;
    if (reader_has_attribute(node, "inert")) return true;
    if (reader_name_is(node, "dialog")
        && !reader_has_attribute(node, "open")) return true;
    return reader_inline_style_hidden(node);
}

static bool reader_attribute_has_any_token(
    lxb_dom_node_t *node, const char *name,
    const char *const *wanted, size_t wanted_count)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    for (size_t at = 0; value != NULL && at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) value[at])) at++;
        for (size_t i = 0; i < wanted_count; i++) {
            size_t wanted_length = strlen(wanted[i]);
            if (at - start == wanted_length
                && strncasecmp(
                    value + start, wanted[i], wanted_length) == 0)
                return true;
        }
    }
    return false;
}

static bool reader_header_is_page_chrome(lxb_dom_node_t *node)
{
    if (!reader_name_is(node, "header")) return false;
    lxb_dom_node_t *ancestor = node->parent;
    static const char *const content_roles[] = { "article", "main" };
    for (size_t depth = 0; ancestor != NULL && depth < 64u;
         depth++, ancestor = ancestor->parent) {
        if (reader_name_is(ancestor, "article")
            || reader_name_is(ancestor, "main")
            || reader_attribute_has_any_token(
                ancestor, "role", content_roles,
                sizeof(content_roles) / sizeof(content_roles[0])))
            return false;
        if (reader_name_is(ancestor, "body")) return true;
    }
    /* A malformed or excessively deep ancestry must not let page chrome
       dominate the bounded Reader score. */
    return true;
}

static bool reader_engine_media_marker(
    const PocDocument *document, lxb_dom_node_t *node)
{
    return document_is_declared_video_card(document, node);
}

static bool reader_excluded_region(
    const PocDocument *document, lxb_dom_node_t *node)
{
    if (reader_engine_media_marker(document, node)) return true;
    if (reader_name_is(node, "nav") || reader_name_is(node, "aside")
        || reader_name_is(node, "footer") || reader_name_is(node, "form")
        || reader_name_is(node, "menu")
        || reader_header_is_page_chrome(node)) return true;
    static const char *const roles[] = {
        "banner", "navigation", "search", "complementary", "contentinfo"
    };
    return reader_attribute_has_any_token(
        node, "role", roles, sizeof(roles) / sizeof(roles[0]));
}

static bool reader_document_has_video_element(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL) return false;
    lxb_dom_node_t *node = body;
    lxb_dom_node_t *boundary = body->parent;
    size_t visited = 0u;
    while (node != NULL && node != boundary && visited++ < READER_NODE_LIMIT) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && reader_name_is(node, "video")
            && !document_is_declared_video_card(document, node)) return true;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary && node->next == NULL)
            node = node->parent;
        node = node == NULL || node == boundary ? NULL : node->next;
    }
    return false;
}

static bool reader_primary_media_element(lxb_dom_node_t *node)
{
    if (!reader_name_is(node, "video") && !reader_name_is(node, "audio"))
        return false;
    size_t length = 0;
    const char *source = document_attribute(node, "src", &length);
    while (source != NULL && length != 0
           && isspace((unsigned char) source[0])) {
        source++;
        length--;
    }
    while (length != 0 && isspace((unsigned char) source[length - 1u]))
        length--;
    if (source != NULL && length != 0) return true;
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL; child = child->next) {
        if (!reader_name_is(child, "source")) continue;
        source = document_attribute(child, "src", &length);
        while (source != NULL && length != 0
               && isspace((unsigned char) source[0])) {
            source++;
            length--;
        }
        while (length != 0 && isspace((unsigned char) source[length - 1u]))
            length--;
        if (source != NULL && length != 0) return true;
    }
    return false;
}

static bool reader_head_media_hint(lxb_dom_node_t *node)
{
    if (reader_name_is(node, "meta")) {
        size_t property_length = 0, content_length = 0;
        const char *property = document_attribute(
            node, "property", &property_length);
        const char *content = document_attribute(
            node, "content", &content_length);
        return property != NULL && content != NULL
            && reader_slice_contains_ci(
                property, property_length, "og:type")
            && reader_slice_contains_ci(content, content_length, "video");
    }
    if (!reader_name_is(node, "script")) return false;
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    if (type == NULL
        || !reader_slice_contains_ci(type, type_length, "ld+json")) {
        return false;
    }
    size_t inspected = 0;
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL && inspected < 4096u; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        if (length > 4096u - inspected) length = 4096u - inspected;
        if (reader_slice_contains_ci(text, length, "videoobject")) return true;
        inspected += length;
    }
    return false;
}

static lxb_dom_node_t *reader_next_within(lxb_dom_node_t *node,
                                          lxb_dom_node_t *boundary)
{
    if (node == NULL) return NULL;
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != boundary) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static bool reader_document_head_media_hint(const PocDocument *document)
{
    if (document == NULL || document->html == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *head = NULL;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 64u; visited++) {
        if (reader_name_is(node, "head")) {
            head = node;
            break;
        }
        node = reader_next_within(node, root);
    }
    if (head == NULL) return false;
    node = head->first_child;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        if (reader_head_media_hint(node)) return true;
        node = reader_next_within(node, head);
    }
    return false;
}

static bool reader_extract_append(ReaderExtractBuffer *output,
                                  const char *text, size_t length)
{
    if (output == NULL || text == NULL) return false;
    size_t limit = output->tail_mode
        ? output->capacity : output->content_limit;
    if (output->length >= limit
        || length > limit - output->length - 1u) {
        if (!output->tail_mode) output->truncated = true;
        return false;
    }
    memcpy(output->data + output->length, text, length);
    output->length += length;
    output->data[output->length] = '\0';
    return true;
}

static bool reader_extract_literal(ReaderExtractBuffer *output,
                                   const char *text)
{
    return reader_extract_append(output, text, strlen(text));
}

static bool reader_extract_escaped(ReaderExtractBuffer *output,
                                   const char *text, size_t length,
                                   bool attribute, bool *meaningful)
{
    if (output == NULL || (text == NULL && length != 0)) return false;
    if (meaningful != NULL) *meaningful = false;
    size_t at = 0;
    while (at < length) {
        size_t until_checkpoint = output->next_cooperate_at
            > output->escaped_input_bytes
            ? output->next_cooperate_at - output->escaped_input_bytes
            : READER_TEXT_WORK_SLICE;
        size_t chunk = length - at;
        if (chunk > until_checkpoint) chunk = until_checkpoint;
        size_t end = at + chunk;
        size_t run = at;
        for (size_t i = at; i < end; i++) {
            if (meaningful != NULL
                && !isspace((unsigned char) text[i])) *meaningful = true;
            const char *escape = NULL;
            switch ((unsigned char) text[i]) {
            case '&': escape = "&amp;"; break;
            case '<': escape = "&lt;"; break;
            case '>': escape = "&gt;"; break;
            case '"': if (attribute) escape = "&quot;"; break;
            default: break;
            }
            if (escape == NULL) continue;
            if (i != run
                && !reader_extract_append(output, text + run, i - run))
                return false;
            if (!reader_extract_literal(output, escape)) return false;
            run = i + 1u;
        }
        if (end != run
            && !reader_extract_append(output, text + run, end - run))
            return false;
        output->escaped_input_bytes += chunk;
        at = end;
        if (output->escaped_input_bytes >= output->next_cooperate_at) {
            if (!tilefinch_platform_cooperate(
                    "reader-extract", output->escaped_input_bytes))
                return false;
            if (output->next_cooperate_at
                > SIZE_MAX - READER_TEXT_WORK_SLICE) {
                output->next_cooperate_at = SIZE_MAX;
            } else {
                output->next_cooperate_at += READER_TEXT_WORK_SLICE;
            }
        }
    }
    return true;
}

static bool reader_extract_declared_video(
    ReaderExtractBuffer *output, const MediaDeclaredVideo *declared)
{
    if (output == NULL || declared == NULL
        || declared->media_url[0] == '\0') return false;
    if (!reader_extract_literal(
            output,
            "<video controls data-tilefinch-declared-media-card=reader "
            "style=\"display:block;width:100%;height:135px;max-height:50vh;"
            "box-sizing:border-box;margin:8px 0;border:2px solid #547696;"
            "border-radius:8px;background:#162431\" aria-label=\""))
        return false;
    const char *label = declared->title[0] == '\0'
        ? "Play declared video in Tilefinch" : declared->title;
    if (!reader_extract_escaped(
            output, label, strlen(label), true, NULL)
        || !reader_extract_literal(output, "\"")) return false;
    if (declared->thumbnail_url[0] != '\0') {
        if (!reader_extract_literal(output, " poster=\"")
            || !reader_extract_escaped(
                output, declared->thumbnail_url,
                strlen(declared->thumbnail_url), true, NULL)
            || !reader_extract_literal(output, "\"")) return false;
    }
    if (declared->duration[0] != '\0') {
        if (!reader_extract_literal(
                output, " data-tilefinch-media-duration=\"")
            || !reader_extract_escaped(
                output, declared->duration, strlen(declared->duration),
                true, NULL)
            || !reader_extract_literal(output, "\"")) return false;
    }
    return reader_extract_literal(output, "></video>");
}

static bool reader_attribute_equals_ci_trimmed(
    lxb_dom_node_t *node, const char *name, const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    while (value != NULL && length != 0
           && isspace((unsigned char) value[0])) {
        value++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) value[length - 1u])) length--;
    size_t wanted_length = strlen(wanted);
    return value != NULL && length == wanted_length
        && strncasecmp(value, wanted, wanted_length) == 0;
}

static bool reader_basic_descends_from(lxb_dom_node_t *node,
                                       lxb_dom_node_t *ancestor,
                                       bool *bounded_out)
{
    lxb_dom_node_t *at = node;
    size_t visited = 0;
    for (; at != NULL && visited++ <= READER_EXTRACT_DEPTH_LIMIT;
         at = at->parent) {
        if (at == ancestor) return true;
    }
    if (at != NULL && bounded_out != NULL) *bounded_out = true;
    return false;
}

/* Preserve the HTML disabled inheritance which the extracted markup would
   otherwise lose when fieldset/optgroup wrappers are flattened. Failure to
   prove the state within a bound omits the control and marks complete Basic
   admission bounded, so no partially actionable clone can be installed. */
static bool reader_basic_effectively_disabled(lxb_dom_node_t *node,
                                               bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    if (reader_has_attribute(node, "disabled")) return true;
    if (reader_name_is(node, "option")) {
        lxb_dom_node_t *at = node == NULL ? NULL : node->parent;
        size_t visited = 0;
        for (; at != NULL && visited++ <= READER_EXTRACT_DEPTH_LIMIT;
             at = at->parent) {
            if ((reader_name_is(at, "optgroup")
                 || reader_name_is(at, "select"))
                && reader_has_attribute(at, "disabled")) return true;
            if (reader_name_is(at, "select")) {
                at = NULL;
                break;
            }
        }
        if (at != NULL) {
            if (bounded_out != NULL) *bounded_out = true;
            return true;
        }
    }
    lxb_dom_node_t *at = node == NULL ? NULL : node->parent;
    size_t visited = 0;
    for (; at != NULL && visited++ <= READER_EXTRACT_DEPTH_LIMIT;
         at = at->parent) {
        if (!reader_name_is(at, "fieldset")
            || !reader_has_attribute(at, "disabled")) continue;
        lxb_dom_node_t *legend = NULL;
        lxb_dom_node_t *child = at->first_child;
        size_t children_visited = 0;
        for (; child != NULL
               && children_visited++ < READER_BASIC_FORM_SCAN_LIMIT;
             child = child->next) {
            if (reader_name_is(child, "legend")) {
                legend = child;
                break;
            }
        }
        if (legend == NULL && child != NULL) {
            if (bounded_out != NULL) *bounded_out = true;
            return true;
        }
        bool ancestry_bounded = false;
        if (legend != NULL
            && reader_basic_descends_from(
                   node, legend, &ancestry_bounded)) continue;
        if (ancestry_bounded) {
            if (bounded_out != NULL) *bounded_out = true;
            return true;
        }
        return true;
    }
    if (at != NULL) {
        if (bounded_out != NULL) *bounded_out = true;
        return true;
    }
    return false;
}

static bool reader_basic_query_input(lxb_dom_node_t *node,
                                     bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    if (!reader_name_is(node, "input")) return false;
    size_t name_length = 0;
    const char *name = document_attribute(node, "name", &name_length);
    if (name == NULL || name_length == 0) return false;
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    bool query_type = type == NULL || type_length == 0
        || reader_attribute_equals_ci_trimmed(node, "type", "text")
        || reader_attribute_equals_ci_trimmed(node, "type", "search");
    if (!query_type) return false;
    return !reader_basic_effectively_disabled(node, bounded_out);
}

static bool reader_basic_form_method_get(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *method = document_attribute(node, "method", &length);
    return method == NULL || length == 0
        || reader_attribute_equals_ci_trimmed(node, "method", "get");
}

static bool reader_basic_control_has_own_label(lxb_dom_node_t *node,
                                               bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    size_t length = 0;
    const char *label = document_attribute(node, "aria-label", &length);
    if (label != NULL && length != 0) return true;
    label = document_attribute(node, "placeholder", &length);
    if (label != NULL && length != 0) return true;
    lxb_dom_node_t *at = node == NULL ? NULL : node->parent;
    size_t visited = 0;
    for (; at != NULL && visited++ <= READER_EXTRACT_DEPTH_LIMIT;
         at = at->parent) {
        if (reader_name_is(at, "label")) return true;
        if (reader_name_is(at, "form")) break;
    }
    if (at != NULL && !reader_name_is(at, "form")
        && bounded_out != NULL) *bounded_out = true;
    return false;
}

static bool reader_basic_label_targets(
    lxb_dom_node_t *label, const char *id, size_t id_length)
{
    if (!reader_name_is(label, "label") || id == NULL || id_length == 0)
        return false;
    size_t for_length = 0;
    const char *for_value = document_attribute(label, "for", &for_length);
    return for_value != NULL && for_length == id_length
        && memcmp(for_value, id, id_length) == 0;
}

/* Admit a form by authored semantics, never by button text or click-handler
   guesses. The scan is constant-storage and bounded independently from the
   complete-document extractor. */
static bool reader_basic_form_admitted(lxb_dom_node_t *form,
                                       bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    if (!reader_name_is(form, "form")
        || !reader_basic_form_method_get(form)) return false;
    size_t label_length = 0;
    const char *form_label = document_attribute(
        form, "aria-label", &label_length);
    bool form_is_search = form_label != NULL && label_length != 0;
    if (!form_is_search) {
        size_t role_length = 0;
        const char *role = document_attribute(form, "role", &role_length);
        form_is_search = role != NULL && role_length == 6u
            && strncasecmp(role, "search", 6u) == 0;
    }
    lxb_dom_node_t *query = NULL;
    lxb_dom_node_t *node = form->first_child;
    size_t visited = 0;
    for (; node != NULL && visited++ < READER_BASIC_FORM_SCAN_LIMIT;) {
        bool disabled_scan_bounded = false;
        if (reader_basic_query_input(node, &disabled_scan_bounded)) {
            query = node;
            break;
        }
        if (disabled_scan_bounded) {
            if (bounded_out != NULL) *bounded_out = true;
            return false;
        }
        node = reader_next_within(node, form);
    }
    if (query == NULL) {
        if (node != NULL && bounded_out != NULL) *bounded_out = true;
        return false;
    }
    bool label_scan_bounded = false;
    if (form_is_search
        || reader_basic_control_has_own_label(
               query, &label_scan_bounded))
        return true;
    if (label_scan_bounded) {
        if (bounded_out != NULL) *bounded_out = true;
        return false;
    }
    size_t id_length = 0;
    const char *id = document_attribute(query, "id", &id_length);
    node = form->first_child;
    visited = 0;
    for (; node != NULL && visited++ < READER_BASIC_FORM_SCAN_LIMIT;) {
        if (reader_basic_label_targets(node, id, id_length)) return true;
        node = reader_next_within(node, form);
    }
    if (node != NULL && bounded_out != NULL) *bounded_out = true;
    return false;
}

static bool reader_basic_submit_button(lxb_dom_node_t *node)
{
    if (!reader_name_is(node, "button")) return false;
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    return type == NULL || length == 0
        || reader_attribute_equals_ci_trimmed(node, "type", "submit");
}

static bool reader_basic_form_control(lxb_dom_node_t *node,
                                      bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    if (reader_basic_effectively_disabled(node, bounded_out)) return false;
    if (reader_name_is(node, "input")) {
        size_t length = 0;
        const char *type = document_attribute(node, "type", &length);
        return type == NULL || length == 0
            || reader_attribute_equals_ci_trimmed(node, "type", "text")
            || reader_attribute_equals_ci_trimmed(node, "type", "search")
            || reader_attribute_equals_ci_trimmed(node, "type", "hidden")
            || reader_attribute_equals_ci_trimmed(node, "type", "submit");
    }
    if (reader_name_is(node, "select")) {
        /* The Basic projection currently retains one selected option and its
           controller serializes one value. Refuse a multiple-select at the
           complete-extraction boundary rather than silently changing its
           successful-controls semantics. */
        if (reader_has_attribute(node, "multiple")) {
            if (bounded_out != NULL) *bounded_out = true;
            return false;
        }
        size_t option_count = 0;
        size_t visited = 0;
        lxb_dom_node_t *at = node->first_child;
        for (; at != NULL && visited++ < READER_BASIC_SELECT_SCAN_LIMIT;
             at = reader_next_within(at, node)) {
            if (reader_name_is(at, "option")
                && ++option_count > READER_BASIC_SELECT_OPTION_LIMIT) {
                if (bounded_out != NULL) *bounded_out = true;
                return false;
            }
        }
        if (at != NULL) {
            if (bounded_out != NULL) *bounded_out = true;
            return false;
        }
        return true;
    }
    return reader_name_is(node, "option")
        || reader_name_is(node, "textarea")
        || reader_basic_submit_button(node);
}

static const char *reader_extract_tag(lxb_dom_node_t *node,
                                      bool basic_mode,
                                      bool admitted_form,
                                      bool basic_control_safe)
{
    static const char *const tags[] = {
        "article", "section", "main", "header", "h1", "h2", "h3", "h4",
        "h5", "h6", "p", "ul", "ol", "li", "dl", "dt", "dd", "blockquote",
        "figure", "figcaption", "table", "caption", "thead", "tbody",
        "tfoot", "tr", "th",
        "td", "pre", "code", "kbd", "samp", "a", "img", "picture",
        "video", "audio", "source", "br", "hr", "strong", "em", "b",
        "i", "time", "sup", "sub", "details", "summary", "abbr",
        "mark", "cite", "q"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (reader_name_is(node, tags[i])) return tags[i];
    if (basic_mode) {
        static const char *const basic_tags[] = {
            "nav", "aside", "footer", "menu", "address", "form",
            "label", "input", "select", "option", "textarea", "button"
        };
        for (size_t i = 0;
             i < sizeof(basic_tags) / sizeof(basic_tags[0]); i++) {
            if (!reader_name_is(node, basic_tags[i])) continue;
            if (reader_name_is(node, "form"))
                return admitted_form ? "form" : NULL;
            if (reader_name_is(node, "label"))
                return admitted_form ? "label" : NULL;
            if (reader_name_is(node, "input")
                || reader_name_is(node, "select")
                || reader_name_is(node, "option")
                || reader_name_is(node, "textarea")
                || reader_name_is(node, "button")) {
                return admitted_form && basic_control_safe
                    ? basic_tags[i] : NULL;
            }
            return basic_tags[i];
        }
    }
    return NULL;
}

static bool reader_extract_attribute(ReaderExtractBuffer *output,
                                     lxb_dom_node_t *node,
                                     const char *name)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    if (value == NULL || length == 0) return true;
    return reader_extract_append(output, " ", 1u)
        && reader_extract_literal(output, name)
        && reader_extract_literal(output, "=\"")
        && reader_extract_escaped(output, value, length, true, NULL)
        && reader_extract_append(output, "\"", 1u);
}

/* Live form values are DOM properties, so they deliberately do not rewrite
   the authored content attribute. Basic is a one-time native projection,
   however, and must snapshot the value visible at the moment it is prepared.
   Keep this separate from reader_extract_attribute so an explicitly empty
   live value still emits value="" rather than falling back to the authored
   default when the extracted markup is parsed. */
static bool reader_extract_value_attribute(ReaderExtractBuffer *output,
                                           const char *name,
                                           const char *value,
                                           size_t length)
{
    return output != NULL && name != NULL && value != NULL
        && reader_extract_append(output, " ", 1u)
        && reader_extract_literal(output, name)
        && reader_extract_literal(output, "=\"")
        && reader_extract_escaped(output, value, length, true, NULL)
        && reader_extract_append(output, "\"", 1u);
}

static bool reader_extract_boolean_attribute(ReaderExtractBuffer *output,
                                             lxb_dom_node_t *node,
                                             const char *name);

static bool reader_extract_option_selected(ReaderExtractBuffer *output,
                                           lxb_dom_node_t *node)
{
    size_t marker_length = 0;
    const char *marker = document_attribute(
        node, "data-tilefinch-option-selected", &marker_length);
    /* Keep authored selected as the extracted control's reset default, then
       project the current property through the same bounded marker consumed
       by layout and native form submission. The live marker is authoritative
       even when false. Malformed source markers follow those consumers'
       fail-soft unselected interpretation and are normalized rather than
       copied verbatim. */
    if (!reader_extract_boolean_attribute(output, node, "selected"))
        return false;
    if (marker == NULL) return true;
    bool selected = marker_length == 4u
        && memcmp(marker, "true", 4u) == 0;
    return reader_extract_value_attribute(
        output, "data-tilefinch-option-selected",
        selected ? "true" : "false", selected ? 4u : 5u);
}

static bool reader_extract_prefixed_anchor_attribute(
    ReaderExtractBuffer *output, lxb_dom_node_t *node, const char *name,
    bool count_mapping, bool *mapped_out)
{
    if (mapped_out != NULL) *mapped_out = false;
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    if (value == NULL || length == 0) return true;
    if (length > READER_ANCHOR_VALUE_LIMIT) {
        output->bounded_out = true;
        return true;
    }
    if (!reader_extract_append(output, " ", 1u)
        || !reader_extract_literal(output, name)
        || !reader_extract_literal(output, "=\"")
        || !reader_extract_literal(output, READER_EXTRACTED_ANCHOR_PREFIX)
        || !reader_extract_escaped(output, value, length, true, NULL)
        || !reader_extract_append(output, "\"", 1u)) return false;
    if (count_mapping && output->mapped_anchors != UINT16_MAX)
        output->mapped_anchors++;
    if (mapped_out != NULL) *mapped_out = true;
    return true;
}

static bool reader_extract_href(ReaderExtractBuffer *output,
                                lxb_dom_node_t *node)
{
    /* Keep the authored URL in history. While an extracted presentation is
       active the native fragment resolver prefers the prefixed clone target,
       then falls back to the raw id/name. */
    return reader_extract_attribute(output, node, "href");
}

static bool reader_extract_boolean_attribute(ReaderExtractBuffer *output,
                                             lxb_dom_node_t *node,
                                             const char *name)
{
    if (!reader_has_attribute(node, name)) return true;
    return reader_extract_append(output, " ", 1u)
        && reader_extract_literal(output, name);
}

static void reader_extract_rewind(ReaderExtractBuffer *output,
                                  size_t checkpoint)
{
    if (output == NULL || checkpoint > output->length) return;
    output->length = checkpoint;
    output->data[checkpoint] = '\0';
}

static bool reader_extract_separator(ReaderExtractBuffer *output)
{
    if (output == NULL || output->length == 0
        || isspace((unsigned char) output->data[output->length - 1u]))
        return true;
    return reader_extract_append(output, " ", 1u);
}

static bool reader_flattened_block_wrapper(lxb_dom_node_t *node)
{
    return reader_name_is(node, "address") || reader_name_is(node, "center")
        || reader_name_is(node, "dialog") || reader_name_is(node, "div")
        || reader_name_is(node, "fieldset") || reader_name_is(node, "hgroup");
}

static bool reader_prune_when_empty(const char *tag)
{
    if (tag == NULL) return false;
    static const char *const tags[] = {
        "a", "p", "li", "ul", "ol", "dl", "dt", "dd", "blockquote",
        "figure", "figcaption", "caption", "h1", "h2", "h3", "h4",
        "h5", "h6", "header", "summary", "picture"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (strcmp(tag, tags[i]) == 0) return true;
    return false;
}

static bool reader_extract_node(ReaderExtractBuffer *output,
                                lxb_dom_node_t *node, size_t depth,
                                bool in_admitted_form,
                                bool *meaningful_content)
{
    if (meaningful_content != NULL) *meaningful_content = false;
    if (output == NULL || node == NULL) return false;
    if (output->truncated) return true;
    if (depth > READER_EXTRACT_DEPTH_LIMIT
        || output->visited_nodes++ >= READER_EXTRACT_VISIT_LIMIT) {
        output->truncated = true;
        return true;
    }
    if (output->visited_nodes >= output->next_node_cooperate_at) {
        if (!tilefinch_platform_cooperate(
                "reader-extract-nodes", output->visited_nodes)) {
            output->bounded_out = true;
            return false;
        }
        if (output->next_node_cooperate_at
            > SIZE_MAX - READER_NODE_WORK_SLICE) {
            output->next_node_cooperate_at = SIZE_MAX;
        } else {
            output->next_node_cooperate_at += READER_NODE_WORK_SLICE;
        }
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        size_t checkpoint = output->length;
        if (text == NULL) return true;
        bool meaningful = false;
        if (reader_extract_escaped(
                output, text, length, false, &meaningful)) {
            if (meaningful) {
                output->visible_text_bytes = reader_add_u32(
                    output->visible_text_bytes,
                    reader_visible_text_bytes(text, length));
            }
            if (meaningful_content != NULL) *meaningful_content = meaningful;
            return true;
        }
        if (!output->truncated) return false;
        reader_extract_rewind(output, checkpoint);
        return true;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || reader_hidden_element(node)
        || reader_engine_media_marker(output->document, node)
        || (!output->basic_mode
            && reader_excluded_region(output->document, node)))
        return true;
    bool admitted_form = in_admitted_form;
    if (reader_name_is(node, "form")) {
        admitted_form = false;
        bool form_scan_bounded = false;
        if (output->basic_mode && !in_admitted_form
            && reader_basic_form_admitted(node, &form_scan_bounded)) {
            if (output->retained_forms < READER_BASIC_FORM_LIMIT) {
                output->retained_forms++;
                admitted_form = true;
            } else {
                output->bounded_out = true;
            }
        }
        if (form_scan_bounded) output->bounded_out = true;
    }
    bool basic_control_node = reader_name_is(node, "input")
        || reader_name_is(node, "select")
        || reader_name_is(node, "option")
        || reader_name_is(node, "textarea")
        || reader_name_is(node, "button");
    bool control_state_bounded = false;
    bool basic_control_safe = !basic_control_node
        || (output->basic_mode && admitted_form
            && reader_basic_form_control(node, &control_state_bounded));
    if (control_state_bounded) output->bounded_out = true;
    bool counted_control = basic_control_safe && output->basic_mode
        && admitted_form
        && (reader_name_is(node, "input")
            || reader_name_is(node, "select")
            || reader_name_is(node, "textarea")
            || reader_name_is(node, "button"));
    bool control_admitted = true;
    if (counted_control) {
        if (output->retained_controls >= READER_BASIC_CONTROL_LIMIT) {
            output->bounded_out = true;
            control_admitted = false;
        } else {
            output->retained_controls++;
        }
    }
    const char *tag = control_admitted
        ? reader_extract_tag(
              node, output->basic_mode, admitted_form, basic_control_safe)
        : NULL;
    const char *image_source = NULL;
    size_t image_source_length = 0;
    if (tag != NULL && strcmp(tag, "img") == 0) {
        image_source = document_attribute(
            node, "src", &image_source_length);
        if (image_source == NULL || image_source_length == 0
            || reader_slice_contains_ci(
                   image_source, image_source_length, "data:image")) {
            static const char *const lazy[] = {
                "data-src", "data-original", "data-thumb", "data-lazy-src"
            };
            image_source = NULL;
            image_source_length = 0;
            for (size_t i = 0; i < sizeof(lazy) / sizeof(lazy[0]); i++) {
                image_source = document_attribute(
                    node, lazy[i], &image_source_length);
                if (image_source != NULL && image_source_length != 0) break;
            }
        }
        /* A placeholder without a usable source paints its alt text into a
           zero-height replaced box in the bounded renderer. The caption and
           surrounding prose remain; omit the broken visual entirely. */
        if (image_source == NULL || image_source_length == 0) tag = NULL;
    }
    bool emitted = tag != NULL;
    size_t flattened_id_length = 0;
    const char *flattened_id = emitted ? NULL
        : document_attribute(node, "id", &flattened_id_length);
    bool marker_emitted = flattened_id != NULL
        && flattened_id_length != 0
        && flattened_id_length <= READER_ANCHOR_VALUE_LIMIT;
    if (flattened_id != NULL
        && flattened_id_length > READER_ANCHOR_VALUE_LIMIT)
        output->bounded_out = true;
    bool separated_wrapper = !emitted && reader_flattened_block_wrapper(node);
    if ((emitted || marker_emitted)
        && output->nodes >= READER_EXTRACT_NODE_LIMIT) {
        /* Preserve a balanced semantic prefix and tell the reader that the
           suffix was intentionally omitted under the device bound. Unknown
           wrapper elements do not consume this scarce emitted-node quota
           unless they carry a fragment target that must remain addressable. */
        output->truncated = true;
        return true;
    }
    size_t checkpoint = output->length;
    size_t nodes_checkpoint = output->nodes;
    uint16_t anchors_checkpoint = output->mapped_anchors;
    if (separated_wrapper && !reader_extract_separator(output))
        goto append_failed;
    if (marker_emitted) {
        /* Unknown wrappers are flattened, but their fragment position is
           still observable. Keep a deliberately empty neutral marker at the
           exact point where the wrapper began. The resolver recognizes the
           marker attribute rather than treating arbitrary zero-sized nodes
           in the hidden source tree as scroll targets. */
        if (!reader_extract_literal(
                output, "<span data-tilefinch-reader-anchor")
            || !reader_extract_prefixed_anchor_attribute(
                   output, node, "id", true, NULL)
            || !reader_extract_literal(output, "></span>"))
            goto append_failed;
        output->nodes++;
    }
    bool retained_anchor_identity = false;
    const char *live_control_value = NULL;
    size_t live_control_value_length = 0;
    if (output->basic_mode && emitted
        && (strcmp(tag, "input") == 0
            || strcmp(tag, "textarea") == 0)) {
        live_control_value = document_control_value(
            node, &live_control_value_length);
    }
    if (emitted) {
        if (!reader_extract_append(output, "<", 1u)
            || !reader_extract_literal(output, tag)) goto append_failed;
        if (!reader_extract_attribute(output, node, "lang")
            || !reader_extract_attribute(output, node, "dir"))
            goto append_failed;
        if (!reader_extract_prefixed_anchor_attribute(
                output, node, "id", true, &retained_anchor_identity))
            goto append_failed;
        if (strcmp(tag, "a") == 0) {
            bool retained_name = false;
            if (!reader_extract_href(output, node)
                || !reader_extract_prefixed_anchor_attribute(
                       output, node, "name", true, &retained_name)
                || !reader_extract_attribute(output, node, "title"))
                goto append_failed;
            retained_anchor_identity = retained_anchor_identity
                || retained_name;
            size_t href_length = 0;
            const char *href = document_attribute(
                node, "href", &href_length);
            if (retained_anchor_identity
                && (href == NULL || href_length == 0u)
                && !reader_extract_literal(
                       output, " data-tilefinch-reader-anchor")) {
                goto append_failed;
            }
        } else if (strcmp(tag, "img") == 0) {
            if (!reader_extract_literal(output, " src=\"")
                    || !reader_extract_escaped(
                        output, image_source, image_source_length, true, NULL)
                    || !reader_extract_append(output, "\"", 1u))
                goto append_failed;
            if (!reader_extract_attribute(output, node, "alt")
                || !reader_extract_attribute(output, node, "width")
                || !reader_extract_attribute(output, node, "height")
                || !reader_extract_attribute(output, node, "srcset")
                || !reader_extract_attribute(output, node, "sizes"))
                goto append_failed;
        } else if (strcmp(tag, "video") == 0
                   || strcmp(tag, "audio") == 0) {
            if (!reader_extract_attribute(output, node, "src")
                || !reader_extract_attribute(output, node, "poster")
                || !reader_extract_boolean_attribute(
                       output, node, "controls")
                || !reader_extract_attribute(output, node, "type"))
                goto append_failed;
        } else if (strcmp(tag, "source") == 0) {
            if (!reader_extract_attribute(output, node, "src")
                || !reader_extract_attribute(output, node, "type")
                || !reader_extract_attribute(output, node, "media")
                || !reader_extract_attribute(output, node, "srcset")
                || !reader_extract_attribute(output, node, "sizes"))
                goto append_failed;
        } else if (strcmp(tag, "th") == 0 || strcmp(tag, "td") == 0) {
            if (!reader_extract_attribute(output, node, "colspan")
                || !reader_extract_attribute(output, node, "rowspan")
                || !reader_extract_attribute(output, node, "scope")
                || !reader_extract_attribute(output, node, "headers"))
                goto append_failed;
        } else if (strcmp(tag, "ol") == 0) {
            if (!reader_extract_attribute(output, node, "start")
                || !reader_extract_boolean_attribute(
                       output, node, "reversed"))
                goto append_failed;
        } else if (strcmp(tag, "li") == 0) {
            if (!reader_extract_attribute(output, node, "value"))
                goto append_failed;
        } else if (strcmp(tag, "time") == 0) {
            if (!reader_extract_attribute(output, node, "datetime"))
                goto append_failed;
        } else if (strcmp(tag, "details") == 0) {
            if (!reader_extract_boolean_attribute(output, node, "open"))
                goto append_failed;
        } else if (strcmp(tag, "form") == 0) {
            if (!reader_extract_literal(output, " method=\"get\"")
                || !reader_extract_attribute(output, node, "action")
                || !reader_extract_attribute(output, node, "role")
                || !reader_extract_attribute(output, node, "aria-label"))
                goto append_failed;
        } else if (strcmp(tag, "label") == 0) {
            if (!reader_extract_prefixed_anchor_attribute(
                    output, node, "for", false, NULL)) goto append_failed;
        } else if (strcmp(tag, "input") == 0) {
            if (!reader_extract_attribute(output, node, "type")
                || !reader_extract_attribute(output, node, "name")
                || (live_control_value != NULL
                    ? !reader_extract_value_attribute(
                          output, "value", live_control_value,
                          live_control_value_length)
                    : !reader_extract_attribute(output, node, "value"))
                || !reader_extract_attribute(output, node, "placeholder")
                || !reader_extract_attribute(output, node, "aria-label")
                || !reader_extract_attribute(output, node, "autocomplete")
                || !reader_extract_boolean_attribute(
                       output, node, "readonly")
                || !reader_extract_boolean_attribute(
                       output, node, "required")) goto append_failed;
        } else if (strcmp(tag, "select") == 0
                   || strcmp(tag, "textarea") == 0
                   || strcmp(tag, "button") == 0) {
            if (!reader_extract_attribute(output, node, "name")
                || !reader_extract_attribute(output, node, "value")
                || !reader_extract_attribute(output, node, "type")
                || !reader_extract_attribute(output, node, "placeholder")
                || !reader_extract_attribute(output, node, "aria-label")
                || (strcmp(tag, "textarea") == 0
                    && !reader_extract_boolean_attribute(
                           output, node, "readonly"))
                || !reader_extract_boolean_attribute(
                       output, node, "required")) goto append_failed;
        } else if (strcmp(tag, "option") == 0) {
            if (!reader_extract_attribute(output, node, "value")
                || !reader_extract_option_selected(output, node))
                goto append_failed;
        } else if (strcmp(tag, "abbr") == 0
                   || strcmp(tag, "blockquote") == 0
                   || strcmp(tag, "q") == 0) {
            const char *attribute = strcmp(tag, "abbr") == 0
                ? "title" : "cite";
            if (!reader_extract_attribute(output, node, attribute))
                goto append_failed;
        }
        if (!reader_extract_append(output, ">", 1u)) goto append_failed;
        output->nodes++;
    }
    bool meaningful = emitted
        && (strcmp(tag, "img") == 0 || strcmp(tag, "video") == 0
            || strcmp(tag, "audio") == 0 || strcmp(tag, "form") == 0
            || strcmp(tag, "input") == 0 || strcmp(tag, "select") == 0
            || strcmp(tag, "textarea") == 0
            || strcmp(tag, "button") == 0);
    bool child_in_admitted_form = admitted_form;
    if (output->basic_mode && admitted_form
        && (!control_admitted || !basic_control_safe)
        && (reader_name_is(node, "select")
            || reader_name_is(node, "textarea")
            || reader_name_is(node, "button"))) {
        child_in_admitted_form = false;
    }
    if (emitted && strcmp(tag, "textarea") == 0
        && live_control_value != NULL) {
        bool value_meaningful = false;
        if (!reader_extract_escaped(
                output, live_control_value, live_control_value_length,
                false, &value_meaningful)) goto append_failed;
        if (value_meaningful) {
            output->visible_text_bytes = reader_add_u32(
                output->visible_text_bytes,
                reader_visible_text_bytes(
                    live_control_value, live_control_value_length));
        }
    } else {
        for (lxb_dom_node_t *child = node->first_child;
             child != NULL && !output->truncated; child = child->next) {
            bool child_meaningful = false;
            if (!reader_extract_node(
                    output, child, depth + 1u, child_in_admitted_form,
                    &child_meaningful)) return false;
            meaningful = meaningful || child_meaningful;
        }
    }
    if (emitted && strcmp(tag, "img") != 0 && strcmp(tag, "source") != 0
        && strcmp(tag, "input") != 0
        && strcmp(tag, "br") != 0 && strcmp(tag, "hr") != 0) {
        bool previous_tail_mode = output->tail_mode;
        if (output->truncated) output->tail_mode = true;
        if (!reader_extract_literal(output, "</")
            || !reader_extract_literal(output, tag)
            || !reader_extract_append(output, ">", 1u)) {
            output->tail_mode = previous_tail_mode;
            return false;
        }
        output->tail_mode = previous_tail_mode;
    }
    if (!output->truncated && emitted && !meaningful
        && !retained_anchor_identity && reader_prune_when_empty(tag)) {
        reader_extract_rewind(output, checkpoint);
        output->nodes = nodes_checkpoint;
        output->mapped_anchors = anchors_checkpoint;
        return true;
    }
    if (!output->truncated && separated_wrapper
        && !reader_extract_separator(output)) goto append_failed;
    if (meaningful_content != NULL) *meaningful_content = meaningful;
    return true;

append_failed:
    if (!output->truncated) return false;
    reader_extract_rewind(output, checkpoint);
    output->nodes = nodes_checkpoint;
    output->mapped_anchors = anchors_checkpoint;
    return true;
}

static bool reader_path_has_segment(const char *href, size_t length,
                                    const char *segment)
{
    size_t wanted = strlen(segment);
    for (size_t i = 0; i + wanted <= length; i++) {
        size_t after = i + wanted;
        if (href[i] == '/' && strncasecmp(href + i, segment, wanted) == 0
            && (after == length || href[after] == '/'
                || href[after] == '?' || href[after] == '#'
                || href[after] == '&' || href[after] == '='))
            return true;
    }
    return false;
}

static bool reader_date_shaped_path(const char *href, size_t length)
{
    for (size_t i = 0; i + 8u <= length; i++) {
        if (href[i] != '/') continue;
        const char *at = href + i + 1u;
        bool year_month = isdigit((unsigned char) at[0])
            && isdigit((unsigned char) at[1])
            && isdigit((unsigned char) at[2])
            && isdigit((unsigned char) at[3])
            && (at[4] == '-' || at[4] == '/')
            && isdigit((unsigned char) at[5])
            && isdigit((unsigned char) at[6]);
        if (!year_month) continue;
        size_t after_month = i + 8u;
        if (after_month == length || href[after_month] == '/'
            || href[after_month] == '-' || href[after_month] == '?'
            || href[after_month] == '#') return true;
    }
    return false;
}

static bool reader_watch_href(const char *href, size_t length)
{
    if (href == NULL || length == 0 || reader_date_shaped_path(href, length))
        return false;
    static const char *const excluded[] = {
        "/tag", "/tags", "/category", "/categories", "/search",
        "/page", "/pagination", "/login", "/signin", "/register"
    };
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
        if (reader_path_has_segment(href, length, excluded[i])) return false;
    if (reader_slice_contains_ci(href, length, "?viewkey=")
        || reader_slice_contains_ci(href, length, "&viewkey=")
        || reader_path_has_segment(href, length, "/watch")
        || reader_slice_contains_ci(href, length, "/video-")
        || reader_slice_contains_ci(href, length, "/video.")) return true;
    for (size_t i = 0; i < length; i++) {
        if (href[i] != '/' || i + 4u >= length) continue;
        size_t at = i + 1u;
        size_t digits = 0;
        while (at < length && isdigit((unsigned char) href[at])) {
            at++;
            digits++;
        }
        if (digits >= 4u
            && (at == length || href[at] == '/' || href[at] == '?'
                || href[at] == '#' || href[at] == '.')) return true;
    }
    return false;
}

static const char *reader_nested_image_alt(lxb_dom_node_t *anchor,
                                           size_t *length)
{
    *length = 0;
    lxb_dom_node_t *boundary = anchor == NULL ? NULL : anchor->parent;
    lxb_dom_node_t *node = anchor;
    size_t visited = 0;
    while (node != NULL && node != boundary && visited++ < 64u) {
        if (reader_name_is(node, "img")) {
            const char *alt = document_attribute(node, "alt", length);
            if (alt != NULL && *length != 0) return alt;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary) break;
        }
    }
    return NULL;
}

static bool reader_title_badge_element(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    size_t length = 0;
    const char *class_name = document_attribute(node, "class", &length);
    return reader_slice_contains_ci(class_name, length, "badge")
        || reader_slice_contains_ci(class_name, length, "quality");
}

static uint32_t reader_anchor_title_bytes(lxb_dom_node_t *anchor)
{
    lxb_dom_node_t *boundary = anchor == NULL ? NULL : anchor->parent;
    lxb_dom_node_t *node = anchor;
    size_t visited = 0;
    size_t hidden_depth = 0;
    uint32_t bytes = 0;
    while (node != NULL && node != boundary && visited++ < 64u) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node != anchor && reader_title_badge_element(node))
            hidden_depth++;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT && hidden_depth == 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            bytes = reader_add_u32(
                bytes, reader_visible_text_bytes(text, length));
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                && node != anchor && reader_title_badge_element(node))
                hidden_depth--;
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary) break;
        }
    }
    return bytes;
}

static uint16_t reader_title_quality(lxb_dom_node_t *anchor,
                                     const ReaderNodeStat *stat,
                                     const char **label,
                                     uint16_t *label_length)
{
    *label = NULL;
    *label_length = 0;
    size_t length = 0;
    const char *title = document_attribute(anchor, "title", &length);
    if (title == NULL || length == 0)
        title = document_attribute(anchor, "aria-label", &length);
    if (title != NULL && length != 0) {
        *label = title;
        *label_length = (uint16_t) (length > READER_LABEL_LIMIT
            ? READER_LABEL_LIMIT : length);
        return (uint16_t) (49152u
            + (length > 16383u ? 16383u : length));
    }
    uint32_t text = stat == NULL ? 0u : reader_anchor_title_bytes(anchor);
    if (text != 0) return (uint16_t) (32768u
        + (text > 16383u ? 16383u : text));
    title = reader_nested_image_alt(anchor, &length);
    if (title != NULL && length != 0) {
        *label = title;
        *label_length = (uint16_t) (length > READER_LABEL_LIMIT
            ? READER_LABEL_LIMIT : length);
        return (uint16_t) (16384u
            + (length > 16383u ? 16383u : length));
    }
    return 0;
}

static uint16_t reader_common_ancestor(const ReaderNodeStat *stats,
                                       uint16_t left, uint16_t right)
{
    for (size_t left_depth = 0;
         left != READER_INVALID_INDEX && left_depth < 6u;
         left_depth++, left = stats[left].parent) {
        uint16_t candidate = right;
        for (size_t right_depth = 0;
             candidate != READER_INVALID_INDEX && right_depth < 6u;
             right_depth++, candidate = stats[candidate].parent)
            if (candidate == left) return left;
    }
    return READER_INVALID_INDEX;
}

static uint16_t reader_entry_container(
    const ReaderNodeStat *stats, uint16_t anchor_index)
{
    uint16_t at = stats[anchor_index].parent;
    uint16_t fallback = anchor_index;
    for (size_t depth = 0; at != READER_INVALID_INDEX && depth < 4u;
         depth++, at = stats[at].parent) {
        fallback = at;
        if ((stats[at].flags & (READER_STAT_IMAGE | READER_STAT_META)) != 0)
            return fallback;
    }
    return fallback;
}

static bool reader_journal_contains(const ReaderMutationJournal *journal,
                                    lxb_dom_node_t *node, const char *name)
{
    for (size_t i = 0; journal != NULL && i < journal->count; i++)
        if (journal->items[i].node == node
            && strcmp(journal->items[i].name, name) == 0) return true;
    return false;
}

static bool reader_set_marker_slice(ReaderMutationJournal *journal,
                                    lxb_dom_node_t *node, const char *name,
                                    const char *value, size_t length)
{
    if (journal == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || value == NULL || length == 0) return false;
    if (reader_journal_contains(journal, node, name)) return true;
    if (reader_has_attribute(node, name)
        || journal->count == journal->capacity) return false;
    if (journal->dry_run) {
        journal->items[journal->count++] = (ReaderMarkerUndo) {
            .node = node,
            .name = name
        };
        return true;
    }
    if (lxb_dom_element_set_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) name, strlen(name),
            (const lxb_char_t *) value, length) == NULL) return false;
    journal->items[journal->count++] = (ReaderMarkerUndo) {
        .node = node,
        .name = name
    };
    return true;
}

static bool reader_set_marker(ReaderMutationJournal *journal,
                              lxb_dom_node_t *node, const char *name,
                              const char *value)
{
    return reader_set_marker_slice(
        journal, node, name, value, value == NULL ? 0u : strlen(value));
}

static void reader_rollback_markers(ReaderMutationJournal *journal)
{
    if (journal != NULL && journal->dry_run) {
        journal->count = 0;
        return;
    }
    while (journal != NULL && journal->count != 0) {
        ReaderMarkerUndo *undo = &journal->items[--journal->count];
        (void) lxb_dom_element_remove_attribute(
            lxb_dom_interface_element(undo->node),
            (const lxb_char_t *) undo->name, strlen(undo->name));
    }
}

static bool reader_mark_path(ReaderMutationJournal *journal,
                             lxb_dom_node_t *node, lxb_dom_node_t *body)
{
    for (node = node == NULL ? NULL : node->parent;
         node != NULL; node = node->parent) {
        if (!reader_set_marker(
                journal, node, "data-tilefinch-reader-path", "1"))
            return false;
        if (node == body) return true;
    }
    return false;
}

static bool reader_descends_from(const ReaderNodeStat *stats,
                                 uint16_t child, uint16_t ancestor)
{
    for (uint16_t at = child; at != READER_INVALID_INDEX;
         at = stats[at].parent)
        if (at == ancestor) return true;
    return false;
}

static bool reader_mark_entry_parts(const ReaderNodeStat *stats,
                                    size_t count, ReaderEntry *entry,
                                    ReaderMutationJournal *journal)
{
    uint16_t container = entry->container;
    uint16_t title = entry->title;
    if (container >= count || title >= count
        || !reader_set_marker(journal, stats[container].node,
                              "data-tilefinch-reader-entry", "1")
        || !reader_set_marker(journal, stats[title].node,
                              "data-tilefinch-reader-title", "1"))
        return false;
    if (entry->label != NULL && entry->label_length != 0
        && !reader_set_marker_slice(
               journal, stats[title].node, "data-tilefinch-reader-label",
               entry->label, entry->label_length)) return false;
    bool thumbnail_marked = false;
    bool meta_marked = false;
    for (size_t i = container + 1u; i < count; i++) {
        if (!reader_descends_from(stats, (uint16_t) i, container)) break;
        if (!thumbnail_marked && reader_name_is(stats[i].node, "img")) {
            thumbnail_marked = reader_set_marker(
                journal, stats[i].node,
                "data-tilefinch-reader-thumb", "1");
            if (!thumbnail_marked) return false;
        }
        if (!meta_marked && (stats[i].own_flags & READER_STAT_META) != 0) {
            meta_marked = reader_set_marker(
                journal, stats[i].node,
                "data-tilefinch-reader-meta", "1");
            if (!meta_marked) return false;
        }
        size_t class_length = 0;
        const char *class_name = document_attribute(
            stats[i].node, "class", &class_length);
        if (i != title && reader_descends_from(stats, (uint16_t) i, title)
            && stats[i].text_bytes <= 16u
            && ((stats[i].own_flags & READER_STAT_META) != 0
                || reader_slice_contains_ci(
                       class_name, class_length, "badge")
                || reader_slice_contains_ci(
                       class_name, class_length, "quality"))) {
            if (!reader_set_marker(
                    journal, stats[i].node,
                    "data-tilefinch-reader-hide", "1"))
                return false;
        }
    }
    return true;
}

static bool reader_prepare_listing(const ReaderNodeStat *stats, size_t count,
                                   ReaderEntry *entries, size_t entry_count,
                                   lxb_dom_node_t *body,
                                   ReaderDocumentAnalysis *analysis,
                                   uint16_t *listing_root,
                                   ReaderMutationJournal *journal)
{
    uint16_t best_root = READER_INVALID_INDEX;
    size_t best_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        uint16_t container = entries[i].container;
        uint16_t root = container >= count
            ? READER_INVALID_INDEX : stats[container].parent;
        if (root == READER_INVALID_INDEX) continue;
        size_t clustered = 0;
        for (size_t j = 0; j < entry_count; j++)
            if (entries[j].container < count
                && stats[entries[j].container].parent == root) clustered++;
        if (clustered > best_count) {
            best_count = clustered;
            best_root = root;
        }
    }
    if (best_count < 8u || best_root == READER_INVALID_INDEX) return true;
    if (!reader_set_marker(
            journal, stats[best_root].node,
            "data-tilefinch-reader-list", "1")
        || !reader_mark_path(journal, stats[best_root].node, body)) return false;
    size_t marked = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].container >= count
            || stats[entries[i].container].parent != best_root) continue;
        if (!reader_mark_entry_parts(
                stats, count, &entries[i], journal)) return false;
        marked++;
    }
    analysis->listing_entries = (uint16_t) marked;
    *listing_root = best_root;
    return true;
}

static bool reader_prepare_article(const ReaderNodeStat *stats, size_t count,
                                   lxb_dom_node_t *body,
                                   ReaderDocumentAnalysis *analysis,
                                   uint16_t *article_root,
                                   ReaderMutationJournal *journal,
                                   bool declared_media)
{
    uint16_t winner = READER_INVALID_INDEX;
    uint32_t best_score = 0;
    for (size_t i = 0; i < count; i++) {
        const ReaderNodeStat *stat = &stats[i];
        if ((stat->flags & READER_STAT_EXCLUDED) != 0
            || stat->text_bytes < 240u
            || !(reader_name_is(stat->node, "article")
                 || reader_name_is(stat->node, "main")
                 || reader_name_is(stat->node, "section")
                 || reader_name_is(stat->node, "div"))) continue;
        uint32_t content_bytes = stat->text_bytes > stat->link_bytes
            ? stat->text_bytes - stat->link_bytes : 0u;
        if (content_bytes <= stat->text_bytes / 2u) continue;
        uint32_t score = reader_add_u32(
            content_bytes, (uint32_t) stat->paragraphs * 80u);
        /* Prefer the page's authored semantic article over an otherwise
           identical main/div wrapper. This also gives auto-Reader a concrete
           content-shape signal instead of treating every prose-heavy landing
           page as a high-confidence article. */
        if (reader_name_is(stat->node, "article"))
            score = reader_add_u32(score, 160u);
        if (score > best_score) {
            best_score = score;
            winner = (uint16_t) i;
        }
    }
    if (winner == READER_INVALID_INDEX) return true;
    const ReaderNodeStat *selected = &stats[winner];
    uint32_t visible = analysis->visible_text_bytes;
    uint32_t threshold = (visible / 100u) * 35u
        + ((visible % 100u) * 35u + 99u) / 100u;
    bool dominant = selected->text_bytes >= (declared_media ? 256u : 600u)
        && selected->paragraphs >= (declared_media ? 2u : 3u)
        && visible != 0 && selected->text_bytes >= threshold;
    if (!dominant) return true;
    if (!reader_set_marker(journal, selected->node,
                           "data-tilefinch-reader-article", "1")
        || !reader_mark_path(journal, selected->node, body)) return false;
    *article_root = winner;
    return true;
}

static bool reader_prepare_watch(const ReaderNodeStat *stats, size_t count,
                                 uint16_t media_index,
                                 lxb_dom_node_t *body,
                                 uint16_t *article_root,
                                 ReaderMutationJournal *journal)
{
    uint16_t winner = READER_INVALID_INDEX;
    if (media_index < count) {
        winner = media_index;
        bool semantic_root = false;
        for (size_t depth = 0; winner != READER_INVALID_INDEX && depth < 6u;
             depth++) {
            if (reader_name_is(stats[winner].node, "main")
                || reader_name_is(stats[winner].node, "article")) {
                semantic_root = true;
                break;
            }
            uint16_t parent = stats[winner].parent;
            if (parent == READER_INVALID_INDEX || parent == 0u) break;
            winner = parent;
        }
        if (!semantic_root || winner == READER_INVALID_INDEX
            || (stats[winner].flags & READER_STAT_HEADING) == 0) {
            winner = READER_INVALID_INDEX;
        }
    } else {
        uint32_t best_score = 0;
        for (size_t i = 1; i < count; i++) {
            const ReaderNodeStat *stat = &stats[i];
            if ((stat->flags & READER_STAT_EXCLUDED) != 0
                || !(reader_name_is(stat->node, "main")
                     || reader_name_is(stat->node, "article"))) continue;
            uint32_t content = stat->text_bytes > stat->link_bytes
                ? stat->text_bytes - stat->link_bytes : 0u;
            uint32_t score = reader_add_u32(content,
                reader_name_is(stat->node, "article") ? 256u : 128u);
            if (score > best_score) {
                best_score = score;
                winner = (uint16_t) i;
            }
        }
    }
    if (winner == READER_INVALID_INDEX || winner == 0u) return true;
    if (!reader_set_marker(journal, stats[winner].node,
                           "data-tilefinch-reader-article", "1")
        || !reader_mark_path(journal, stats[winner].node, body)) return false;
    *article_root = winner;
    return true;
}

/* One ownership boundary for every extracted presentation. The caller owns
   body markers and document_refresh rollback; this helper only constructs,
   populates, and connects the bounded native root. */
static bool reader_commit_extracted_root(
    PocDocument *document, lxb_dom_node_t *body,
    const ReaderExtractBuffer *output, ReaderPageKind kind,
    bool connect_root, lxb_dom_node_t **installed_root)
{
    if (installed_root != NULL) *installed_root = NULL;
    if (document == NULL || document->html == NULL || body == NULL
        || output == NULL || output->data == NULL) return false;
    static const lxb_char_t root_name[] = "main";
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, root_name,
        sizeof(root_name) - 1u, NULL);
    lxb_dom_node_t *root = element == NULL
        ? NULL : lxb_dom_interface_node(element);
    const char *kind_name = reader_page_kind_name(kind);
    bool okay = root != NULL;
    if (okay) okay = lxb_dom_element_set_attribute(
        element, (const lxb_char_t *) "data-tilefinch-reader-root",
        sizeof("data-tilefinch-reader-root") - 1u,
        (const lxb_char_t *) kind_name, strlen(kind_name)) != NULL;
    if (okay && output->truncated)
        okay = lxb_dom_element_set_attribute(
            element,
            (const lxb_char_t *) "data-tilefinch-reader-truncated",
            sizeof("data-tilefinch-reader-truncated") - 1u,
            (const lxb_char_t *) "1", 1u) != NULL;
    if (okay) okay = lxb_dom_element_set_attribute(
        element, (const lxb_char_t *) "hidden", 6u,
        (const lxb_char_t *) "hidden", 6u) != NULL;
    if (okay) okay = document_set_element_inner_html(
        document, root, output->data, output->length);
    if (okay && connect_root) okay = lxb_dom_node_append_child(body, root)
        == LXB_DOM_EXCEPTION_OK;
    if (!okay && root != NULL) {
        if (root->parent != NULL) lxb_dom_node_remove(root);
        lxb_dom_node_destroy_deep(root);
    }
    if (okay && installed_root != NULL) *installed_root = root;
    return okay;
}

/* This search runs only over a freshly parsed, native-authored Reader root,
   before author script can run. It converts the diagnostic marker into exact
   pointer provenance used by layout and activation. */
static lxb_dom_node_t *reader_find_generated_media_card(
    lxb_dom_node_t *root)
{
    if (root == NULL) return NULL;
    for (lxb_dom_node_t *node = root->first_child;
         node != NULL; node = node->next) {
        size_t marker_length = 0u;
        const char *marker = document_attribute(
            node, "data-tilefinch-declared-media-card", &marker_length);
        if (marker != NULL && marker_length == sizeof("reader") - 1u
            && memcmp(marker, "reader", sizeof("reader") - 1u) == 0) {
            return node;
        }
    }
    return NULL;
}

static void reader_forget_generated_media_card(PocDocument *document)
{
    if (document != NULL) document->reader_declared_video_card_node = NULL;
}

static bool reader_install_extracted_tree(
    PocDocument *document, lxb_dom_node_t *body,
    const ReaderNodeStat *stats, size_t count,
    ReaderPageKind kind, uint16_t article_root, uint16_t listing_root,
    const MediaDeclaredVideo *declared_video,
    bool synthesize_declared_video,
    ReaderDocumentAnalysis *analysis, bool install,
    bool require_complete, bool connect_root,
    bool *meaningful_output, lxb_dom_node_t **installed_root)
{
    if (meaningful_output != NULL) *meaningful_output = false;
    if (installed_root != NULL) *installed_root = NULL;
    if (document == NULL || document->budget == NULL || document->html == NULL
        || body == NULL || stats == NULL || count == 0
        || kind == READER_PAGE_RAW) return false;
    char *markup = budget_malloc_category(
        document->budget, BUDGET_CATEGORY_DOM, READER_EXTRACT_BYTE_LIMIT);
    if (markup == NULL) return false;
    ReaderExtractBuffer output = {
        .document = document,
        .data = markup,
        .capacity = READER_EXTRACT_BYTE_LIMIT,
        .content_limit = READER_EXTRACT_BYTE_LIMIT
            - READER_EXTRACT_TAIL_RESERVE,
        .next_cooperate_at = READER_TEXT_WORK_SLICE,
        .next_node_cooperate_at = READER_NODE_WORK_SLICE
    };
    markup[0] = '\0';
    bool okay = reader_extract_literal(
        &output, "<header><strong>Reader</strong></header>");
    if (okay && synthesize_declared_video)
        okay = reader_extract_declared_video(&output, declared_video);
    bool article_meaningful = false;
    if (okay && article_root < count) {
        okay = reader_extract_node(
            &output, stats[article_root].node, 0u, false,
            &article_meaningful);
    }
    bool listing_meaningful = false;
    if (okay && !output.truncated && listing_root < count) {
        if (kind == READER_PAGE_WATCH)
            okay = reader_extract_literal(&output, "<h2>More</h2>");
        if (okay) {
            okay = reader_extract_node(
                &output, stats[listing_root].node, 0u, false,
                &listing_meaningful);
        }
    }
    if (okay && output.truncated) {
        output.tail_mode = true;
        okay = reader_extract_literal(
            &output,
            "<p data-tilefinch-reader-truncated=\"1\">"
            "Reader view shortened to fit this device.</p>");
    }
    if (!okay) {
        budget_free(document->budget, markup);
        return false;
    }
    bool meaningful = kind == READER_PAGE_LISTING
        ? listing_meaningful : article_meaningful;
    if (okay && !meaningful) {
        if (analysis != NULL) {
            analysis->mapped_anchors = output.mapped_anchors;
            if (output.truncated) analysis->extraction_truncated = true;
            if (output.truncated || output.bounded_out)
                analysis->bounded_out = true;
        }
        budget_free(document->budget, markup);
        return true;
    }
    if (meaningful_output != NULL) *meaningful_output = true;
    if (analysis != NULL) {
        analysis->extracted_bytes = output.length > UINT32_MAX
            ? UINT32_MAX : (uint32_t) output.length;
        analysis->extracted_nodes = output.nodes > UINT16_MAX
            ? UINT16_MAX : (uint16_t) output.nodes;
        analysis->mapped_anchors = output.mapped_anchors;
        analysis->extraction_truncated = output.truncated;
        if (output.truncated || output.bounded_out)
            analysis->bounded_out = true;
    }
    /* Strict automatic recovery needs the complete source decision, but it
       must not pay for a second serialize-and-discard pass. Serialize once
       and decline installation transactionally when the bounded result is
       incomplete. Explicit Reader mode continues to admit the labeled,
       shortened tree. */
    if (require_complete
        && (output.truncated || output.bounded_out
            || (analysis != NULL && analysis->bounded_out))) {
        budget_free(document->budget, markup);
        return true;
    }
    if (!install) {
        budget_free(document->budget, markup);
        return true;
    }
    lxb_dom_node_t *root = NULL;
    okay = reader_commit_extracted_root(
        document, body, &output, kind, connect_root, &root);
    if (okay && synthesize_declared_video) {
        lxb_dom_node_t *card = reader_find_generated_media_card(root);
        if (card == NULL) {
            if (root->parent != NULL) lxb_dom_node_remove(root);
            lxb_dom_node_destroy_deep(root);
            root = NULL;
            okay = false;
        } else {
            document->reader_declared_video_card_node = card;
        }
    }
    if (okay && installed_root != NULL) *installed_root = root;
    budget_free(document->budget, markup);
    return okay;
}

static bool reader_document_prepare_internal(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis, bool install, bool require_complete,
    bool connect_root, lxb_dom_node_t **prepared_root)
{
    if (prepared_root != NULL) *prepared_root = NULL;
    if (analysis == NULL) return false;
    *analysis = (ReaderDocumentAnalysis) { .prepared = true };
    lxb_dom_node_t *body = document_body_node(document);
    if (document == NULL || document->budget == NULL || body == NULL)
        return true;
    size_t capacity = document->element_count;
    if (capacity == 0) return true;
    if (capacity > READER_NODE_LIMIT) capacity = READER_NODE_LIMIT;
    ReaderNodeStat *stats = budget_calloc_category(
        document->budget, BUDGET_CATEGORY_DOM, capacity, sizeof(*stats));
    if (stats == NULL) return true;
    uint16_t stack[READER_DEPTH_LIMIT];
    ComputedStyle *style_stack = stylesheet == NULL ? NULL
        : budget_calloc_category(
              document->budget, BUDGET_CATEGORY_DOM,
              READER_DEPTH_LIMIT, sizeof(*style_stack));
    if (stylesheet != NULL && style_stack == NULL) {
        budget_free(document->budget, stats);
        return true;
    }
    ComputedStyle root_style = {0};
    bool has_root_style = style_stack != NULL && body->parent != NULL
        && body->parent->type == LXB_DOM_NODE_TYPE_ELEMENT;
    if (has_root_style)
        root_style = style_for_node(stylesheet, body->parent, NULL);
    size_t depth = 0;
    size_t count = 0;
    size_t hidden_depth = 0;
    size_t style_hidden_depth = 0;
    size_t excluded_depth = 0;
    size_t link_depth = 0;
    bool primary_media = false;
    const MediaDeclaredVideo *declared_video =
        media_declared_video_cached(document);
    bool declared_primary_media = declared_video != NULL;
    bool declared_watch_evidence = declared_primary_media
        && (declared_video->structured || declared_video->og_video
            || declared_video->og_type_video);
    bool head_media_hint = reader_document_head_media_hint(document);
    uint16_t primary_media_index = READER_INVALID_INDEX;
    lxb_dom_node_t *node = body;
    lxb_dom_node_t *boundary = body->parent;
    size_t visited = 0;
    size_t analyzed_text_bytes = 0;
    size_t next_text_cooperate_at = READER_TEXT_WORK_SLICE;
    bool okay = true;
    while (node != NULL && node != boundary) {
        if (++visited > READER_NODE_LIMIT) {
            analysis->bounded_out = true;
            break;
        }
        if ((visited & 255u) == 0
            && !tilefinch_platform_cooperate("reader-analyze", visited)) {
            analysis->bounded_out = true;
            okay = false;
            break;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (count == capacity || depth == READER_DEPTH_LIMIT) {
                analysis->bounded_out = true;
                okay = false;
                break;
            }
            bool hidden = reader_hidden_element(node);
            bool style_hidden = false;
            if (style_stack != NULL) {
                const ComputedStyle *parent_style = depth == 0
                    ? (has_root_style ? &root_style : NULL)
                    : &style_stack[depth - 1u];
                style_stack[depth] = style_for_node(
                    stylesheet, node, parent_style);
                /* Reader deliberately recovers a useful server-rendered body
                   from globally hidden hydration shells. Descendant authored
                   hiding still means a player is not a visible primary media
                   experience, but it must not remove prose from extraction. */
                style_hidden = depth != 0
                    && (style_stack[depth].display == DISPLAY_NONE
                    || style_stack[depth].visibility_hidden
                    || style_stack[depth].opacity == 0u);
            }
            bool excluded = reader_excluded_region(document, node);
            bool link = reader_name_is(node, "a");
            if (hidden) hidden_depth++;
            if (style_hidden) style_hidden_depth++;
            if (excluded) excluded_depth++;
            if (link) link_depth++;
            ReaderNodeStat *stat = &stats[count];
            stat->node = node;
            stat->parent = depth == 0 ? READER_INVALID_INDEX
                : stack[depth - 1u];
            if (hidden_depth != 0 || excluded_depth != 0)
                stat->flags |= READER_STAT_EXCLUDED;
            if (hidden) stat->own_flags |= READER_STAT_OWN_HIDDEN;
            if (style_hidden)
                stat->own_flags |= READER_STAT_OWN_STYLE_HIDDEN;
            if (excluded) stat->own_flags |= READER_STAT_OWN_REGION;
            if (link) stat->own_flags |= READER_STAT_OWN_LINK;
            if (reader_name_is(node, "img"))
                stat->own_flags |= READER_STAT_IMAGE;
            if (reader_name_is(node, "h1") || reader_name_is(node, "h2")
                || reader_name_is(node, "h3"))
                stat->own_flags |= READER_STAT_HEADING;
            if (reader_name_is(node, "p")) stat->paragraphs = 1u;
            stat->flags |= stat->own_flags;
            bool media_element = hidden_depth == 0
                && style_hidden_depth == 0 && excluded_depth == 0
                && reader_primary_media_element(node);
            if (hidden_depth != 0 && reader_head_media_hint(node))
                head_media_hint = true;
            primary_media = primary_media || media_element;
            if (media_element && !reader_name_is(node, "meta")
                && primary_media_index == READER_INVALID_INDEX)
                primary_media_index = (uint16_t) count;
            stack[depth++] = (uint16_t) count++;
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT && depth != 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (hidden_depth == 0 && excluded_depth == 0) {
                ReaderVisibleTextScan text_scan = {0};
                bool has_meta = false;
                for (size_t at = 0; text != NULL && at < length;) {
                    size_t until_checkpoint = next_text_cooperate_at
                        > analyzed_text_bytes
                        ? next_text_cooperate_at - analyzed_text_bytes
                        : READER_TEXT_WORK_SLICE;
                    size_t chunk = length - at;
                    if (chunk > until_checkpoint) chunk = until_checkpoint;
                    reader_visible_text_scan(&text_scan, text + at, chunk);
                    size_t meta_start = at > 16u ? at - 16u : 0u;
                    has_meta = has_meta || reader_text_has_meta(
                        text + meta_start, at + chunk - meta_start);
                    analyzed_text_bytes += chunk;
                    at += chunk;
                    if (analyzed_text_bytes >= next_text_cooperate_at) {
                        if (!tilefinch_platform_cooperate(
                                "reader-analyze-text",
                                analyzed_text_bytes)) {
                            analysis->bounded_out = true;
                            okay = false;
                            break;
                        }
                        if (next_text_cooperate_at
                            > SIZE_MAX - READER_TEXT_WORK_SLICE) {
                            next_text_cooperate_at = SIZE_MAX;
                        } else {
                            next_text_cooperate_at += READER_TEXT_WORK_SLICE;
                        }
                    }
                }
                if (!okay) break;
                uint32_t bytes = text_scan.bytes;
                ReaderNodeStat *stat = &stats[stack[depth - 1u]];
                stat->text_bytes = reader_add_u32(stat->text_bytes, bytes);
                if (link_depth != 0)
                    stat->link_bytes = reader_add_u32(
                        stat->link_bytes, bytes);
                if (has_meta) {
                    stat->flags |= READER_STAT_META;
                    stat->own_flags |= READER_STAT_META;
                }
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                const ReaderNodeStat *closing = depth == 0 ? NULL
                    : &stats[stack[depth - 1u]];
                if (closing != NULL
                    && (closing->own_flags & READER_STAT_OWN_LINK) != 0)
                    link_depth--;
                if (closing != NULL
                    && (closing->own_flags & READER_STAT_OWN_REGION) != 0)
                    excluded_depth--;
                if (closing != NULL
                    && (closing->own_flags & READER_STAT_OWN_HIDDEN) != 0)
                    hidden_depth--;
                if (closing != NULL
                    && (closing->own_flags
                        & READER_STAT_OWN_STYLE_HIDDEN) != 0)
                    style_hidden_depth--;
                depth--;
            }
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary) break;
        }
    }
    analysis->visited_nodes = visited > UINT32_MAX
        ? UINT32_MAX : (uint32_t) visited;
    if (okay) {
        for (size_t i = count; i != 0; i--) {
            ReaderNodeStat *stat = &stats[i - 1u];
            if (stat->parent == READER_INVALID_INDEX) continue;
            ReaderNodeStat *parent = &stats[stat->parent];
            parent->text_bytes = reader_add_u32(
                parent->text_bytes, stat->text_bytes);
            parent->link_bytes = reader_add_u32(
                parent->link_bytes, stat->link_bytes);
            parent->paragraphs = (uint16_t) (
                stat->paragraphs > UINT16_MAX - parent->paragraphs
                    ? UINT16_MAX : parent->paragraphs + stat->paragraphs);
            parent->flags |= stat->flags
                & (READER_STAT_IMAGE | READER_STAT_META
                   | READER_STAT_HEADING);
        }
        if (count != 0)
            analysis->visible_text_bytes = stats[0].text_bytes;

        ReaderEntry entries[READER_ENTRY_LIMIT];
        size_t entry_count = 0;
        memset(entries, 0, sizeof(entries));
        for (size_t i = 0; i < count; i++) {
            if (!reader_name_is(stats[i].node, "a")
                || (stats[i].flags & READER_STAT_EXCLUDED) != 0) continue;
            size_t href_length = 0;
            const char *href = document_attribute(
                stats[i].node, "href", &href_length);
            if (!reader_watch_href(href, href_length)
                || href_length > UINT16_MAX) continue;
            uint16_t container = reader_entry_container(
                stats, (uint16_t) i);
            if (container == READER_INVALID_INDEX
                || (stats[container].flags & READER_STAT_EXCLUDED) != 0
                || (stats[container].flags
                    & (READER_STAT_IMAGE | READER_STAT_META)) == 0) continue;
            size_t found = entry_count;
            for (size_t j = 0; j < entry_count; j++)
                if (entries[j].href_length == href_length
                    && memcmp(entries[j].href, href, href_length) == 0) {
                    found = j;
                    break;
                }
            const char *label = NULL;
            uint16_t label_length = 0;
            uint16_t quality = reader_title_quality(
                stats[i].node, &stats[i], &label, &label_length);
            if (found == entry_count) {
                if (entry_count == READER_ENTRY_LIMIT) continue;
                entries[entry_count++] = (ReaderEntry) {
                    .title = (uint16_t) i,
                    .container = container,
                    .href = href,
                    .label = label,
                    .href_length = (uint16_t) href_length,
                    .label_length = label_length,
                    .title_quality = quality
                };
            } else {
                uint16_t common = reader_common_ancestor(
                    stats, entries[found].container, container);
                if (common != READER_INVALID_INDEX && common != 0u)
                    entries[found].container = common;
                if (quality > entries[found].title_quality) {
                    entries[found].title = (uint16_t) i;
                    entries[found].label = label;
                    entries[found].label_length = label_length;
                    entries[found].title_quality = quality;
                }
            }
        }
        uint16_t listing = READER_INVALID_INDEX;
        uint16_t article = READER_INVALID_INDEX;
        size_t marker_capacity = count + entry_count * 4u + 4u;
        if (marker_capacity > READER_MARKER_LIMIT)
            marker_capacity = READER_MARKER_LIMIT;
        ReaderMarkerUndo *undo = marker_capacity == 0 ? NULL
            : budget_malloc_category(
                  document->budget, BUDGET_CATEGORY_DOM,
                  marker_capacity * sizeof(*undo));
        ReaderMutationJournal journal = {
            .items = undo,
            .capacity = marker_capacity,
            .dry_run = !install
        };
        if (marker_capacity != 0 && undo == NULL) okay = false;
        BudgetAllocationOwner previous =
            document_allocation_owner_enter(document);
        if (okay && primary_media) okay = reader_prepare_watch(
            stats, count, primary_media_index, body, &article, &journal);
        if (okay && !primary_media) okay = reader_prepare_article(
            stats, count, body, analysis, &article, &journal,
            declared_watch_evidence);
        bool declared_watch_root = declared_watch_evidence
            && article < count
            && stats[article].paragraphs >= 2u
            && stats[article].text_bytes >= 256u;
        /* A watch page's related rail is secondary only when a primary
           media/title subtree was actually preserved. With a head-only media
           hint and no trustworthy root, leave the page conservatively raw
           rather than exposing only the recommendations. */
        if (okay && (!primary_media || article != READER_INVALID_INDEX)
            && !(head_media_hint && !primary_media
                 && !declared_watch_root))
            okay = reader_prepare_listing(
                stats, count, entries, entry_count, body, analysis, &listing,
                &journal);
        if (okay) {
            ReaderPageKind kind = (primary_media || declared_watch_root)
                    && article != READER_INVALID_INDEX ? READER_PAGE_WATCH
                : analysis->listing_entries >= 8u ? READER_PAGE_LISTING
                : article != READER_INVALID_INDEX
                    ? READER_PAGE_ARTICLE : READER_PAGE_RAW;
            lxb_dom_node_t *extracted_root = NULL;
            bool extracted_meaningful = false;
            if (kind != READER_PAGE_RAW) {
                const char *kind_name = reader_page_kind_name(kind);
                okay = reader_set_marker(
                    &journal, body, "data-tilefinch-reader-kind", kind_name);
                if (okay) okay = reader_install_extracted_tree(
                    document, body, stats, count, kind, article, listing,
                    declared_video, declared_watch_root && !primary_media,
                    analysis, install, require_complete,
                    connect_root,
                    &extracted_meaningful,
                    &extracted_root);
                if (okay && !extracted_meaningful) {
                    reader_rollback_markers(&journal);
                    analysis->listing_entries = 0u;
                    kind = READER_PAGE_RAW;
                } else if (okay && require_complete
                           && (analysis->bounded_out
                               || analysis->extraction_truncated)) {
                    /* Complete recovery classified and serialized the source
                       in one pass, but deliberately connected no root. Undo
                       the provisional body/path markers while preserving the
                       classified analysis for diagnostics and manual Reader. */
                    reader_rollback_markers(&journal);
                }
            }
            analysis->kind = kind;
            /* A bounded prefix can still produce a useful manual Reader tree,
               but an unseen suffix could change the page kind. Never use that
               partial conclusion for optional auto-engagement. */
            bool auto_article = kind != READER_PAGE_ARTICLE
                || (article < count
                    && reader_name_is(stats[article].node, "article")
                    && (stats[article].flags & READER_STAT_HEADING) != 0);
            analysis->high_confidence = kind != READER_PAGE_RAW
                && auto_article && !analysis->bounded_out;
            if (okay && install && connect_root && extracted_root != NULL
                && kind != READER_PAGE_RAW
                && !document_refresh(document)) {
                /* Refresh allocates its replacement metadata before changing
                   the committed counters/cache. If that allocation is
                   refused, restore the exact pre-Reader DOM while the marker
                   journal and extracted root are still owned and reachable. */
                if (extracted_root != NULL) {
                    reader_forget_generated_media_card(document);
                    if (extracted_root->parent != NULL)
                        lxb_dom_node_remove(extracted_root);
                    lxb_dom_node_destroy_deep(extracted_root);
                }
                reader_rollback_markers(&journal);
                okay = false;
            }
            if (okay && install && extracted_root != NULL
                && kind != READER_PAGE_RAW && prepared_root != NULL) {
                *prepared_root = extracted_root;
            }
        }
        if (!okay) reader_rollback_markers(&journal);
        document_allocation_owner_leave(document, previous);
        budget_free(document->budget, undo);
    }
    budget_free(document->budget, style_stack);
    budget_free(document->budget, stats);
    if (!okay) {
        *analysis = (ReaderDocumentAnalysis) {
            .prepared = true,
            .bounded_out = true,
            .visited_nodes = analysis->visited_nodes
        };
    }
    /* Reader analysis and its extracted tree are optional presentation work.
       Allocation refusal while refreshing the connected clone has already
       restored the exact raw DOM above; report that stable RAW result rather
       than aborting an otherwise valid navigation. */
    return true;
}

bool reader_document_prepare_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis)
{
    return reader_document_prepare_internal(
        document, stylesheet, analysis, true, false, true, NULL);
}

bool reader_document_prepare_detached_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis, lxb_dom_node_t **prepared_root)
{
    return reader_document_prepare_internal(
        document, stylesheet, analysis, true, false, false, prepared_root);
}

bool reader_document_analyze_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis)
{
    return reader_document_prepare_internal(
        document, stylesheet, analysis, false, false, false, NULL);
}

bool reader_document_prepare_complete_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis)
{
    if (analysis == NULL) return false;
    return reader_document_prepare_internal(
        document, stylesheet, analysis, true, true, true, NULL);
}

static bool reader_exact_root_matches(
    lxb_dom_node_t *root, ReaderPageKind kind)
{
    if (root == NULL || kind == READER_PAGE_RAW
        || root->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    size_t marker_length = 0;
    const char *marker = document_attribute(
        root, "data-tilefinch-reader-root", &marker_length);
    const char *expected = reader_page_kind_name(kind);
    size_t expected_length = strlen(expected);
    return marker != NULL && marker_length == expected_length
        && memcmp(marker, expected, expected_length) == 0;
}

bool reader_document_connect_prepared_view(
    PocDocument *document, lxb_dom_node_t *root, ReaderPageKind kind)
{
    if (document == NULL) return false;
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL || root == NULL
        || root->parent != NULL || !reader_exact_root_matches(root, kind)) {
        return false;
    }
    if (lxb_dom_node_append_child(body, root) != LXB_DOM_EXCEPTION_OK)
        return false;
    if (document_refresh(document)) return true;
    lxb_dom_node_remove(root);
    return false;
}

void reader_document_discard_exact_prepared_view(
    PocDocument *document, lxb_dom_node_t *root, ReaderPageKind kind)
{
    if (document == NULL || root == NULL
        || !reader_exact_root_matches(root, kind)) return;
    lxb_dom_node_t *body = document_body_node(document);
    bool connected = root->parent != NULL;
    if (connected) lxb_dom_node_remove(root);
    reader_forget_generated_media_card(document);
    lxb_dom_node_destroy_deep(root);
    if (body != NULL) {
        (void) lxb_dom_element_remove_attribute(
            lxb_dom_interface_element(body),
            (const lxb_char_t *) "data-tilefinch-reader-kind",
            sizeof("data-tilefinch-reader-kind") - 1u);
    }
    if (connected) (void) document_refresh(document);
}

static bool reader_document_prepare_basic_internal(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis, bool install, bool require_complete)
{
    (void) stylesheet;
    if (analysis == NULL) return false;
    *analysis = (ReaderDocumentAnalysis) { .prepared = true };
    if (document == NULL || document->budget == NULL)
        return true;
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL) return true;
    char *markup = budget_malloc_category(
        document->budget, BUDGET_CATEGORY_DOM, READER_EXTRACT_BYTE_LIMIT);
    if (markup == NULL) {
        analysis->bounded_out = true;
        return true;
    }
    ReaderExtractBuffer output = {
        .document = document,
        .data = markup,
        .capacity = READER_EXTRACT_BYTE_LIMIT,
        .content_limit = READER_EXTRACT_BYTE_LIMIT
            - READER_EXTRACT_TAIL_RESERVE,
        .next_cooperate_at = READER_TEXT_WORK_SLICE,
        .next_node_cooperate_at = READER_NODE_WORK_SLICE,
        .basic_mode = true
    };
    markup[0] = '\0';
    const MediaDeclaredVideo *declared_video =
        media_declared_video_cached(document);
    bool synthesize_declared_video = declared_video != NULL
        && !reader_document_has_video_element(document);
    bool okay = reader_extract_literal(
        &output, "<header><strong>Basic view</strong></header>");
    if (okay && synthesize_declared_video)
        okay = reader_extract_declared_video(&output, declared_video);
    bool meaningful = synthesize_declared_video;
    for (lxb_dom_node_t *child = body->first_child;
         okay && child != NULL && !output.truncated; child = child->next) {
        bool child_meaningful = false;
        okay = reader_extract_node(
            &output, child, 0u, false, &child_meaningful);
        meaningful = meaningful || child_meaningful;
    }
    if (okay && !require_complete
        && (output.truncated || output.bounded_out)) {
        output.tail_mode = true;
        okay = reader_extract_literal(
            &output,
            output.truncated
                ? "<p data-tilefinch-reader-truncated=\"1\">"
                  "Basic view shortened to fit this device.</p>"
                : "<p data-tilefinch-basic-bounded=\"1\">"
                  "Some controls or anchors were omitted to fit this device."
                  "</p>");
    }
    analysis->visited_nodes = output.visited_nodes > UINT32_MAX
        ? UINT32_MAX : (uint32_t) output.visited_nodes;
    analysis->visible_text_bytes = output.visible_text_bytes;
    analysis->extracted_bytes = output.length > UINT32_MAX
        ? UINT32_MAX : (uint32_t) output.length;
    analysis->extracted_nodes = output.nodes > UINT16_MAX
        ? UINT16_MAX : (uint16_t) output.nodes;
    analysis->retained_forms = output.retained_forms;
    analysis->mapped_anchors = output.mapped_anchors;
    analysis->bounded_out = output.bounded_out || output.truncated;
    analysis->extraction_truncated = output.truncated;
    if (meaningful) analysis->kind = READER_PAGE_BASIC;
    if (!okay || !meaningful
        || (require_complete
            && (output.truncated || output.bounded_out))) {
        budget_free(document->budget, markup);
        if (!okay) analysis->bounded_out = true;
        return okay;
    }
    if (!install) {
        budget_free(document->budget, markup);
        return true;
    }
    ReaderMarkerUndo undo[1];
    ReaderMutationJournal journal = {
        .items = undo,
        .capacity = sizeof(undo) / sizeof(undo[0])
    };
    lxb_dom_node_t *root = NULL;
    BudgetAllocationOwner previous =
        document_allocation_owner_enter(document);
    okay = reader_set_marker(
        &journal, body, "data-tilefinch-reader-kind", "basic");
    if (okay) okay = reader_commit_extracted_root(
        document, body, &output, READER_PAGE_BASIC, true, &root);
    if (okay && synthesize_declared_video) {
        lxb_dom_node_t *card = reader_find_generated_media_card(root);
        if (card == NULL) {
            okay = false;
        } else {
            document->reader_declared_video_card_node = card;
        }
    }
    if (okay && !document_refresh(document)) okay = false;
    if (!okay) {
        if (root != NULL) {
            reader_forget_generated_media_card(document);
            if (root->parent != NULL) lxb_dom_node_remove(root);
            lxb_dom_node_destroy_deep(root);
        }
        reader_rollback_markers(&journal);
    }
    document_allocation_owner_leave(document, previous);
    budget_free(document->budget, markup);
    if (!okay) {
        *analysis = (ReaderDocumentAnalysis) {
            .prepared = true,
            .bounded_out = true,
            .visited_nodes = analysis->visited_nodes
        };
    }
    return okay;
}

bool reader_document_prepare_basic_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis)
{
    return reader_document_prepare_basic_internal(
        document, stylesheet, analysis, true, false);
}

bool reader_document_prepare_basic_complete_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis)
{
    /* Extraction already builds into bounded temporary markup before any DOM
       mutation. Complete admission can therefore validate and commit that
       same buffer once, rather than repeating the entire source traversal. */
    return reader_document_prepare_basic_internal(
        document, stylesheet, analysis, true, true);
}

bool reader_document_discard_prepared_view(
    PocDocument *document, ReaderPageKind kind)
{
    if (document == NULL || kind == READER_PAGE_RAW) return false;
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *root = body == NULL ? NULL : body->last_child;
    size_t marker_length = 0;
    const char *marker = root == NULL ? NULL : document_attribute(
        root, "data-tilefinch-reader-root", &marker_length);
    const char *expected = reader_page_kind_name(kind);
    size_t expected_length = strlen(expected);
    if (root == NULL || root->type != LXB_DOM_NODE_TYPE_ELEMENT
        || marker == NULL || marker_length != expected_length
        || memcmp(marker, expected, expected_length) != 0) return false;
    lxb_dom_node_remove(root);
    reader_forget_generated_media_card(document);
    lxb_dom_node_destroy_deep(root);
    (void) lxb_dom_element_remove_attribute(
        lxb_dom_interface_element(body),
        (const lxb_char_t *) "data-tilefinch-reader-kind",
        sizeof("data-tilefinch-reader-kind") - 1u);
    /* document_refresh allocates replacement metadata before publishing it.
       The DOM and public marker are already restored even if that optional
       bookkeeping allocation is refused again. */
    return document_refresh(document);
}

bool reader_document_prepare(PocDocument *document,
                             ReaderDocumentAnalysis *analysis)
{
    return reader_document_prepare_with_stylesheet(document, NULL, analysis);
}

const char *reader_page_kind_name(ReaderPageKind kind)
{
    switch (kind) {
    case READER_PAGE_BASIC: return "basic";
    case READER_PAGE_ARTICLE: return "article";
    case READER_PAGE_LISTING: return "listing";
    case READER_PAGE_WATCH: return "watch";
    case READER_PAGE_RAW:
    default: return "raw";
    }
}
