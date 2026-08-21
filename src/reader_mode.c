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
#define READER_INVALID_INDEX UINT16_MAX

enum {
    READER_STAT_EXCLUDED = 1u << 0,
    READER_STAT_IMAGE = 1u << 1,
    READER_STAT_META = 1u << 2
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
    return reader_name_is(node, "head") || reader_name_is(node, "script")
        || reader_name_is(node, "style") || reader_name_is(node, "template")
        || reader_name_is(node, "noscript");
}

static bool reader_excluded_region(lxb_dom_node_t *node)
{
    return reader_name_is(node, "nav") || reader_name_is(node, "aside")
        || reader_name_is(node, "footer") || reader_name_is(node, "form")
        || reader_name_is(node, "menu");
}

static bool reader_primary_media_element(lxb_dom_node_t *node)
{
    if (reader_name_is(node, "video")) return true;
    size_t length = 0;
    const char *itemtype = document_attribute(node, "itemtype", &length);
    if (reader_slice_contains_ci(itemtype, length, "VideoObject")) return true;
    if (!reader_name_is(node, "meta")) return false;
    const char *property = document_attribute(node, "property", &length);
    if (property == NULL || length != 7u
        || strncasecmp(property, "og:type", 7u) != 0) return false;
    const char *content = document_attribute(node, "content", &length);
    return reader_slice_contains_ci(content, length, "video");
}

static bool reader_json_video_marker(lxb_dom_node_t *node,
                                     const char *text, size_t length)
{
    lxb_dom_node_t *parent = node == NULL ? NULL : node->parent;
    if (parent == NULL || !reader_name_is(parent, "script")) return false;
    size_t type_length = 0;
    const char *type = document_attribute(parent, "type", &type_length);
    return reader_slice_contains_ci(type, type_length, "ld+json")
        && reader_slice_contains_ci(text, length, "VideoObject");
}

static bool reader_head_primary_media(const PocDocument *document)
{
    if (document == NULL || document->html == NULL) return false;
    lxb_html_head_element_t *head =
        lxb_html_document_head_element(document->html);
    lxb_dom_node_t *root = head == NULL
        ? NULL : lxb_dom_interface_node(head);
    lxb_dom_node_t *boundary = root == NULL ? NULL : root->parent;
    lxb_dom_node_t *node = root;
    size_t visited = 0;
    while (node != NULL && node != boundary && visited++ < 256u) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && reader_primary_media_element(node)) return true;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (reader_json_video_marker(node, text, length)) return true;
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
    return false;
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
        for (size_t depth = 0; winner != READER_INVALID_INDEX && depth < 6u;
             depth++) {
            if (reader_name_is(stats[winner].node, "main")
                || reader_name_is(stats[winner].node, "article")) break;
            uint16_t parent = stats[winner].parent;
            if (parent == READER_INVALID_INDEX || parent == 0u) break;
            winner = parent;
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
    if (capacity > READER_NODE_LIMIT) {
        analysis->bounded_out = true;
        return true;
    }
    ReaderNodeStat *stats = budget_calloc_category(
        document->budget, BUDGET_CATEGORY_DOM, capacity, sizeof(*stats));
    if (stats == NULL) return true;
    uint16_t stack[READER_DEPTH_LIMIT];
    size_t depth = 0;
    size_t count = 0;
    size_t hidden_depth = 0;
    size_t excluded_depth = 0;
    size_t link_depth = 0;
    bool primary_media = reader_head_primary_media(document);
    uint16_t primary_media_index = READER_INVALID_INDEX;
    lxb_dom_node_t *node = body;
    lxb_dom_node_t *boundary = body->parent;
    size_t visited = 0;
    bool okay = true;
    while (node != NULL && node != boundary) {
        if (++visited > READER_NODE_LIMIT
            || ((visited & 255u) == 0
                && !tilefinch_platform_cooperate("reader-analyze", visited))) {
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
            if (reader_name_is(node, "p")) stat->paragraphs = 1u;
            stat->flags |= stat->own_flags;
            bool media_element = reader_primary_media_element(node);
            primary_media = primary_media || media_element;
            if (media_element && !reader_name_is(node, "meta")
                && primary_media_index == READER_INVALID_INDEX)
                primary_media_index = (uint16_t) count;
            stack[depth++] = (uint16_t) count++;
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT && depth != 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            primary_media = primary_media
                || reader_json_video_marker(node, text, length);
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
                & (READER_STAT_IMAGE | READER_STAT_META);
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
        if (okay && (!primary_media || article != READER_INVALID_INDEX))
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
            }
            analysis->kind = kind;
            analysis->high_confidence = kind != READER_PAGE_RAW;
        }
        if (!okay) reader_rollback_markers(&journal);
        document_allocation_owner_leave(document, previous);
        budget_free(document->budget, undo);
        if (okay && analysis->kind != READER_PAGE_RAW)
            document_note_connected_mutation(document);
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
