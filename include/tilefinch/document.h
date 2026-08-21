#ifndef TILEFINCH_DOCUMENT_H
#define TILEFINCH_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/html/html.h>
#include <lexbor/html/tokenizer.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_security_policy.h"

typedef struct DocumentControlState DocumentControlState;

typedef enum {
    DOCUMENT_GLYPH_SCRIPT_HAN = 1u << 0,
    DOCUMENT_GLYPH_SCRIPT_JAPANESE = 1u << 1,
    DOCUMENT_GLYPH_SCRIPT_KOREAN = 1u << 2,
    DOCUMENT_GLYPH_SCRIPT_CYRILLIC = 1u << 3,
    DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED = 1u << 4
} DocumentGlyphScript;

typedef struct {
    Budget *budget;
    BudgetAllocationOwner allocation_owner;
    TilefinchContentSecurityPolicy content_security_policy;
    lxb_html_document_t *html;
    char *title;
    char *body_text;
    size_t node_count;
    size_t element_count;
    size_t text_node_count;
    size_t attribute_count;
    size_t attribute_value_bytes;
    size_t text_bytes;
    size_t body_text_node_count;
    size_t body_text_length;
    /* Visible-text ranges observed by the existing parser statistics pass.
       The PSP frontend uses this compact hint to attach installed fallback
       packs lazily; it is not serialized and causes no storage I/O here. */
    uint8_t glyph_script_mask;
    /* Monotonic connected-content identity. Layout-owned document caches use
       this instead of rescanning the complete DOM on every relayout. */
    uint64_t content_generation;
    uint64_t inline_container_units_generation;
    bool inline_container_units_present;
    /* The connected first <base href> freezes its resolved URL when it first
       becomes authoritative. The exact href snapshot distinguishes later
       attribute changes without re-resolving an unchanged relative href
       after history.pushState/replaceState. */
    lxb_dom_node_t *base_element;
    char *base_href_snapshot;
    size_t base_href_snapshot_length;
    char *frozen_base_url;
    /* Live form values are properties, not content attributes. Keep the
       bounded renderer-facing state beside the native DOM so script changes
       repaint without making input.getAttribute("value") lie. */
    DocumentControlState *control_states;
    size_t control_state_count;
    size_t parser_form_owner_count;
    /* Aggregate parser/refresh fact used to keep ordinary pointer motion out
       of the JavaScript realm when no authored mouse/pointer attribute can
       observe it. Dynamic attribute mutations conservatively reopen the JS
       probe from the bootstrap side. */
    bool pointer_event_attributes_present;
} PocDocument;

typedef struct {
    bool declared;
    bool device_width;
    int layout_width;
    int scale_numerator;
    int scale_denominator;
} MobileViewport;

typedef bool (*DocumentElementClosedCallback)(void *opaque,
                                              PocDocument *document,
                                              lxb_dom_node_t *element);

/* A parser transaction has a distinct DOM allocation owner until
   document_parser_finish transfers the completed document to its caller.
   This keeps an aborted pumpable candidate from reclaiming incumbent DOM
   allocations made while the candidate is paused. DOM work performed by
   parser callbacks remains candidate-owned; callback-created non-DOM
   consumers must still be destroyed before document_parser_abort. Aborting
   does not traverse Lexbor's potentially partial state after an allocation
   failure. */
typedef struct {
    PocDocument document;
    Budget *budget;
    size_t bytes_fed;
    size_t chunks_fed;
    lxb_html_tokenizer_t *tokenizer;
    lxb_html_tokenizer_token_f original_token_callback;
    void *original_token_context;
    DocumentElementClosedCallback element_closed;
    void *element_closed_opaque;
    bool active;
    bool failed;
} DocumentParser;

bool document_parser_begin(DocumentParser *parser, Budget *budget);
/* Select the HTML parsing model before feeding the first byte.  In
   particular, enabled scripting makes <noscript> raw text as required by the
   HTML tree builder instead of exposing its fallback elements to style and
   layout consumers. */
bool document_parser_set_scripting(DocumentParser *parser, bool enabled);
bool document_parser_feed(DocumentParser *parser, const char *data,
                          size_t length);
void document_parser_set_element_closed_callback(
    DocumentParser *parser, DocumentElementClosedCallback callback,
    void *opaque);
bool document_parser_finish(DocumentParser *parser, PocDocument *document);
void document_parser_abort(DocumentParser *parser);

/* Scope raw Lexbor mutations to the document that owns the resulting DOM
   allocations. Callers which mutate parser.document outside parser APIs must
   use this pair; normal script/controller paths scope this automatically. */
BudgetAllocationOwner document_allocation_owner_enter(
    const PocDocument *document);
void document_allocation_owner_leave(const PocDocument *document,
                                     BudgetAllocationOwner previous_owner);

bool document_parse(PocDocument *document, Budget *budget,
                    const char *html, size_t html_length, size_t chunk_size);
bool document_refresh(PocDocument *document);
void document_note_connected_mutation(PocDocument *document);
/* Lexbor's document-owned fragment parser is not reentrant with an active
   streaming parse.  This wrapper preserves the streaming parser while
   applying a synchronous element innerHTML mutation. */
bool document_set_element_inner_html(PocDocument *document,
                                     lxb_dom_node_t *element,
                                     const char *html, size_t length);
/* Materializes the legacy aggregate body-text view on first use. The live DOM
   remains authoritative and document_refresh invalidates this derived cache. */
const char *document_body_text(PocDocument *document);
void document_destroy(PocDocument *document);
lxb_dom_node_t *document_body_node(const PocDocument *document);
const char *document_element_name(lxb_dom_node_t *node, size_t *length);
const char *document_attribute(lxb_dom_node_t *node, const char *name,
                               size_t *length);
/* CSP distinguishes authored/setAttribute style text from declarations made
   through the CSSOM. Lexbor attribute nodes provide a stable, clone-safe
   provenance slot without adding a page-sized side table. */
bool document_style_attribute_cssom_authorized(lxb_dom_node_t *node);
void document_style_attribute_set_cssom_authorized(lxb_dom_node_t *node,
                                                    bool authorized);
const char *document_control_value(lxb_dom_node_t *node, size_t *length);
bool document_control_value_set(PocDocument *document, lxb_dom_node_t *node,
                                const char *value, size_t length);
bool document_control_resize_set(PocDocument *document, lxb_dom_node_t *node,
                                 int width, int height);
bool document_control_resize(lxb_dom_node_t *node,
                             int *width, int *height);
/* Returns the parser-established form owner retained for native controls
   whose malformed-table recovery moved them outside their source form.
   Explicit form= and later DOM association rules remain the caller's
   responsibility. */
lxb_dom_node_t *document_control_parser_form_owner(lxb_dom_node_t *node);
bool document_has_parser_form_owners(const PocDocument *document);
/* Detach engine-owned live control values before Lexbor destroys a subtree.
   This prevents node->user and the document registry from retaining pointers
   to one another after script-driven detached-node reclamation. */
bool document_control_state_discard_subtree(
    PocDocument *document, lxb_dom_node_t *root);
const char *document_text_data(lxb_dom_node_t *node, size_t *length);
/* Returns the calculated document base URL. The first connected base element
   with an href attribute is authoritative, including an empty or invalid
   href, and freezes its resolved URL until that first element or exact href
   changes. A failed resolution freezes the then-current document fallback
   rather than consulting a later base element. */
bool document_base_url(PocDocument *document, const char *document_url,
                       char *output, size_t output_size);
/* Computes the document's normalized Referrer-Policy from a normalized or
   mixed-case response fallback and any valid <meta name=referrer> override.
   Invalid metadata is ignored. Traversal is nonrecursive and bounded. */
bool document_referrer_policy(const PocDocument *document,
                              const char *response_fallback,
                              char *output, size_t output_size);
bool document_mobile_viewport(const PocDocument *document, int device_width,
                              int legacy_width, MobileViewport *viewport);

#endif
