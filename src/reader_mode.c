#include "tilefinch/reader_mode.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/platform.h"

#include <lexbor/dom/interfaces/element.h>

#define READER_NODE_LIMIT 8192u
#define READER_DEPTH_LIMIT 128u
#define READER_ENTRY_LIMIT 64u
#define READER_PART_SCAN_LIMIT 256u
#define READER_MARKER_LIMIT 640u
#define READER_LABEL_LIMIT 192u
#define READER_EXTRACT_NODE_LIMIT 512u
#define READER_EXTRACT_DEPTH_LIMIT 64u
#define READER_EXTRACT_BYTE_LIMIT (256u * 1024u)
#define READER_INVALID_INDEX UINT16_MAX

enum {
    READER_STAT_EXCLUDED = 1u << 0,
    READER_STAT_IMAGE = 1u << 1,
    READER_STAT_META = 1u << 2,
    READER_STAT_HEADING = 1u << 3
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
} ReaderMutationJournal;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t nodes;
    bool truncated;
} ReaderExtractBuffer;

static bool reader_name_is(const lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *actual = document_element_name(
        (lxb_dom_node_t *) node, &length);
    size_t wanted = strlen(name);
    return actual != NULL && length == wanted
        && strncasecmp(actual, name, wanted) == 0;
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

static uint32_t reader_visible_text_bytes(const char *text, size_t length)
{
    uint32_t bytes = 0;
    bool pending_space = false;
    for (size_t i = 0; text != NULL && i < length; i++) {
        unsigned char value = (unsigned char) text[i];
        if (isspace(value)) {
            pending_space = bytes != 0;
        } else {
            if (pending_space && bytes != UINT32_MAX) bytes++;
            if (bytes != UINT32_MAX) bytes++;
            pending_space = false;
        }
    }
    return bytes;
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

static bool reader_hidden_element(lxb_dom_node_t *node)
{
    if (reader_name_is(node, "head") || reader_name_is(node, "script")
        || reader_name_is(node, "style") || reader_name_is(node, "template")
        || reader_name_is(node, "noscript")) return true;
    size_t length = 0;
    if (document_attribute(node, "hidden", &length) != NULL) return true;
    const char *aria_hidden = document_attribute(node, "aria-hidden", &length);
    return aria_hidden != NULL && length == 4u
        && strncasecmp(aria_hidden, "true", 4u) == 0;
}

static bool reader_excluded_region(lxb_dom_node_t *node)
{
    return reader_name_is(node, "nav") || reader_name_is(node, "aside")
        || reader_name_is(node, "footer") || reader_name_is(node, "form")
        || reader_name_is(node, "menu");
}

static bool reader_primary_media_element(lxb_dom_node_t *node)
{
    if (!reader_name_is(node, "video") && !reader_name_is(node, "audio"))
        return false;
    size_t length = 0;
    if (document_attribute(node, "src", &length) != NULL && length != 0)
        return true;
    if (document_attribute(node, "controls", &length) != NULL
        || document_attribute(node, "poster", &length) != NULL) return true;
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL; child = child->next) {
        if (!reader_name_is(child, "source")) continue;
        if (document_attribute(child, "src", &length) != NULL && length != 0)
            return true;
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
    if (output == NULL || text == NULL || output->truncated) return false;
    if (length > output->capacity - output->length - 1u) {
        output->truncated = true;
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
                                   bool attribute)
{
    for (size_t i = 0; i < length; i++) {
        const char *escape = NULL;
        switch ((unsigned char) text[i]) {
        case '&': escape = "&amp;"; break;
        case '<': escape = "&lt;"; break;
        case '>': escape = "&gt;"; break;
        case '"': if (attribute) escape = "&quot;"; break;
        default: break;
        }
        if (escape != NULL) {
            if (!reader_extract_literal(output, escape)) return false;
        } else if (!reader_extract_append(output, text + i, 1u)) {
            return false;
        }
    }
    return true;
}

static const char *reader_extract_tag(lxb_dom_node_t *node)
{
    static const char *const tags[] = {
        "article", "section", "main", "h1", "h2", "h3", "h4", "h5",
        "h6", "p", "ul", "ol", "li", "dl", "dt", "dd", "blockquote",
        "figure", "figcaption", "table", "caption", "thead", "tbody",
        "tfoot", "tr", "th",
        "td", "pre", "code", "kbd", "samp", "a", "img", "video",
        "audio", "source", "br", "hr", "strong", "em", "b", "i"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (reader_name_is(node, tags[i])) return tags[i];
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
        && reader_extract_escaped(output, value, length, true)
        && reader_extract_append(output, "\"", 1u);
}

static bool reader_extract_node(ReaderExtractBuffer *output,
                                lxb_dom_node_t *node, size_t depth)
{
    if (output == NULL || node == NULL || depth > READER_EXTRACT_DEPTH_LIMIT)
        return false;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        return text == NULL || reader_extract_escaped(
            output, text, length, false);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || reader_hidden_element(node) || reader_excluded_region(node))
        return true;
    if (++output->nodes > READER_EXTRACT_NODE_LIMIT) {
        /* A large article remains useful as a bounded leading extraction.
           Skip whole later subtrees so the emitted HTML remains balanced. */
        return true;
    }
    const char *tag = reader_extract_tag(node);
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
    if (emitted) {
        if (!reader_extract_append(output, "<", 1u)
            || !reader_extract_literal(output, tag)) return false;
        if (strcmp(tag, "a") == 0) {
            if (!reader_extract_attribute(output, node, "href")
                || !reader_extract_attribute(output, node, "title"))
                return false;
        } else if (strcmp(tag, "img") == 0) {
            if (!reader_extract_literal(output, " src=\"")
                    || !reader_extract_escaped(
                        output, image_source, image_source_length, true)
                    || !reader_extract_append(output, "\"", 1u)) return false;
            if (!reader_extract_attribute(output, node, "alt")
                || !reader_extract_attribute(output, node, "width")
                || !reader_extract_attribute(output, node, "height"))
                return false;
        } else if (strcmp(tag, "video") == 0
                   || strcmp(tag, "audio") == 0
                   || strcmp(tag, "source") == 0) {
            if (!reader_extract_attribute(output, node, "src")
                || !reader_extract_attribute(output, node, "poster")
                || !reader_extract_attribute(output, node, "controls")
                || !reader_extract_attribute(output, node, "type"))
                return false;
        } else if (strcmp(tag, "th") == 0 || strcmp(tag, "td") == 0) {
            if (!reader_extract_attribute(output, node, "colspan")
                || !reader_extract_attribute(output, node, "rowspan"))
                return false;
        }
        if (!reader_extract_append(output, ">", 1u)) return false;
    }
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL; child = child->next)
        if (!reader_extract_node(output, child, depth + 1u)) return false;
    if (emitted && strcmp(tag, "img") != 0 && strcmp(tag, "source") != 0
        && strcmp(tag, "br") != 0 && strcmp(tag, "hr") != 0) {
        if (!reader_extract_literal(output, "</")
            || !reader_extract_literal(output, tag)
            || !reader_extract_append(output, ">", 1u)) return false;
    }
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
    size_t existing_length = 0;
    if (document_attribute(node, name, &existing_length) != NULL
        || journal->count == journal->capacity) return false;
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
                                   ReaderMutationJournal *journal)
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
    bool dominant = selected->text_bytes >= 600u
        && selected->paragraphs >= 3u
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

static bool reader_install_extracted_tree(
    PocDocument *document, lxb_dom_node_t *body,
    const ReaderNodeStat *stats, size_t count,
    ReaderPageKind kind, uint16_t article_root, uint16_t listing_root)
{
    if (document == NULL || document->budget == NULL || document->html == NULL
        || body == NULL || stats == NULL || count == 0
        || kind == READER_PAGE_RAW) return false;
    char *markup = budget_malloc_category(
        document->budget, BUDGET_CATEGORY_DOM, READER_EXTRACT_BYTE_LIMIT);
    if (markup == NULL) return false;
    ReaderExtractBuffer output = {
        .data = markup,
        .capacity = READER_EXTRACT_BYTE_LIMIT
    };
    markup[0] = '\0';
    bool okay = reader_extract_literal(
        &output, "<header><strong>Reader</strong></header>");
    if (okay && article_root < count)
        okay = reader_extract_node(&output, stats[article_root].node, 0u);
    if (okay && listing_root < count) {
        if (kind == READER_PAGE_WATCH)
            okay = reader_extract_literal(&output, "<h2>More</h2>");
        if (okay)
            okay = reader_extract_node(&output, stats[listing_root].node, 0u);
    }
    static const lxb_char_t root_name[] = "main";
    lxb_dom_element_t *element = okay
        ? lxb_dom_document_create_element(
              &document->html->dom_document, root_name,
              sizeof(root_name) - 1u, NULL)
        : NULL;
    lxb_dom_node_t *root = element == NULL
        ? NULL : lxb_dom_interface_node(element);
    const char *kind_name = reader_page_kind_name(kind);
    okay = root != NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "data-tilefinch-reader-root",
               sizeof("data-tilefinch-reader-root") - 1u,
               (const lxb_char_t *) kind_name, strlen(kind_name)) != NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "hidden", 6u,
               (const lxb_char_t *) "hidden", 6u) != NULL
        && document_set_element_inner_html(
               document, root, output.data, output.length)
        && lxb_dom_node_append_child(body, root) == LXB_DOM_EXCEPTION_OK;
    if (!okay && root != NULL) {
        if (root->parent != NULL) lxb_dom_node_remove(root);
        lxb_dom_node_destroy_deep(root);
    }
    budget_free(document->budget, markup);
    return okay && !output.truncated;
}

bool reader_document_prepare(PocDocument *document,
                             ReaderDocumentAnalysis *analysis)
{
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
    size_t depth = 0;
    size_t count = 0;
    size_t hidden_depth = 0;
    size_t excluded_depth = 0;
    size_t link_depth = 0;
    bool primary_media = false;
    bool head_media_hint = reader_document_head_media_hint(document);
    uint16_t primary_media_index = READER_INVALID_INDEX;
    lxb_dom_node_t *node = body;
    lxb_dom_node_t *boundary = body->parent;
    size_t visited = 0;
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
            bool excluded = reader_excluded_region(node);
            if (hidden) hidden_depth++;
            if (excluded) excluded_depth++;
            if (reader_name_is(node, "a")) link_depth++;
            ReaderNodeStat *stat = &stats[count];
            stat->node = node;
            stat->parent = depth == 0 ? READER_INVALID_INDEX
                : stack[depth - 1u];
            if (hidden_depth != 0 || excluded_depth != 0)
                stat->flags |= READER_STAT_EXCLUDED;
            if (reader_name_is(node, "img"))
                stat->own_flags |= READER_STAT_IMAGE;
            if (reader_name_is(node, "h1") || reader_name_is(node, "h2")
                || reader_name_is(node, "h3"))
                stat->own_flags |= READER_STAT_HEADING;
            if (reader_name_is(node, "p")) stat->paragraphs = 1u;
            stat->flags |= stat->own_flags;
            bool media_element = hidden_depth == 0 && excluded_depth == 0
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
            if (hidden_depth == 0) {
                uint32_t bytes = reader_visible_text_bytes(text, length);
                ReaderNodeStat *stat = &stats[stack[depth - 1u]];
                stat->text_bytes = reader_add_u32(stat->text_bytes, bytes);
                if (link_depth != 0)
                    stat->link_bytes = reader_add_u32(
                        stat->link_bytes, bytes);
                if (reader_text_has_meta(text, length)) {
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
                if (reader_name_is(node, "a")) link_depth--;
                if (reader_excluded_region(node)) excluded_depth--;
                if (reader_hidden_element(node)) hidden_depth--;
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
            .capacity = marker_capacity
        };
        if (marker_capacity != 0 && undo == NULL) okay = false;
        BudgetAllocationOwner previous =
            document_allocation_owner_enter(document);
        if (okay && primary_media) okay = reader_prepare_watch(
            stats, count, primary_media_index, body, &article, &journal);
        /* A watch page's related rail is secondary only when a primary
           media/title subtree was actually preserved. With a head-only media
           hint and no trustworthy root, leave the page conservatively raw
           rather than exposing only the recommendations. */
        if (okay && (!primary_media || article != READER_INVALID_INDEX)
            && !(head_media_hint && !primary_media))
            okay = reader_prepare_listing(
                stats, count, entries, entry_count, body, analysis, &listing,
                &journal);
        if (okay && !primary_media) okay = reader_prepare_article(
            stats, count, body, analysis, &article, &journal);
        if (okay) {
            ReaderPageKind kind = primary_media
                    && article != READER_INVALID_INDEX ? READER_PAGE_WATCH
                : analysis->listing_entries >= 8u ? READER_PAGE_LISTING
                : article != READER_INVALID_INDEX
                    ? READER_PAGE_ARTICLE : READER_PAGE_RAW;
            if (kind != READER_PAGE_RAW) {
                const char *kind_name = reader_page_kind_name(kind);
                okay = reader_set_marker(
                    &journal, body, "data-tilefinch-reader-kind", kind_name);
                if (okay) okay = reader_install_extracted_tree(
                    document, body, stats, count, kind, article, listing);
            }
            analysis->kind = kind;
            /* A bounded prefix can still produce a useful manual Reader tree,
               but an unseen suffix could change the page kind. Never use that
               partial conclusion for optional auto-engagement. */
            analysis->high_confidence = kind != READER_PAGE_RAW
                && !analysis->bounded_out;
        }
        if (!okay) reader_rollback_markers(&journal);
        document_allocation_owner_leave(document, previous);
        budget_free(document->budget, undo);
        if (okay && analysis->kind != READER_PAGE_RAW) {
            (void) document_refresh(document);
            document_note_connected_mutation(document);
        }
    }
    budget_free(document->budget, stats);
    if (!okay) {
        *analysis = (ReaderDocumentAnalysis) {
            .prepared = true,
            .bounded_out = true,
            .visited_nodes = analysis->visited_nodes
        };
    }
    return true;
}

const char *reader_page_kind_name(ReaderPageKind kind)
{
    switch (kind) {
    case READER_PAGE_ARTICLE: return "article";
    case READER_PAGE_LISTING: return "listing";
    case READER_PAGE_WATCH: return "watch";
    case READER_PAGE_RAW:
    default: return "raw";
    }
}
