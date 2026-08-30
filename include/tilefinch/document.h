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

#define DOCUMENT_CONTROL_VALUE_LIMIT 512u

/* A fixed-size, allocation-free rollback record for one native value edit.
   The controller takes this after beforeinput author work has settled and
   restores it if refresh or relayout cannot publish the edit. */
typedef struct {
    bool valid;
    bool state_present;
    bool value_present;
    bool default_value_known;
    bool transaction_active;
    size_t value_length;
    size_t value_capacity;
    lxb_dom_node_t *node;
    char *retained_value;
    char value[DOCUMENT_CONTROL_VALUE_LIMIT + 1u];
} DocumentControlValueSnapshot;

/* A compact rollback record for the authored checkedness latch. Native
   checkbox/radio defaults can touch up to the bounded group size, so this
   deliberately stays allocation-free and small enough for a fixed array. */
typedef struct {
    bool valid;
    bool state_present;
    bool default_known;
    bool default_checked;
    lxb_dom_node_t *node;
} DocumentControlCheckedSnapshot;

typedef struct {
    Budget *budget;
    char *markup;
    size_t length;
    size_t capacity;
    size_t source_text_bytes;
    /* Bounded semantic-action census retained with the serialized body so a
       later parser prefix can replace an earlier text-only rollback source. */
    size_t source_action_count;
} DocumentBodySnapshot;

typedef enum {
    DOCUMENT_GLYPH_SCRIPT_HAN = 1u << 0,
    DOCUMENT_GLYPH_SCRIPT_JAPANESE = 1u << 1,
    DOCUMENT_GLYPH_SCRIPT_KOREAN = 1u << 2,
    DOCUMENT_GLYPH_SCRIPT_CYRILLIC = 1u << 3,
    DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED = 1u << 4,
    DOCUMENT_GLYPH_SCRIPT_ARABIC = 1u << 5,
    DOCUMENT_GLYPH_SCRIPT_HEBREW = 1u << 6
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
    uint16_t glyph_script_mask;
    /* Existing parser-census fact that keeps ordinary LTR pages out of the
       paragraph bidi pipeline without a second DOM walk. */
    bool bidi_text_present;
    /* An authored `dir` boundary can require visual reordering even when its
       current text is ASCII. Keep that uncommon fact separate so ordinary
       documents still bypass bidi before style/layout work. */
    bool bidi_markup_present;
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
    /* Aggregate authored focus intent. The engine carries one bounded
       post-layout autofocus obligation only for documents that need it. */
    bool autofocus_attribute_present;
} PocDocument;

typedef bool (*DocumentBodySnapshotReplaceCallback)(
    void *opaque, PocDocument *document,
    const char *markup, size_t length);

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
/* When a bounded script pass cannot start authored client-side UI, preserve
   useful static content: materialize a server-authored header navigation or
   replace a loading-only metadata-rich body with a small summary. Returns
   true only when it changed the connected document. */
bool document_install_static_shell_fallback(PocDocument *document);
void document_note_connected_mutation(PocDocument *document);
/* Lexbor's document-owned fragment parser is not reentrant with an active
   streaming parse.  This wrapper preserves the streaming parser while
   applying a synchronous element innerHTML mutation. */
bool document_set_element_inner_html(PocDocument *document,
                                     lxb_dom_node_t *element,
                                     const char *html, size_t length);
/* Retain one complete, bounded server-rendered body across author-script
   execution. A partial serialization is never exposed as a fallback. */
bool document_body_snapshot_capture(PocDocument *document,
                                    DocumentBodySnapshot *snapshot);
/* Allocation-free, 4K-node-bounded census used to detect a compact late
   server action without serializing the body at every parser checkpoint. */
size_t document_body_action_count(const PocDocument *document);
/* A replacement callback owns external-handle retirement and mutation
   publication; the callback-free path performs the native equivalents. */
bool document_body_snapshot_restore_if_degraded(
    PocDocument *document, const DocumentBodySnapshot *snapshot,
    DocumentBodySnapshotReplaceCallback replace, void *replace_opaque);
void document_body_snapshot_destroy(DocumentBodySnapshot *snapshot);
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
bool document_control_value_snapshot(
    PocDocument *document, lxb_dom_node_t *node,
    DocumentControlValueSnapshot *snapshot);
/* Replaces the live value while retaining the prior allocation in snapshot.
   Finish with commit on successful presentation or restore on failure. */
bool document_control_value_transaction_set(
    PocDocument *document, lxb_dom_node_t *node,
    const char *value, size_t length,
    DocumentControlValueSnapshot *snapshot);
void document_control_value_commit(
    PocDocument *document, DocumentControlValueSnapshot *snapshot);
/* Restores without allocating. It also removes a control-state record which
   the failed edit created for a node that previously had none. */
bool document_control_value_restore(
    PocDocument *document, lxb_dom_node_t *node,
    DocumentControlValueSnapshot *snapshot);
bool document_control_checked_snapshot(
    PocDocument *document, lxb_dom_node_t *node,
    DocumentControlCheckedSnapshot *snapshot);
bool document_control_checked_restore(
    PocDocument *document, lxb_dom_node_t *node,
    const DocumentControlCheckedSnapshot *snapshot);
/* Returns the bounded authored value captured before the live input/textarea
   value first changes. Reset defaults remain page-owned and survive later
   editing without being stored in author-visible data attributes. */
bool document_control_default_value(
    PocDocument *document, lxb_dom_node_t *node,
    const char **value, size_t *length);
/* Retain the authored checked state before a script-free controller default
   first mutates a checkbox/radio. The existing bounded control-state table
   owns this bit; no engine-private data-* attribute leaks into page markup. */
bool document_control_checked_default(
    PocDocument *document, lxb_dom_node_t *node, bool authored_checked,
    bool *default_checked);
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
/* Returns the first connected manifest link's borrowed href. Callers resolve
   it against document_base_url before any network or storage operation. */
const char *document_web_app_manifest_href(
    const PocDocument *document, size_t *length);

#endif
