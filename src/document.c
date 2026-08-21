#include "tilefinch/document.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/tree.h>
#include <lexbor/ns/const.h>

#include "tilefinch/platform.h"
#include "tilefinch/font.h"
#include "tilefinch/url.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_DOM, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_DOM, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_DOM, (p), (s))

/* A malformed or author-generated tree must not turn maintenance passes into
   unbounded work.  The walkers below use parent/sibling links rather than the
   C call stack and fail their public operation if this ceiling is reached. */
#define DOCUMENT_TRAVERSAL_NODE_LIMIT 65536u
/* A whole-document parse is a single call, and a site adapter makes it the
   first thing a navigation pump does once the adapter hands its document
   over.  Every later stage of that commit -- statistics, style, resources,
   images, layout -- already carries both a work bound and a cooperate
   checkpoint; without one here the pump owns the CPU for the whole parse,
   which is long enough on the device that cancel cannot be sampled and the
   supervisor cannot paint.  The checkpoint is taken per window of fed bytes
   rather than per chunk, so a document smaller than one window costs
   nothing. */
#define DOCUMENT_PARSE_COOPERATE_BYTES (8u * 1024u)
#define DOCUMENT_CONTROL_STATE_LIMIT 128u
#define DOCUMENT_CONTROL_VALUE_LIMIT 512u
#define DOCUMENT_CONTROL_STATE_MAGIC UINT32_C(0x4354524c)

static const char document_cssom_style_marker;

bool document_style_attribute_cssom_authorized(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_attr_t *attribute = lxb_dom_element_attr_by_name(
        lxb_dom_interface_element(node), (const lxb_char_t *) "style", 5);
    return attribute != NULL
        && attribute->node.user == &document_cssom_style_marker;
}

void document_style_attribute_set_cssom_authorized(lxb_dom_node_t *node,
                                                    bool authorized)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    lxb_dom_attr_t *attribute = lxb_dom_element_attr_by_name(
        lxb_dom_interface_element(node), (const lxb_char_t *) "style", 5);
    if (attribute == NULL) return;
    if (authorized) {
        attribute->node.user = (void *) &document_cssom_style_marker;
    } else if (attribute->node.user == &document_cssom_style_marker) {
        attribute->node.user = NULL;
    }
}

struct DocumentControlState {
    uint32_t magic;
    lxb_dom_node_t *node;
    lxb_dom_node_t *parser_form_owner;
    char *value;
    size_t length;
    int resized_width;
    int resized_height;
    struct DocumentControlState *next;
};

static bool document_parser_form_associated_tag(lxb_tag_id_t tag_id)
{
    return tag_id == LXB_TAG_BUTTON || tag_id == LXB_TAG_FIELDSET
        || tag_id == LXB_TAG_INPUT || tag_id == LXB_TAG_OBJECT
        || tag_id == LXB_TAG_OUTPUT || tag_id == LXB_TAG_SELECT
        || tag_id == LXB_TAG_TEXTAREA;
}

static bool document_node_has_tag(lxb_dom_node_t *node,
                                  lxb_tag_id_t tag_id)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && node->ns == LXB_NS_HTML && node->local_name == tag_id;
}

static DocumentControlState *document_control_state_ensure(
    PocDocument *document, lxb_dom_node_t *node)
{
    if (document == NULL || document->budget == NULL || node == NULL
        || node->owner_document != &document->html->dom_document) return NULL;
    DocumentControlState *state = node->user;
    if (state != NULL) {
        return state->magic == DOCUMENT_CONTROL_STATE_MAGIC
                    && state->node == node
            ? state : NULL;
    }
    if (document->control_state_count >= DOCUMENT_CONTROL_STATE_LIMIT) {
        return NULL;
    }
    state = budget_calloc(document->budget, 1, sizeof(*state));
    if (state == NULL) return NULL;
    state->magic = DOCUMENT_CONTROL_STATE_MAGIC;
    state->node = node;
    state->next = document->control_states;
    document->control_states = state;
    document->control_state_count++;
    node->user = state;
    return state;
}

static bool document_control_parser_form_set(
    PocDocument *document, lxb_dom_node_t *node, lxb_dom_node_t *form)
{
    DocumentControlState *state =
        document_control_state_ensure(document, node);
    if (state == NULL) return false;
    if (state->parser_form_owner == NULL && form != NULL) {
        document->parser_form_owner_count++;
    }
    state->parser_form_owner = form;
    return true;
}

static lxb_html_token_t *document_parser_token(
    lxb_html_tokenizer_t *tokenizer, lxb_html_token_t *token, void *opaque)
{
    DocumentParser *parser = opaque;
    lxb_html_tree_t *tree = parser->original_token_context;
    bool record_form_owner = tree != NULL && tree->form != NULL
        && (token->type & LXB_HTML_TOKEN_TYPE_CLOSE) == 0
        && document_parser_form_associated_tag(token->tag_id);
    lxb_dom_node_t *current_before = !record_form_owner
        ? NULL : lxb_html_tree_current_node(tree);
    lxb_dom_node_t *last_child_before = current_before == NULL
        ? NULL : current_before->last_child;
    lxb_dom_node_t *parser_form = !record_form_owner
        ? NULL : lxb_dom_interface_node(tree->form);
    lxb_dom_node_t *closed = NULL;
    lxb_dom_node_t *self_closing_parent = NULL;
    if ((token->type & LXB_HTML_TOKEN_TYPE_CLOSE) != 0
        && tree != NULL) {
        lxb_dom_node_t *current = lxb_html_tree_current_node(tree);
        if (current != NULL && current->local_name == token->tag_id) {
            closed = current;
        }
    } else if ((token->type & LXB_HTML_TOKEN_TYPE_CLOSE_SELF) != 0
               && tree != NULL) {
        lxb_dom_node_t *current = lxb_html_tree_current_node(tree);
        /* In the HTML namespace the self-closing flag on non-void elements
           is ignored. In foreign content it closes the element immediately,
           so it is also a parser-script execution checkpoint. */
        if (current != NULL && current->ns == LXB_NS_SVG) {
            self_closing_parent = current;
        }
    }
    lxb_html_token_t *processed = parser->original_token_callback(
        tokenizer, token, parser->original_token_context);
    if (processed != NULL && parser_form != NULL) {
        lxb_dom_node_t *current_after = lxb_html_tree_current_node(tree);
        lxb_dom_node_t *candidate =
            document_node_has_tag(current_after, token->tag_id)
                ? current_after : NULL;
        if (candidate == NULL && current_after != NULL
            && current_after->last_child != last_child_before
            && document_node_has_tag(
                   current_after->last_child, token->tag_id)) {
            candidate = current_after->last_child;
        }
        if (candidate == NULL && current_before != NULL
            && current_before->last_child != last_child_before
            && document_node_has_tag(
                   current_before->last_child, token->tag_id)) {
            candidate = current_before->last_child;
        }
        bool ordinary_ancestor = false;
        lxb_dom_node_t *ancestor = candidate == NULL
            ? NULL : candidate->parent;
        for (size_t depth = 0; ancestor != NULL && depth < 256;
             depth++, ancestor = ancestor->parent) {
            if (ancestor != parser_form) continue;
            ordinary_ancestor = true;
            break;
        }
        if (candidate != NULL && !ordinary_ancestor
            && !document_control_parser_form_set(
                   &parser->document, candidate, parser_form)) {
            tokenizer->status = LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            return NULL;
        }
    }
    if (processed != NULL && closed == NULL
        && self_closing_parent != NULL) {
        lxb_dom_node_t *candidate = self_closing_parent->last_child;
        if (candidate != NULL && candidate->ns == LXB_NS_SVG
            && candidate->local_name == token->tag_id) {
            closed = candidate;
        }
    }
    if (processed != NULL && closed != NULL
        && parser->element_closed != NULL
        && !parser->element_closed(parser->element_closed_opaque,
                                   &parser->document, closed)) {
        /* DOM changes made by a parser-blocking script belong to the
           candidate too. Higher-level non-DOM consumers are torn down by the
           caller before the owner-scoped DOM rollback. */
        tokenizer->status = LXB_STATUS_ABORTED;
        return NULL;
    }
    return processed;
}

typedef struct {
    size_t nodes;
    size_t elements;
    size_t text_nodes;
    size_t attributes;
    size_t attribute_value_bytes;
    size_t body_bytes;
    size_t body_text_nodes;
    uint8_t glyph_script_mask;
    bool pointer_event_attributes_present;
} DocumentStats;

static uint8_t document_codepoint_glyph_script(unsigned codepoint)
{
    if ((codepoint >= 0x3400u && codepoint <= 0x4dbfu)
        || (codepoint >= 0x4e00u && codepoint <= 0x9fffu)
        || (codepoint >= 0xf900u && codepoint <= 0xfaffu)
        || (codepoint >= 0x20000u && codepoint <= 0x323afu)) {
        return DOCUMENT_GLYPH_SCRIPT_HAN;
    }
    if ((codepoint >= 0x3040u && codepoint <= 0x30ffu)
        || (codepoint >= 0x31f0u && codepoint <= 0x31ffu)
        || (codepoint >= 0xff66u && codepoint <= 0xff9du)) {
        return DOCUMENT_GLYPH_SCRIPT_JAPANESE;
    }
    if ((codepoint >= 0x1100u && codepoint <= 0x11ffu)
        || (codepoint >= 0x3130u && codepoint <= 0x318fu)
        || (codepoint >= 0xa960u && codepoint <= 0xa97fu)
        || (codepoint >= 0xac00u && codepoint <= 0xd7afu)
        || (codepoint >= 0xd7b0u && codepoint <= 0xd7ffu)) {
        return DOCUMENT_GLYPH_SCRIPT_KOREAN;
    }
    if ((codepoint >= 0x0400u && codepoint <= 0x052fu)
        || (codepoint >= 0x1c80u && codepoint <= 0x1c8fu)
        || (codepoint >= 0x2de0u && codepoint <= 0x2dffu)
        || (codepoint >= 0xa640u && codepoint <= 0xa69fu)) {
        return DOCUMENT_GLYPH_SCRIPT_CYRILLIC;
    }
    if ((codepoint >= 0x0100u && codepoint <= 0x024fu)
        || (codepoint >= 0x0300u && codepoint <= 0x036fu)
        || (codepoint >= 0x1e00u && codepoint <= 0x1effu)
        || (codepoint >= 0x2c60u && codepoint <= 0x2c7fu)
        || (codepoint >= 0xa720u && codepoint <= 0xa7ffu)
        || (codepoint >= 0xab30u && codepoint <= 0xab6fu)) {
        return DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED;
    }
    return 0;
}

static void document_note_glyph_scripts(DocumentStats *stats,
                                        const char *text, size_t length)
{
    if (stats == NULL || text == NULL || length == 0) return;
    const uint8_t all = DOCUMENT_GLYPH_SCRIPT_HAN
        | DOCUMENT_GLYPH_SCRIPT_JAPANESE | DOCUMENT_GLYPH_SCRIPT_KOREAN
        | DOCUMENT_GLYPH_SCRIPT_CYRILLIC
        | DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED;
    size_t at = 0;
    while (at < length && stats->glyph_script_mask != all) {
        /* Most top-site text is ASCII. Skip it without calling the UTF-8
           decoder; this pass runs only once in the parser's existing visible
           text/statistics walk. */
        while (at < length && (unsigned char) text[at] < 0x80u) at++;
        if (at >= length) break;
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) {
            at++;
            continue;
        }
        stats->glyph_script_mask |=
            document_codepoint_glyph_script(codepoint);
        at += used;
    }
}

static bool pointer_event_attribute_name(
    const lxb_char_t *name, size_t length)
{
    return name != NULL
        && ((length >= 7u && memcmp(name, "onmouse", 7u) == 0)
            || (length >= 9u
                && memcmp(name, "onpointer", 9u) == 0));
}

static bool name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static bool document_stats_add(size_t *value, size_t amount)
{
    if (value == NULL || amount > SIZE_MAX - *value) return false;
    *value += amount;
    return true;
}

static bool gather_stats(lxb_dom_node_t *node, bool in_body, bool hidden,
                         DocumentStats *stats)
{
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t body_depth = in_body ? 1u : 0u;
    size_t hidden_depth = hidden ? 1u : 0u;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if ((visited & 255u) == 0
            && !tilefinch_platform_cooperate("document-stats", visited)) {
            return false;
        }
        if (!document_stats_add(&stats->nodes, 1)) return false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (!document_stats_add(&stats->elements, 1)) return false;
            lxb_dom_element_t *element = lxb_dom_interface_element(node);
            for (lxb_dom_attr_t *attr = element->first_attr;
                 attr != NULL; attr = attr->next) {
                size_t name_length = 0;
                const lxb_char_t *attribute_name =
                    lxb_dom_attr_qualified_name(attr, &name_length);
                if (pointer_event_attribute_name(
                        attribute_name, name_length)) {
                    stats->pointer_event_attributes_present = true;
                }
                if (!document_stats_add(&stats->attributes, 1)
                    || (attr->value != NULL
                        && !document_stats_add(
                            &stats->attribute_value_bytes,
                            attr->value->length))) return false;
            }
            if (name_is(node, "body")) body_depth++;
            if (name_is(node, "script") || name_is(node, "style")
                || name_is(node, "head")) hidden_depth++;
        }
        if (node->type == LXB_DOM_NODE_TYPE_TEXT
            && !document_stats_add(&stats->text_nodes, 1)) return false;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT
            && body_depth != 0 && hidden_depth == 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            document_note_glyph_scripts(stats, text, length);
            if (!document_stats_add(&stats->body_bytes, length)
                || !document_stats_add(&stats->body_text_nodes, 1)) {
                return false;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                if (name_is(node, "body")) body_depth--;
                if (name_is(node, "script") || name_is(node, "style")
                    || name_is(node, "head")) hidden_depth--;
            }
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary_parent) return true;
        }
    }
    return true;
}

static bool copy_body_text(lxb_dom_node_t *node, bool hidden, char *output,
                           size_t capacity, size_t *offset)
{
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t hidden_depth = hidden ? 1u : 0u;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if ((visited & 255u) == 0
            && !tilefinch_platform_cooperate("document-text", visited)) {
            return false;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (name_is(node, "script") || name_is(node, "style")
                || name_is(node, "head")) hidden_depth++;
        }
        if (node->type == LXB_DOM_NODE_TYPE_TEXT && hidden_depth == 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (text != NULL && length != 0) {
                /* Leave room for this node's separator and the final NUL.
                   Stats are normally refreshed after every DOM mutation,
                   but this bound keeps materialization safe if a caller's
                   metadata is stale. */
                if (*offset > capacity || capacity - *offset < 2
                    || length > capacity - *offset - 2) return false;
                memcpy(output + *offset, text, length);
                *offset += length;
                output[(*offset)++] = ' ';
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                && (name_is(node, "script") || name_is(node, "style")
                    || name_is(node, "head"))) hidden_depth--;
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary_parent) return true;
        }
    }
    return true;
}

void document_parser_abort(DocumentParser *parser)
{
    if (parser == NULL) return;
    BudgetAllocationOwner allocation_owner =
        parser->document.allocation_owner;
    /* Lexbor can retain inconsistent internal pointers after an OOM. The
       parser transaction owns no external resources, so reclaim its complete
       allocation generation without calling back through that partial state. */
    memset(&parser->document, 0, sizeof(parser->document));
    if (parser->budget != NULL) {
        budget_rollback_owner_category(parser->budget,
                                       allocation_owner,
                                       BUDGET_CATEGORY_DOM);
    }
    memset(parser, 0, sizeof(*parser));
}

bool document_parser_begin(DocumentParser *parser, Budget *budget)
{
    if (parser == NULL) return false;
    memset(parser, 0, sizeof(*parser));
    if (budget == NULL || !budget_lexbor_is_installed(budget)) return false;
    parser->budget = budget;
    if (!budget_allocation_owner_create(
            budget, &parser->document.allocation_owner)) {
        memset(parser, 0, sizeof(*parser));
        return false;
    }
    parser->document.budget = budget;
    BudgetAllocationOwner previous = budget_allocation_owner_enter(
        budget, parser->document.allocation_owner);
    parser->document.html = lxb_html_document_create();
    bool began = parser->document.html != NULL
        && lxb_html_document_parse_chunk_begin(parser->document.html)
               == LXB_STATUS_OK;
    budget_allocation_owner_leave(budget, previous);
    if (!began) {
        document_parser_abort(parser);
        return false;
    }
    lxb_dom_document_t *dom = lxb_dom_interface_document(
        parser->document.html);
    lxb_html_parser_t *html_parser = dom->parser;
    if (html_parser == NULL || html_parser->tkz == NULL) {
        document_parser_abort(parser);
        return false;
    }
    parser->tokenizer = html_parser->tkz;
    parser->original_token_callback = parser->tokenizer->callback_token_done;
    parser->original_token_context = parser->tokenizer->callback_token_ctx;
    lxb_html_tokenizer_callback_token_done_set(
        parser->tokenizer, document_parser_token, parser);
    parser->active = true;
    return true;
}

bool document_parser_set_scripting(DocumentParser *parser, bool enabled)
{
    if (parser == NULL || !parser->active || parser->bytes_fed != 0
        || parser->document.html == NULL) return false;
    lxb_dom_document_t *dom = lxb_dom_interface_document(
        parser->document.html);
    lxb_html_parser_t *html_parser = dom == NULL ? NULL : dom->parser;
    if (html_parser == NULL) return false;
    lxb_html_parser_scripting_set(html_parser, enabled);
    lxb_html_document_scripting_set(parser->document.html, enabled);
    return true;
}

BudgetAllocationOwner document_allocation_owner_enter(
    const PocDocument *document)
{
    if (document == NULL || document->budget == NULL) {
        return BUDGET_ALLOCATION_OWNER_NONE;
    }
    return budget_allocation_owner_enter(document->budget,
                                          document->allocation_owner);
}

void document_allocation_owner_leave(const PocDocument *document,
                                     BudgetAllocationOwner previous_owner)
{
    if (document == NULL || document->budget == NULL) return;
    budget_allocation_owner_leave(document->budget, previous_owner);
}

void document_parser_set_element_closed_callback(
    DocumentParser *parser, DocumentElementClosedCallback callback,
    void *opaque)
{
    if (parser == NULL || !parser->active) return;
    parser->element_closed = callback;
    parser->element_closed_opaque = opaque;
}

bool document_parser_feed(DocumentParser *parser, const char *data,
                          size_t length)
{
    if (parser == NULL || !parser->active || parser->failed
        || (data == NULL && length != 0)
        || length > SIZE_MAX - parser->bytes_fed) return false;
    if (length == 0) return true;
    /* A transport may deliver one very large chunk. Lexbor does not yield
       inside its chunk entry point, so feed bounded windows while preserving
       tokenizer continuity and sample cancellation between windows. */
    size_t offset = 0;
    while (offset < length) {
        size_t window = length - offset;
        if (window > DOCUMENT_PARSE_COOPERATE_BYTES) {
            window = DOCUMENT_PARSE_COOPERATE_BYTES;
        }
        BudgetAllocationOwner previous = budget_allocation_owner_enter(
            parser->budget, parser->document.allocation_owner);
        lxb_status_t status = lxb_html_document_parse_chunk(
            parser->document.html,
            (const lxb_char_t *) data + offset, window);
        budget_allocation_owner_leave(parser->budget, previous);
        if (status != LXB_STATUS_OK) {
            /* The caller owns callback-created consumers. Leave the
               transaction intact so it can destroy those consumers before
               the O(1) parser rollback. */
            parser->failed = true;
            return false;
        }
        offset += window;
        parser->bytes_fed += window;
        if (offset < length
            && !tilefinch_platform_cooperate(
                "document-parse", parser->bytes_fed)) {
            parser->failed = true;
            return false;
        }
    }
    parser->chunks_fed++;
    return true;
}

bool document_parser_finish(DocumentParser *parser, PocDocument *document)
{
    if (parser == NULL || document == NULL || !parser->active
        || parser->failed) return false;
    BudgetAllocationOwner previous = budget_allocation_owner_enter(
        parser->budget, parser->document.allocation_owner);
    lxb_status_t status = lxb_html_document_parse_chunk_end(
        parser->document.html);
    if (parser->tokenizer != NULL
        && parser->original_token_callback != NULL) {
        lxb_html_tokenizer_callback_token_done_set(
            parser->tokenizer, parser->original_token_callback,
            parser->original_token_context);
    }
    bool refreshed = status == LXB_STATUS_OK
        && document_refresh(&parser->document);
    budget_allocation_owner_leave(parser->budget, previous);
    if (!refreshed) {
        parser->failed = true;
        return false;
    }
    parser->active = false;
    *document = parser->document;
    memset(parser, 0, sizeof(*parser));
    return true;
}

bool document_parse(PocDocument *document, Budget *budget,
                    const char *html, size_t html_length, size_t chunk_size)
{
    if (document == NULL || budget == NULL || html == NULL) {
        return false;
    }
    memset(document, 0, sizeof(*document));
    if (chunk_size == 0) return false;
    DocumentParser parser;
    if (!document_parser_begin(&parser, budget)) return false;

    size_t cooperated = 0;
    for (size_t offset = 0; offset < html_length; offset += chunk_size) {
        size_t length = html_length - offset;
        if (length > chunk_size) {
            length = chunk_size;
        }
        if (!document_parser_feed(&parser, html + offset, length)) {
            document_parser_abort(&parser);
            return false;
        }
        size_t fed = offset + length;
        if (fed - cooperated < DOCUMENT_PARSE_COOPERATE_BYTES) continue;
        cooperated = fed;
        if (!tilefinch_platform_cooperate("document-parse", fed)) {
            document_parser_abort(&parser);
            return false;
        }
    }
    if (!document_parser_finish(&parser, document)) {
        document_parser_abort(&parser);
        return false;
    }
    return true;
}

bool document_refresh(PocDocument *document)
{
    if (document == NULL || document->html == NULL
        || document->budget == NULL) return false;
    BudgetAllocationOwner previous =
        document_allocation_owner_enter(document);
    DocumentStats stats = {0};
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    if (!gather_stats(root, false, false, &stats)) {
        document_allocation_owner_leave(document, previous);
        return false;
    }
    size_t title_length = 0;
    const lxb_char_t *title = lxb_html_document_title(document->html,
                                                      &title_length);
    if (title_length == SIZE_MAX) {
        document_allocation_owner_leave(document, previous);
        return false;
    }
    char *new_title = budget_malloc(document->budget, title_length + 1);
    if (new_title == NULL) {
        document_allocation_owner_leave(document, previous);
        return false;
    }
    if (title != NULL && title_length != 0) {
        memcpy(new_title, title, title_length);
    }
    new_title[title_length] = '\0';
    budget_free(document->budget, document->title);
    budget_free(document->budget, document->body_text);
    document->title = new_title;
    document->body_text = NULL;
    document->node_count = stats.nodes;
    document->element_count = stats.elements;
    document->text_node_count = stats.text_nodes;
    document->attribute_count = stats.attributes;
    document->attribute_value_bytes = stats.attribute_value_bytes;
    document->text_bytes = stats.body_bytes;
    document->body_text_node_count = stats.body_text_nodes;
    document->body_text_length = 0;
    document->glyph_script_mask = stats.glyph_script_mask;
    document->pointer_event_attributes_present =
        stats.pointer_event_attributes_present;
    document_note_connected_mutation(document);
    document_allocation_owner_leave(document, previous);
    return true;
}

static bool document_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    size_t wanted_length = wanted == NULL ? 0 : strlen(wanted);
    return name != NULL && length == wanted_length
        && strncasecmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *document_bounded_next(lxb_dom_node_t *node,
                                             lxb_dom_node_t *root,
                                             bool skip_children)
{
    if (node == NULL || root == NULL) return NULL;
    if (!skip_children && node->first_child != NULL) return node->first_child;
    while (node != root && node->next == NULL) node = node->parent;
    return node == root ? NULL : node->next;
}

static bool document_loading_only_body(lxb_dom_node_t *body)
{
    if (body == NULL) return false;
    char visible[80] = {0};
    size_t used = 0;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        bool skip = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "script")
                || document_name_is(node, "style")
                || document_name_is(node, "template")
                || document_name_is(node, "svg"));
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t at = 0; text != NULL && at < length; at++) {
                unsigned char character = (unsigned char) text[at];
                if (isspace(character)) character = ' ';
                if (character == ' ' && (used == 0 || visible[used - 1] == ' '))
                    continue;
                if (used + 1u >= sizeof(visible)) return false;
                visible[used++] = (char) tolower(character);
            }
        }
        node = document_bounded_next(node, body, skip);
    }
    while (used > 0 && visible[used - 1] == ' ') used--;
    visible[used] = '\0';
    return used > 0 && strstr(visible, "loading") != NULL;
}

static bool document_subtree_has_little_visible_text(lxb_dom_node_t *root,
                                                     size_t limit)
{
    if (root == NULL) return false;
    size_t visible = 0;
    bool previous_space = true;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        bool skip = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "script")
                || document_name_is(node, "style")
                || document_name_is(node, "template")
                || document_name_is(node, "svg"));
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t at = 0; text != NULL && at < length; at++) {
                bool space = isspace((unsigned char) text[at]) != 0;
                if (!space || !previous_space) visible++;
                previous_space = space;
                if (visible > limit) return false;
            }
        }
        node = document_bounded_next(node, root, skip);
    }
    return true;
}

static bool document_subtree_has_little_active_text(lxb_dom_node_t *root,
                                                    size_t limit)
{
    if (root == NULL) return false;
    size_t visible = 0;
    bool previous_space = true;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        bool skip = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "script")
                || document_name_is(node, "style")
                || document_name_is(node, "template")
                || document_name_is(node, "svg")
                || document_name_is(node, "noscript"));
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t at = 0; text != NULL && at < length; at++) {
                bool space = isspace((unsigned char) text[at]) != 0;
                if (!space || !previous_space) visible++;
                previous_space = space;
                if (visible > limit) return false;
            }
        }
        node = document_bounded_next(node, root, skip);
    }
    return true;
}

static bool document_static_shell_id(const char *id, size_t length)
{
    static const char *const ids[] = {"root", "app", "__next", "__nuxt"};
    for (size_t at = 0; at < sizeof(ids) / sizeof(ids[0]); at++) {
        size_t wanted = strlen(ids[at]);
        if (length == wanted && strncasecmp(id, ids[at], wanted) == 0)
            return true;
    }
    return false;
}

/* A failed or deliberately shed client bundle can leave a branded header and
   an otherwise empty hydration root. Fill a conventional empty root when one
   exists; a body with effectively no authored text is itself a safe fallback
   target for shells that mount without an id. Useful server-rendered content
   is never replaced by this compatibility path. */
static lxb_dom_node_t *document_static_shell_target(lxb_dom_node_t *body)
{
    if (body == NULL) return NULL;
    if (document_loading_only_body(body)) return body;
    if (!document_subtree_has_little_visible_text(body, 48u)) return NULL;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t id_length = 0;
            const char *id = document_attribute(node, "id", &id_length);
            if (id != NULL && document_static_shell_id(id, id_length)
                && document_subtree_has_little_visible_text(node, 12u)) {
                return node;
            }
        }
        node = document_bounded_next(node, body, false);
    }
    return document_subtree_has_little_visible_text(body, 12u) ? body : NULL;
}

static const char *document_meta_description(PocDocument *document,
                                             size_t *length)
{
    if (length != NULL) *length = 0;
    if (document == NULL || document->html == NULL) return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "meta")) {
            size_t key_length = 0;
            const char *key = document_attribute(node, "name", &key_length);
            if (key == NULL) {
                key = document_attribute(node, "property", &key_length);
            }
            bool description = key != NULL
                && ((key_length == 11u
                     && strncasecmp(key, "description", key_length) == 0)
                    || (key_length == 14u
                        && strncasecmp(key, "og:description", key_length) == 0));
            if (description) {
                const char *content = document_attribute(
                    node, "content", length);
                if (content != NULL && length != NULL && *length >= 24u)
                    return content;
            }
        }
        node = document_bounded_next(node, root, false);
    }
    return NULL;
}

static const char *document_meta_preview_image(PocDocument *document,
                                               size_t *length)
{
    if (length != NULL) *length = 0;
    if (document == NULL || document->html == NULL) return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "meta")) {
            size_t key_length = 0;
            const char *key = document_attribute(node, "property", &key_length);
            if (key == NULL) key = document_attribute(node, "name", &key_length);
            bool image = key != NULL
                && ((key_length == 8u
                     && strncasecmp(key, "og:image", key_length) == 0)
                    || (key_length == 13u
                        && strncasecmp(key, "twitter:image", key_length) == 0));
            if (image) {
                const char *content = document_attribute(node, "content", length);
                size_t content_length = length == NULL ? 0u : *length;
                if (content != NULL && content_length >= 9u
                    && content_length <= 512u
                    && ((content_length >= 8u
                         && strncasecmp(content, "https://", 8u) == 0)
                        || (content_length >= 7u
                            && strncasecmp(content, "http://", 7u) == 0))) {
                    return content;
                }
            }
        }
        node = document_bounded_next(node, root, false);
    }
    if (length != NULL) *length = 0;
    return NULL;
}

static bool document_markup_append(char *output, size_t capacity,
                                   size_t *used, const char *text,
                                   size_t length, bool escape)
{
    if (output == NULL || used == NULL || text == NULL) return false;
    for (size_t at = 0; at < length; at++) {
        const char *piece = text + at;
        size_t piece_length = 1;
        if (escape) {
            if (text[at] == '&') { piece = "&amp;"; piece_length = 5; }
            else if (text[at] == '<') { piece = "&lt;"; piece_length = 4; }
            else if (text[at] == '>') { piece = "&gt;"; piece_length = 4; }
            else if (text[at] == '"') { piece = "&quot;"; piece_length = 6; }
        }
        if (piece_length > capacity - *used - 1u) return false;
        memcpy(output + *used, piece, piece_length);
        *used += piece_length;
    }
    output[*used] = '\0';
    return true;
}

typedef struct {
    const char *href;
    size_t href_length;
    char label[48];
    size_t label_length;
} DocumentStaticNavigationLink;

static bool document_node_is_within_named(lxb_dom_node_t *node,
                                          const char *name,
                                          lxb_dom_node_t **ancestor)
{
    for (size_t depth = 0; node != NULL && depth < 12u;
         depth++, node = node->parent) {
        if (!document_name_is(node, name)) continue;
        if (ancestor != NULL) *ancestor = node;
        return true;
    }
    return false;
}

static size_t document_static_link_label(lxb_dom_node_t *anchor,
                                         char *output, size_t capacity)
{
    if (anchor == NULL || output == NULL || capacity < 2u) return 0;
    size_t used = 0;
    lxb_dom_node_t *node = anchor;
    for (size_t visited = 0; node != NULL && visited < 96u; visited++) {
        bool skip = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "script")
                || document_name_is(node, "style")
                || document_name_is(node, "svg"));
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t at = 0; text != NULL && at < length; at++) {
                unsigned char character = (unsigned char) text[at];
                if (isspace(character)) character = ' ';
                if (character == ' '
                    && (used == 0 || output[used - 1] == ' ')) continue;
                if (used + 1u >= capacity) break;
                output[used++] = (char) character;
            }
        }
        node = document_bounded_next(node, anchor, skip);
    }
    while (used > 0 && output[used - 1] == ' ') used--;
    if (used == 0) {
        static const char *const label_attributes[] = {
            "aria-label", "title"
        };
        for (size_t at = 0;
             at < sizeof(label_attributes) / sizeof(label_attributes[0]);
             at++) {
            size_t length = 0;
            const char *label = document_attribute(
                anchor, label_attributes[at], &length);
            if (label == NULL || length == 0) continue;
            if (length >= capacity) length = capacity - 1u;
            memcpy(output, label, length);
            used = length;
            break;
        }
    }
    if (used == 0) {
        lxb_dom_node_t *descendant = anchor->first_child;
        for (size_t visited = 0;
             descendant != NULL && visited < 32u; visited++) {
            if (descendant->type == LXB_DOM_NODE_TYPE_ELEMENT
                && document_name_is(descendant, "img")) {
                size_t length = 0;
                const char *alt = document_attribute(
                    descendant, "alt", &length);
                if (alt != NULL && length != 0) {
                    if (length >= capacity) length = capacity - 1u;
                    memcpy(output, alt, length);
                    used = length;
                    break;
                }
            }
            descendant = document_bounded_next(descendant, anchor, false);
        }
    }
    output[used] = '\0';
    return used;
}

static bool document_static_navigation_duplicate(
    const DocumentStaticNavigationLink *links, size_t count,
    const char *href, size_t href_length)
{
    for (size_t at = 0; at < count; at++) {
        if (links[at].href_length == href_length
            && memcmp(links[at].href, href, href_length) == 0) return true;
    }
    return false;
}

/* Some server-rendered mobile headers leave an empty hydration placeholder
   while retaining a complete desktop navigation in the same header.  When
   page script was deliberately shed, turn that authored navigation into a
   small disclosure rather than inventing destinations or leaving a dead
   blank.  The scan, output, and number of copied links are all fixed-size. */
static bool document_install_static_navigation_fallback(
    PocDocument *document)
{
    if (document == NULL || document->html == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *placeholder = NULL;
    lxb_dom_node_t *header = NULL;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        lxb_dom_node_t *candidate_header = NULL;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "noscript")
            && node->first_child == NULL
            && document_node_is_within_named(node, "header",
                                             &candidate_header)) {
            placeholder = node;
            header = candidate_header;
            break;
        }
        node = document_bounded_next(node, root, false);
    }
    if (placeholder == NULL || header == NULL) return false;

    DocumentStaticNavigationLink links[6] = {0};
    size_t link_count = 0;
    node = header;
    for (size_t visited = 0; node != NULL && visited < 384u
         && link_count < sizeof(links) / sizeof(links[0]); visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "a")) {
            size_t href_length = 0;
            const char *href = document_attribute(node, "href", &href_length);
            size_t label_length = document_static_link_label(
                node, links[link_count].label,
                sizeof(links[link_count].label));
            if (href != NULL && href_length != 0u && label_length > 1u
                && href[0] != '#'
                && !document_static_navigation_duplicate(
                    links, link_count, href, href_length)) {
                links[link_count].href = href;
                links[link_count].href_length = href_length;
                links[link_count].label_length = label_length;
                link_count++;
            }
        }
        node = document_bounded_next(node, header, false);
    }
    if (link_count < 3u) return false;

    const size_t capacity = 1536u;
    char *markup = budget_malloc(document->budget, capacity);
    if (markup == NULL) return false;
    size_t used = 0;
#define APPEND_NAV_LITERAL(value) \
    document_markup_append(markup, capacity, &used, \
                           (value), sizeof(value) - 1u, false)
    bool valid = APPEND_NAV_LITERAL(
        "<details style=\"position:relative;font:16px sans-serif\">"
        "<summary aria-label=\"Menu\" style=\"font-size:26px;line-height:32px;"
        "width:36px\">&#59136;</summary><nav style=\"position:absolute;"
        "left:0;top:36px;width:190px;padding:8px;background:#fff;"
        "border:1px solid #c8ced5;border-radius:10px;z-index:20\">");
    for (size_t at = 0; valid && at < link_count; at++) {
        valid = APPEND_NAV_LITERAL(
                    "<a style=\"display:block;padding:7px;color:#111;\" href=\"")
            && document_markup_append(markup, capacity, &used,
                                      links[at].href,
                                      links[at].href_length, true)
            && APPEND_NAV_LITERAL("\">")
            && document_markup_append(markup, capacity, &used,
                                      links[at].label,
                                      links[at].label_length, true)
            && APPEND_NAV_LITERAL("</a>");
    }
    valid = valid && APPEND_NAV_LITERAL("</nav></details>");
#undef APPEND_NAV_LITERAL
    /* A scripting-aware UA hides <noscript> itself. Replace its immediate
       wrapper when one exists so the fallback remains visible even though
       the document did attempt scripts. */
    lxb_dom_node_t *target = placeholder;
    if (placeholder->parent != NULL
        && placeholder->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        target = placeholder->parent;
    }
    bool changed = valid
        && document_set_element_inner_html(document, target, markup, used);
    budget_free(document->budget, markup);
    return changed;
}

static bool document_attribute_has_token(lxb_dom_node_t *node,
                                         const char *name,
                                         const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t wanted_length = wanted == NULL ? 0u : strlen(wanted);
    if (value == NULL || wanted_length == 0u) return false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) value[at])) at++;
        if (at - start == wanted_length
            && strncasecmp(value + start, wanted, wanted_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool document_attribute_contains(lxb_dom_node_t *node,
                                        const char *name,
                                        const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t wanted_length = wanted == NULL ? 0u : strlen(wanted);
    if (value == NULL || wanted_length == 0u || wanted_length > length) {
        return false;
    }
    for (size_t at = 0; at <= length - wanted_length; at++) {
        if (strncasecmp(value + at, wanted, wanted_length) == 0) return true;
    }
    return false;
}

static bool document_has_authored_banner(lxb_dom_node_t *body)
{
    if (body == NULL) return false;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        bool banner = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "header")
                || document_attribute_has_token(node, "role", "banner"));
        if (banner
            && !document_subtree_has_little_active_text(node, 3u)) {
            return true;
        }
        if (banner) {
            lxb_dom_node_t *child = node;
            for (size_t nested = 0;
                 child != NULL && nested < 96u; nested++) {
                if (child->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && document_name_is(child, "a")) return true;
                child = document_bounded_next(child, node, false);
            }
        }
        node = document_bounded_next(node, body, false);
    }
    return false;
}

static const char *document_app_name(PocDocument *document, size_t *length,
                                     bool *app_shell)
{
    if (length != NULL) *length = 0;
    if (app_shell != NULL) *app_shell = false;
    if (document == NULL || document->html == NULL) return NULL;
    const char *name = NULL;
    size_t name_length = 0;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 768u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "link")
            && document_attribute_has_token(node, "rel", "manifest")) {
            if (app_shell != NULL) *app_shell = true;
        } else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                   && document_name_is(node, "meta")) {
            size_t key_length = 0;
            const char *key = document_attribute(node, "name", &key_length);
            if (key == NULL) {
                key = document_attribute(node, "property", &key_length);
            }
            bool application_name = key != NULL
                && ((key_length == 16u
                     && strncasecmp(key, "application-name", key_length) == 0)
                    || (key_length == 25u
                        && strncasecmp(key, "apple-mobile-web-app-title",
                                       key_length) == 0)
                    || (key_length == 12u
                        && strncasecmp(key, "og:site_name", key_length) == 0));
            if (application_name) {
                size_t content_length = 0;
                const char *content = document_attribute(
                    node, "content", &content_length);
                if (content != NULL && content_length != 0u
                    && name == NULL) {
                    name = content;
                    name_length = content_length;
                }
                if (app_shell != NULL) *app_shell = true;
            }
        }
        node = document_bounded_next(node, root, false);
    }
    if (name == NULL && document->title != NULL
        && app_shell != NULL && *app_shell) {
        name = document->title;
        name_length = strlen(name);
    }
    if (name_length > 48u) name_length = 48u;
    if (length != NULL) *length = name_length;
    return name;
}

/* A client-rendered application can retain useful navigation and identity
   metadata while leaving its top bar entirely to a shed external bundle.
   Recover one compact, ordinary-flow header from that authored information.
   The evidence gates avoid adding browser-invented chrome to content pages;
   no destination is guessed and no new resource is fetched. */
static bool document_install_recovered_app_header(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    bool authored_banner = document_has_authored_banner(body);
    if (body == NULL || authored_banner) return false;

    size_t name_length = 0;
    bool app_shell = false;
    const char *name = document_app_name(document, &name_length, &app_shell);
    if (!app_shell || name == NULL || name_length < 2u) return false;

    DocumentStaticNavigationLink links[6] = {0};
    size_t link_count = 0;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0; node != NULL && visited < 1024u
         && link_count < sizeof(links) / sizeof(links[0]); visited++) {
        bool navigation = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (document_name_is(node, "nav")
                || document_attribute_has_token(node, "role", "navigation")
                || document_attribute_contains(
                    node, "aria-label", "navigation")
                || document_attribute_contains(
                    node, "data-testid", "navigation"));
        if (navigation) {
            lxb_dom_node_t *candidate = node;
            for (size_t nested = 0; candidate != NULL && nested < 384u
                 && link_count < sizeof(links) / sizeof(links[0]); nested++) {
                if (candidate->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && document_name_is(candidate, "a")) {
                    size_t href_length = 0;
                    const char *href = document_attribute(
                        candidate, "href", &href_length);
                    size_t label_length = document_static_link_label(
                        candidate, links[link_count].label,
                        sizeof(links[link_count].label));
                    if (href != NULL && href_length != 0u
                        && href[0] != '#' && label_length > 1u
                        && !document_static_navigation_duplicate(
                            links, link_count, href, href_length)) {
                        links[link_count].href = href;
                        links[link_count].href_length = href_length;
                        links[link_count].label_length = label_length;
                        link_count++;
                    }
                }
                candidate = document_bounded_next(candidate, node, false);
            }
            if (link_count >= 2u) break;
            link_count = 0;
        }
        node = document_bounded_next(node, body, false);
    }
    if (link_count < 2u) return false;

    const size_t capacity = 1792u;
    char *markup = budget_malloc(document->budget, capacity);
    if (markup == NULL) return false;
    size_t used = 0;
#define APPEND_APP_LITERAL(value) \
    document_markup_append(markup, capacity, &used, \
                           (value), sizeof(value) - 1u, false)
    bool valid = APPEND_APP_LITERAL(
        "<span style=\"display:block;flex:1 1 auto;min-width:0;"
        "max-width:320px;color:#111;font-weight:700;white-space:nowrap;"
        "overflow:hidden;text-overflow:ellipsis\">");
    valid = valid && document_markup_append(
        markup, capacity, &used, name, name_length, true);
    valid = valid && APPEND_APP_LITERAL(
        "</span><details style=\"position:relative;margin-left:12px\">");
    valid = valid && APPEND_APP_LITERAL(
            "<summary style=\"padding:7px 10px;border:1px solid #aeb6c2;"
            "border-radius:8px;color:#111\">Menu</summary>"
            "<nav style=\"position:absolute;right:0;top:38px;width:190px;"
            "padding:8px;background:#fff;border:1px solid #c8ced5;"
            "border-radius:10px;z-index:20\">");
    for (size_t at = 0; valid && at < link_count; at++) {
        valid = APPEND_APP_LITERAL(
                    "<a style=\"display:block;padding:7px;color:#111\" href=\"")
            && document_markup_append(markup, capacity, &used,
                                      links[at].href,
                                      links[at].href_length, true)
            && APPEND_APP_LITERAL("\">")
            && document_markup_append(markup, capacity, &used,
                                      links[at].label,
                                      links[at].label_length, true)
            && APPEND_APP_LITERAL("</a>");
    }
    valid = valid && APPEND_APP_LITERAL("</nav></details>");
#undef APPEND_APP_LITERAL

    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    static const lxb_char_t header_name[] = "header";
    lxb_dom_element_t *element = valid
        ? lxb_dom_document_create_element(
              &document->html->dom_document, header_name,
              sizeof(header_name) - 1u, NULL)
        : NULL;
    lxb_dom_node_t *header = element == NULL
        ? NULL : lxb_dom_interface_node(element);
    size_t body_style_length = 0;
    const char *body_style = document_attribute(
        body, "style", &body_style_length);
    char recovered_body_style[640];
    size_t recovered_body_style_length = 0;
    bool body_style_fits = body_style == NULL || body_style_length == 0u
        || body_style_length < sizeof(recovered_body_style) - 24u;
    if (body_style_fits && body_style != NULL && body_style_length != 0u) {
        memcpy(recovered_body_style, body_style, body_style_length);
        recovered_body_style_length = body_style_length;
        if (recovered_body_style[recovered_body_style_length - 1u] != ';') {
            recovered_body_style[recovered_body_style_length++] = ';';
        }
    }
    static const char reserved_top[] = "padding-top:48px";
    memcpy(recovered_body_style + recovered_body_style_length,
           reserved_top, sizeof(reserved_top) - 1u);
    recovered_body_style_length += sizeof(reserved_top) - 1u;
    recovered_body_style[recovered_body_style_length] = '\0';
    bool changed = body_style_fits && header != NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "style", 5u,
               (const lxb_char_t *)
                   "position:fixed;left:0;top:0;width:100%;height:48px;"
                   "display:flex;align-items:center;"
                   "justify-content:space-between;padding:0 16px;"
                   "border-bottom:1px solid #d8dee8;background:#fff;"
                   "font:15px sans-serif;box-sizing:border-box;z-index:30",
               sizeof("position:fixed;left:0;top:0;width:100%;height:48px;"
                      "display:flex;align-items:center;"
                      "justify-content:space-between;padding:0 16px;"
                      "border-bottom:1px solid #d8dee8;background:#fff;"
                      "font:15px sans-serif;box-sizing:border-box;z-index:30")
                   - 1u)
               != NULL
        && document_set_element_inner_html(document, header, markup, used)
        && (body->first_child == NULL
            ? lxb_dom_node_append_child(body, header) == LXB_DOM_EXCEPTION_OK
            : lxb_dom_node_insert_before_spec(
                  body, header, body->first_child) == LXB_DOM_EXCEPTION_OK)
        && lxb_dom_element_set_attribute(
               lxb_dom_interface_element(body),
               (const lxb_char_t *) "style", 5u,
               (const lxb_char_t *) recovered_body_style,
               recovered_body_style_length) != NULL;
    if (!changed && header != NULL) {
        if (header->parent != NULL) lxb_dom_node_remove(header);
        lxb_dom_node_destroy_deep(header);
    }
    document_allocation_owner_leave(document, previous);
    budget_free(document->budget, markup);
    return changed;
}

/* If the bounded script pipeline could not start, a scripting parse can
   leave the page's authored <noscript> fallback as one raw-text node. On an
   otherwise empty shell, parse that bounded fragment exactly as a no-script
   user agent would. This is content-shaped recovery: useful active body text
   always wins, and script-bearing fallback markup is rejected. */
static bool document_install_static_noscript_fallback(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL
        || !document_subtree_has_little_active_text(body, 16u)) return false;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_name_is(node, "noscript")
            && node->first_child != NULL
            && node->first_child->type == LXB_DOM_NODE_TYPE_TEXT
            && node->first_child->next == NULL) {
            size_t length = 0;
            const char *markup = document_text_data(node->first_child, &length);
            if (markup == NULL || length < 24u || length > 4096u) return false;
            size_t visible = 0;
            bool contains_script = false;
            for (size_t at = 0; at < length; at++) {
                if (!isspace((unsigned char) markup[at])) visible++;
                static const char script_open[] = "<script";
                if (at + sizeof(script_open) - 1u <= length
                    && strncasecmp(markup + at, script_open,
                                   sizeof(script_open) - 1u) == 0) {
                    contains_script = true;
                    break;
                }
            }
            return visible >= 24u && !contains_script
                && document_set_element_inner_html(document, body,
                                                   markup, length)
                && document_refresh(document);
        }
        node = document_bounded_next(node, body, false);
    }
    return false;
}

bool document_install_static_shell_fallback(PocDocument *document)
{
    if (document_install_static_noscript_fallback(document)) return true;
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *target = document_static_shell_target(body);
    size_t description_length = 0;
    const char *description = document_meta_description(
        document, &description_length);
    if (body == NULL || target == NULL || description == NULL) {
        bool navigation_changed =
            document_install_static_navigation_fallback(document);
        bool app_header_changed = navigation_changed ? false
            : document_install_recovered_app_header(document);
        return (navigation_changed || app_header_changed)
            && document_refresh(document);
    }
    if (description_length > 320u) description_length = 320u;
    size_t title_length = document->title == NULL
        ? 0 : strlen(document->title);
    if (title_length > 96u) title_length = 96u;
    size_t image_length = 0;
    const char *image = document_meta_preview_image(document, &image_length);

    const size_t capacity = 2048u;
    char *markup = budget_malloc(document->budget, capacity);
    if (markup == NULL) return false;
    size_t used = 0;
#define APPEND_LITERAL(value) \
    document_markup_append(markup, capacity, &used, \
                           (value), sizeof(value) - 1u, false)
    bool valid = APPEND_LITERAL(
        "<main style=\"max-width:420px;margin:28px auto;padding:22px;"
        "font:16px sans-serif;line-height:1.45;background:#fff;"
        "color:#1f2937;border:1px solid #d8dee8;border-radius:14px\">")
        && (image == NULL
            || (APPEND_LITERAL(
                    "<img id=\"tilefinch-static-preview\" alt=\"\" "
                    "style=\"display:block;max-width:160px;"
                    "max-height:96px;margin:0 auto 14px\" src=\"")
                && document_markup_append(markup, capacity, &used,
                                          image, image_length, true)
                && APPEND_LITERAL("\">")))
        && (title_length == 0
            || (APPEND_LITERAL("<h1 style=\"font-size:24px;margin:0 0 14px\">")
                && document_markup_append(markup, capacity, &used,
                                          document->title, title_length, true)
                && APPEND_LITERAL("</h1>")))
        && APPEND_LITERAL("<p>")
        && document_markup_append(markup, capacity, &used,
                                  description, description_length, true)
        && APPEND_LITERAL("</p><p style=\"color:#5f6875;font-size:13px\">"
                          "The interactive page could not start, so its "
                          "published summary is shown instead.</p></main>");
#undef APPEND_LITERAL
    bool changed = valid
        && document_set_element_inner_html(document, target, markup, used)
        && document_refresh(document);
    budget_free(document->budget, markup);
    return changed;
}

void document_note_connected_mutation(PocDocument *document)
{
    if (document == NULL) return;
    document->content_generation++;
    if (document->content_generation == 0) document->content_generation = 1;
}

bool document_set_element_inner_html(PocDocument *document,
                                     lxb_dom_node_t *element,
                                     const char *html, size_t length)
{
    if (document == NULL || document->html == NULL || element == NULL
        || element->type != LXB_DOM_NODE_TYPE_ELEMENT
        || (html == NULL && length != 0)
        || element->owner_document != &document->html->dom_document) {
        return false;
    }

    lxb_dom_document_t *dom = &document->html->dom_document;
    lxb_html_parser_t *streaming_parser = dom->parser;
    bool parser_active = streaming_parser != NULL
        && lxb_html_parser_state(streaming_parser)
               == LXB_HTML_PARSER_STATE_PROCESS;
    if (parser_active) dom->parser = NULL;

    bool updated = lxb_html_element_inner_html_set(
        lxb_html_interface_element(element),
        (const lxb_char_t *) (html == NULL ? "" : html), length) != NULL;

    if (parser_active) {
        lxb_html_parser_t *fragment_parser = dom->parser;
        dom->parser = streaming_parser;
        lxb_html_parser_destroy(fragment_parser);
    }
    return updated;
}

const char *document_body_text(PocDocument *document)
{
    if (document == NULL || document->html == NULL
        || document->budget == NULL) return NULL;
    if (document->body_text != NULL) return document->body_text;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (document->text_bytes > SIZE_MAX - document->body_text_node_count
            || document->text_bytes + document->body_text_node_count
                   == SIZE_MAX) return NULL;
        size_t capacity = document->text_bytes
                          + document->body_text_node_count + 1;
        BudgetAllocationOwner previous =
            document_allocation_owner_enter(document);
        char *body_text = budget_malloc(document->budget, capacity);
        if (body_text == NULL) {
            document_allocation_owner_leave(document, previous);
            return NULL;
        }
        size_t offset = 0;
        lxb_dom_node_t *body = document_body_node(document);
        bool copied = body == NULL || body->first_child == NULL
            || copy_body_text(body->first_child, false, body_text,
                              capacity, &offset);
        if (copied && offset < capacity) {
            body_text[offset] = '\0';
            document->body_text = body_text;
            document->body_text_length = offset;
            document_allocation_owner_leave(document, previous);
            return body_text;
        }
        budget_free(document->budget, body_text);
        document_allocation_owner_leave(document, previous);

        /* A raw or delayed mutation can invalidate the cached aggregate
           counts before its normal refresh boundary. Recompute once and
           retry; copy_body_text remains bounded if the inconsistency cannot
           be repaired. */
        if (attempt != 0 || !document_refresh(document)) return NULL;
    }
    return NULL;
}

void document_destroy(PocDocument *document)
{
    if (document == NULL) {
        return;
    }
    if (document->budget != NULL) {
        DocumentControlState *state = document->control_states;
        while (state != NULL) {
            DocumentControlState *next = state->next;
            if (state->node != NULL && state->node->user == state) {
                state->node->user = NULL;
            }
            budget_free(document->budget, state->value);
            budget_free(document->budget, state);
            state = next;
        }
        budget_free(document->budget, document->title);
        budget_free(document->budget, document->body_text);
        budget_free(document->budget, document->base_href_snapshot);
        budget_free(document->budget, document->frozen_base_url);
    }
    if (document->html != NULL) {
        document->html = lxb_html_document_destroy(document->html);
    }
    memset(document, 0, sizeof(*document));
}

lxb_dom_node_t *document_body_node(const PocDocument *document)
{
    if (document == NULL || document->html == NULL) {
        return NULL;
    }
    lxb_html_body_element_t *body = lxb_html_document_body_element(document->html);
    return body == NULL ? NULL : lxb_dom_interface_node(body);
}

const char *document_element_name(lxb_dom_node_t *node, size_t *length)
{
    if (length != NULL) {
        *length = 0;
    }
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return NULL;
    }
    return (const char *) lxb_dom_element_local_name(
        lxb_dom_interface_element(node), length);
}

const char *document_attribute(lxb_dom_node_t *node, const char *name,
                               size_t *length)
{
    if (length != NULL) {
        *length = 0;
    }
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT || name == NULL) {
        return NULL;
    }
    return (const char *) lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        strlen(name), length);
}

const char *document_control_value(lxb_dom_node_t *node, size_t *length)
{
    if (length != NULL) *length = 0;
    if (node == NULL) return NULL;
    const DocumentControlState *state = node->user;
    if (state == NULL || state->magic != DOCUMENT_CONTROL_STATE_MAGIC
        || state->node != node) return NULL;
    if (length != NULL) *length = state->length;
    return state->value;
}

lxb_dom_node_t *document_control_parser_form_owner(lxb_dom_node_t *node)
{
    if (node == NULL) return NULL;
    const DocumentControlState *state = node->user;
    if (state == NULL || state->magic != DOCUMENT_CONTROL_STATE_MAGIC
        || state->node != node) return NULL;
    lxb_dom_node_t *owner = state->parser_form_owner;
    return owner != NULL && owner->owner_document == node->owner_document
        ? owner : NULL;
}

bool document_has_parser_form_owners(const PocDocument *document)
{
    return document != NULL && document->parser_form_owner_count != 0;
}

bool document_control_value_set(PocDocument *document, lxb_dom_node_t *node,
                                const char *value, size_t length)
{
    if (document == NULL || document->budget == NULL || node == NULL
        || (value == NULL && length != 0)
        || length > DOCUMENT_CONTROL_VALUE_LIMIT
        || node->owner_document
               != &document->html->dom_document) return false;
    DocumentControlState *state = node->user;
    if (state != NULL
        && (state->magic != DOCUMENT_CONTROL_STATE_MAGIC
            || state->node != node)) return false;
    BudgetAllocationOwner previous =
        document_allocation_owner_enter(document);
    char *copy = budget_malloc(document->budget, length + 1);
    if (copy == NULL) {
        document_allocation_owner_leave(document, previous);
        return false;
    }
    if (length != 0) memcpy(copy, value, length);
    copy[length] = '\0';
    if (state == NULL) state = document_control_state_ensure(document, node);
    if (state == NULL) {
        budget_free(document->budget, copy);
        document_allocation_owner_leave(document, previous);
        return false;
    }
    budget_free(document->budget, state->value);
    state->value = copy;
    state->length = length;
    document_allocation_owner_leave(document, previous);
    return true;
}

bool document_control_resize_set(PocDocument *document, lxb_dom_node_t *node,
                                 int width, int height)
{
    if (document == NULL || node == NULL || width < 1 || height < 1
        || width > 2048 || height > 2048
        || node->owner_document
               != &document->html->dom_document) return false;
    DocumentControlState *state = document_control_state_ensure(
        document, node);
    if (state == NULL) return false;
    state->resized_width = width;
    state->resized_height = height;
    return true;
}

bool document_control_resize(lxb_dom_node_t *node,
                             int *width, int *height)
{
    if (node == NULL) return false;
    DocumentControlState *state = node->user;
    if (state == NULL || state->magic != DOCUMENT_CONTROL_STATE_MAGIC
        || state->node != node || state->resized_width < 1
        || state->resized_height < 1) return false;
    if (width != NULL) *width = state->resized_width;
    if (height != NULL) *height = state->resized_height;
    return true;
}

static void document_control_state_discard_node(
    PocDocument *document, lxb_dom_node_t *node)
{
    if (document == NULL || document->budget == NULL || node == NULL
        || node->user == NULL) return;
    DocumentControlState *candidate = node->user;
    if (candidate->magic != DOCUMENT_CONTROL_STATE_MAGIC
        || candidate->node != node) {
        /* Lexbor copies node->user while cloning. A clone must never retire
           the original node's engine-owned state. */
        node->user = NULL;
        return;
    }
    DocumentControlState **link = &document->control_states;
    while (*link != NULL && *link != candidate) link = &(*link)->next;
    if (*link == NULL) return;
    DocumentControlState *state = *link;
    *link = state->next;
    node->user = NULL;
    if (state->parser_form_owner != NULL
        && document->parser_form_owner_count != 0) {
        document->parser_form_owner_count--;
    }
    state->magic = 0;
    state->node = NULL;
    budget_free(document->budget, state->value);
    budget_free(document->budget, state);
    if (document->control_state_count != 0) {
        document->control_state_count--;
    }
}

static bool document_control_state_walk(
    PocDocument *document, lxb_dom_node_t *root, size_t ownership_depth,
    size_t *visited, bool discard)
{
    if (root == NULL || visited == NULL || ownership_depth >= 8) return false;
    lxb_dom_node_t *node = root;
    for (;;) {
        if ((*visited)++ >= DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if (discard) document_control_state_discard_node(document, node);
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node->ns == LXB_NS_HTML) {
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            if (name != NULL && name_length == 8
                && strncasecmp(name, "template", 8) == 0) {
                lxb_html_template_element_t *element =
                    lxb_html_interface_template(node);
                lxb_dom_node_t *content = element->content == NULL ? NULL
                    : lxb_dom_interface_node(element->content);
                if (content != NULL
                    && !document_control_state_walk(
                           document, content, ownership_depth + 1,
                           visited, discard)) {
                    return false;
                }
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) return true;
        node = node->next;
    }
}

static bool document_control_parser_owner_discard(
    PocDocument *document, lxb_dom_node_t *root, bool discard)
{
    if (document == NULL || root == NULL) return false;
    size_t visited = 0;
    for (DocumentControlState *state = document->control_states;
         state != NULL; state = state->next) {
        lxb_dom_node_t *owner = state->parser_form_owner;
        for (lxb_dom_node_t *at = owner; at != NULL; at = at->parent) {
            if (visited++ >= DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
            if (at != root) continue;
            if (discard) {
                state->parser_form_owner = NULL;
                if (document->parser_form_owner_count != 0) {
                    document->parser_form_owner_count--;
                }
            }
            break;
        }
    }
    return true;
}

bool document_control_state_discard_subtree(
    PocDocument *document, lxb_dom_node_t *root)
{
    if (document == NULL || root == NULL) return false;
    /* Prove the complete ordinary/template ownership walk before freeing the
       first state. A cap hit must retain everything, not partially unlink the
       control-state list and then report failure. */
    size_t visited = 0;
    if (!document_control_state_walk(
            document, root, 0, &visited, false)
        || !document_control_parser_owner_discard(
               document, root, false)) return false;
    if (!document_control_parser_owner_discard(
            document, root, true)) return false;
    visited = 0;
    return document_control_state_walk(
        document, root, 0, &visited, true);
}

const char *document_text_data(lxb_dom_node_t *node, size_t *length)
{
    if (length != NULL) {
        *length = 0;
    }
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_TEXT) {
        return NULL;
    }
    lxb_dom_text_t *text = lxb_dom_interface_text(node);
    if (length != NULL) {
        *length = text->char_data.data.length;
    }
    return (const char *) text->char_data.data.data;
}

bool document_base_url(PocDocument *document, const char *document_url,
                       char *output, size_t output_size)
{
    if (document == NULL || document->html == NULL || document_url == NULL
        || output == NULL || output_size == 0) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    size_t visited = 0;
    while (node != NULL) {
        if (++visited > DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if (name_is(node, "base")) {
            size_t href_length = 0;
            const char *href = document_attribute(node, "href", &href_length);
            bool present = href != NULL
                || lxb_dom_element_has_attribute(
                       lxb_dom_interface_element(node),
                       (const lxb_char_t *) "href", 4);
            if (present) {
                if (href == NULL) href = "";
                bool unchanged = document->base_element == node
                    && document->base_href_snapshot != NULL
                    && document->frozen_base_url != NULL
                    && document->base_href_snapshot_length == href_length
                    && memcmp(document->base_href_snapshot, href,
                              href_length) == 0;
                if (!unchanged) {
                    const char *calculated = document_url;
                    char reference[TILEFINCH_URL_SERIALIZED_LIMIT];
                    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
                    if (href_length < sizeof(reference)) {
                        memcpy(reference, href, href_length);
                        reference[href_length] = '\0';
                        if (tilefinch_url_resolve(
                                document_url, reference, resolved,
                                sizeof(resolved))
                            && tilefinch_csp_allows_base_uri(
                                   &document->content_security_policy,
                                   resolved)) calculated = resolved;
                    }
                    size_t calculated_length = strlen(calculated);
                    if (href_length == SIZE_MAX
                        || calculated_length == SIZE_MAX) return false;
                    BudgetAllocationOwner previous =
                        document_allocation_owner_enter(document);
                    char *href_snapshot = budget_malloc(
                        document->budget, href_length + 1);
                    char *frozen = budget_malloc(
                        document->budget, calculated_length + 1);
                    document_allocation_owner_leave(document, previous);
                    if (href_snapshot == NULL || frozen == NULL) {
                        budget_free(document->budget, href_snapshot);
                        budget_free(document->budget, frozen);
                        return false;
                    }
                    memcpy(href_snapshot, href, href_length);
                    href_snapshot[href_length] = '\0';
                    memcpy(frozen, calculated, calculated_length + 1);
                    budget_free(document->budget,
                                document->base_href_snapshot);
                    budget_free(document->budget, document->frozen_base_url);
                    document->base_element = node;
                    document->base_href_snapshot = href_snapshot;
                    document->base_href_snapshot_length = href_length;
                    document->frozen_base_url = frozen;
                }
                size_t frozen_length = strlen(document->frozen_base_url);
                if (frozen_length >= output_size) return false;
                memcpy(output, document->frozen_base_url,
                       frozen_length + 1);
                return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    budget_free(document->budget, document->base_href_snapshot);
    budget_free(document->budget, document->frozen_base_url);
    document->base_element = NULL;
    document->base_href_snapshot = NULL;
    document->base_href_snapshot_length = 0;
    document->frozen_base_url = NULL;
    size_t fallback_length = strlen(document_url);
    if (fallback_length >= output_size) return false;
    memcpy(output, document_url, fallback_length + 1);
    return true;
}

static bool ascii_span_equal(const char *text, size_t length,
                             const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (text == NULL || length != wanted_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static bool ascii_span_contains_folded(const char *text, size_t length,
                                       const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (text == NULL || wanted_length == 0 || wanted_length > length) {
        return false;
    }
    for (size_t at = 0; at <= length - wanted_length; at++) {
        if (ascii_span_equal(text + at, wanted_length, wanted)) return true;
    }
    return false;
}

static const char *normalized_referrer_policy(const char *value,
                                              size_t length)
{
    if (value == NULL) return NULL;
    while (length != 0 && isspace((unsigned char) *value)) {
        value++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) value[length - 1])) length--;
    static const char *const known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (ascii_span_equal(value, length, known[i])) return known[i];
    }
    return NULL;
}

bool document_referrer_policy(const PocDocument *document,
                              const char *response_fallback,
                              char *output, size_t output_size)
{
    if (document == NULL || document->html == NULL
        || output == NULL || output_size == 0) return false;
    const char *selected = "";
    if (response_fallback != NULL) {
        const char *normalized = normalized_referrer_policy(
            response_fallback, strlen(response_fallback));
        if (normalized != NULL) selected = normalized;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    size_t visited = 0;
    while (node != NULL) {
        if (++visited > DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if (name_is(node, "meta")) {
            size_t name_length = 0, content_length = 0;
            const char *name = document_attribute(
                node, "name", &name_length);
            const char *content = document_attribute(
                node, "content", &content_length);
            if (name != NULL
                && ascii_span_equal(name, name_length, "referrer")) {
                const char *normalized = normalized_referrer_policy(
                    content, content_length);
                if (normalized != NULL) selected = normalized;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    size_t length = strlen(selected);
    if (length >= output_size) return false;
    memcpy(output, selected, length + 1u);
    return true;
}

static bool find_viewport_meta(lxb_dom_node_t *node,
                               lxb_dom_node_t **result)
{
    if (result == NULL) return false;
    *result = NULL;
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > DOCUMENT_TRAVERSAL_NODE_LIMIT) return false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && name_is(node, "meta")) {
            size_t name_length = 0;
            const char *name = document_attribute(node, "name", &name_length);
            if (name != NULL
                && ascii_span_equal(name, name_length, "viewport")) {
                *result = node;
                return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) node = node->parent;
        if (node == NULL || node == boundary_parent) break;
        node = node->next;
    }
    return true;
}

static bool document_doctype_is_mobile(const PocDocument *document)
{
    if (document == NULL || document->html == NULL) return false;
    lxb_dom_document_type_t *doctype =
        document->html->dom_document.doctype;
    if (doctype == NULL) return false;
    size_t public_id_length = 0;
    const char *public_id = (const char *) lxb_dom_document_type_public_id(
        doctype, &public_id_length);
    /* XHTML Mobile Profile and XHTML Basic were explicit mobile-document
       contracts before meta viewport became ubiquitous. Treating them as a
       legacy 980px desktop page makes the server's deliberately compact
       markup microscopic. The public identifiers are immutable parser input,
       so this check adds no DOM walk or per-frame state. */
    return public_id != NULL
        && (ascii_span_contains_folded(public_id, public_id_length,
                                       "XHTML Mobile")
            || ascii_span_contains_folded(public_id, public_id_length,
                                          "XHTML Basic"));
}

bool document_mobile_viewport(const PocDocument *document, int device_width,
                              int legacy_width, MobileViewport *viewport)
{
    if (document == NULL || document->html == NULL || viewport == NULL
        || device_width <= 0 || legacy_width < device_width) return false;
    *viewport = (MobileViewport) {
        .layout_width = legacy_width,
        .scale_numerator = device_width,
        .scale_denominator = legacy_width
    };
    lxb_dom_node_t *meta = NULL;
    if (!find_viewport_meta(lxb_dom_interface_node(document->html),
                            &meta)) return false;
    if (meta == NULL) {
        if (document_doctype_is_mobile(document)) {
            viewport->device_width = true;
            viewport->layout_width = device_width;
            viewport->scale_numerator = 1;
            viewport->scale_denominator = 1;
        }
        return true;
    }
    viewport->declared = true;
    size_t content_length = 0;
    const char *content = document_attribute(meta, "content", &content_length);
    if (content == NULL) return true;
    for (size_t at = 0; at < content_length;) {
        while (at < content_length
               && (isspace((unsigned char) content[at])
                   || content[at] == ',' || content[at] == ';')) at++;
        size_t name_start = at;
        while (at < content_length && content[at] != '='
               && content[at] != ',' && content[at] != ';') at++;
        size_t name_end = at;
        while (name_end > name_start
               && isspace((unsigned char) content[name_end - 1])) name_end--;
        if (at >= content_length || content[at] != '=') {
            while (at < content_length && content[at] != ','
                   && content[at] != ';') at++;
            continue;
        }
        at++;
        while (at < content_length && isspace((unsigned char) content[at])) at++;
        size_t value_start = at;
        while (at < content_length && content[at] != ','
               && content[at] != ';') at++;
        size_t value_end = at;
        while (value_end > value_start
               && isspace((unsigned char) content[value_end - 1])) value_end--;
        if (!ascii_span_equal(content + name_start, name_end - name_start,
                              "width")) continue;
        if (ascii_span_equal(content + value_start, value_end - value_start,
                             "device-width")) {
            viewport->device_width = true;
            viewport->layout_width = device_width;
            viewport->scale_numerator = 1;
            viewport->scale_denominator = 1;
            return true;
        }
        char number[16];
        size_t number_length = value_end - value_start;
        if (number_length == 0 || number_length >= sizeof(number)) continue;
        memcpy(number, content + value_start, number_length);
        number[number_length] = '\0';
        char *end = NULL;
        long parsed = strtol(number, &end, 10);
        if (end == number || *end != '\0' || parsed < 240 || parsed > 1024) {
            continue;
        }
        viewport->layout_width = (int) parsed;
        viewport->scale_numerator = device_width;
        viewport->scale_denominator = (int) parsed;
        return true;
    }
    return true;
}
