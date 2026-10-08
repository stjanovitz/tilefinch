#ifndef TILEFINCH_DOCUMENT_H
#define TILEFINCH_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/html/html.h>
#include <lexbor/html/tokenizer.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_security_policy.h"
#include "tilefinch/text_encoding.h"

typedef struct DocumentControlState DocumentControlState;
typedef struct DocumentNonceSlot DocumentNonceSlot;
typedef struct DocumentNonceRegistry DocumentNonceRegistry;
typedef struct DocumentAdoptedSheets DocumentAdoptedSheets;
typedef struct MediaDeclaredVideoCache MediaDeclaredVideoCache;

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

/* MutationObserver records for nodes the HTML parser inserts. While an
   observer can see childList changes (armed), each parser insertion into the
   connected tree is journaled in order with its insertion-time siblings; the
   script runtime turns the journal into childList records at the microtask
   checkpoint the parser performs before a parser-blocking script, and once
   more after EOF. Nothing is recorded and nothing is allocated while unarmed.

   The journal is bounded. Past DOCUMENT_PARSER_INSERTION_LIMIT entries an
   insertion is folded into a subtree root {parent, first}: "parent gained
   the children from first onward, and anything inside them". Because the
   tree builder only appends to open elements, every later insertion under
   that parent is covered. A root is delivered as one record per child of the
   run (within the same limit), or, past it, as a record naming the parent
   itself as added so a subtree scan still reaches everything. When the root
   slots run out they collapse into one root at the common ancestor. */
#define DOCUMENT_PARSER_INSERTION_LIMIT 256u
#define DOCUMENT_PARSER_INSERTION_ROOT_LIMIT 16u

typedef struct {
    lxb_dom_node_t *parent;
    lxb_dom_node_t *node;
    lxb_dom_node_t *previous;
    lxb_dom_node_t *next;
} DocumentParserInsertion;

typedef struct {
    lxb_dom_node_t *parent;
    lxb_dom_node_t *first;
} DocumentParserInsertionRoot;

typedef struct {
    DocumentParserInsertion *entries;
    size_t count;
    size_t capacity;
    DocumentParserInsertionRoot roots[DOCUMENT_PARSER_INSERTION_ROOT_LIMIT];
    size_t root_count;
    /* Cumulative diagnostics: insertions journaled exactly, and insertions
       that could only be reported through a coalesced subtree root. */
    size_t recorded;
    size_t coalesced;
    bool armed;
} DocumentParserInsertionJournal;

typedef enum {
    DOCUMENT_GLYPH_SCRIPT_HAN = 1u << 0,
    DOCUMENT_GLYPH_SCRIPT_JAPANESE = 1u << 1,
    DOCUMENT_GLYPH_SCRIPT_KOREAN = 1u << 2,
    DOCUMENT_GLYPH_SCRIPT_CYRILLIC = 1u << 3,
    DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED = 1u << 4,
    DOCUMENT_GLYPH_SCRIPT_ARABIC = 1u << 5,
    DOCUMENT_GLYPH_SCRIPT_HEBREW = 1u << 6,
    DOCUMENT_GLYPH_SCRIPT_DEVANAGARI = 1u << 7
} DocumentGlyphScript;

#define DOCUMENT_GLYPH_SCRIPT_KINDS 8u
#define DOCUMENT_GLYPH_CENSUS_SAMPLES 4u
/* A script is in meaningful use when the visible text holds at least this
   many of its codepoints and at least one per DOCUMENT_GLYPH_OFFER_SHARE
   visible-text bytes. The share keeps a long article's language-picker list
   ("Русский", "العربية") from counting as the page's own script. */
#define DOCUMENT_GLYPH_OFFER_MINIMUM 32u
#define DOCUMENT_GLYPH_OFFER_SHARE 20u

/* Collected by the parser's existing visible-text statistics pass; no extra
   DOM walk and no font or storage access. `samples[i]` holds the page's
   codepoints of script bit i at its 1st, 4th, 16th and 64th occurrence
   (zero where the page had fewer), so a consumer can test real page
   characters against the active faces: the embedded fonts carry Latin
   Extended-A/B but not Latin Extended Additional, for example. */
typedef struct {
    uint16_t offer_mask;
    uint32_t samples[DOCUMENT_GLYPH_SCRIPT_KINDS]
                    [DOCUMENT_GLYPH_CENSUS_SAMPLES];
} DocumentGlyphCensus;

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
    /* Attribute-only script mutations skip the whole-document refresh, so
       the two totals above may lag until document_refresh_attribute_totals()
       or the next full refresh. Nothing but diagnostics reads them. */
    bool attribute_totals_stale;
    size_t text_bytes;
    size_t body_text_node_count;
    size_t body_text_length;
    /* Visible-text ranges observed by the existing parser statistics pass.
       The PSP frontend uses this compact hint to attach installed fallback
       packs lazily; it is not serialized and causes no storage I/O here. */
    uint16_t glyph_script_mask;
    /* The same pass's usage census, for the in-page language-pack offer. */
    DocumentGlyphCensus glyph_census;
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
    /* Parsed with scripting explicitly disabled (JavaScript off for this
       page): <noscript> holds ordinary elements and renders. Documents built
       any other way keep the scripting-aware rule that hides it. */
    bool noscript_rendered;
    /* Lazily populated, page-lifetime declaration cache. Keeping this on the
       retained document gives layout, image discovery, Reader and activation
       one immutable selection without rescanning large data scripts. */
    MediaDeclaredVideoCache *declared_video_cache;
    uint32_t declared_video_discovery_count;
    bool declared_video_cache_scanned;
    bool declared_video_cache_found;
    /* Native recovery controls are identified by provenance, never by their
       public diagnostic attributes. Author markup may use the same attribute
       spelling without acquiring browser-chrome activation semantics. */
    lxb_dom_node_t *declared_video_card_node;
    lxb_dom_node_t *reader_declared_video_card_node;
    DocumentParserInsertionJournal parser_insertions;
    /* [[CryptographicNonce]] slots; see document_element_nonce(). */
    DocumentNonceRegistry *nonce_registry;
    /* Constructed stylesheets and the adoptedStyleSheets lists naming them;
       see document_adopted_sheets_active(). NULL until a page adopts one. */
    DocumentAdoptedSheets *adopted_sheets;
    /* The document's character encoding (a TilefinchEncoding). Zero is
       UTF-8, the default for documents not built by a sniffing parser. */
    uint8_t encoding;
    /* The process-wide frame-container insertion count when this document's
       parse began; see document_frames_impossible(). */
    uint64_t frame_insertions_at_parse;
} PocDocument;

static inline bool document_is_declared_video_card(
    const PocDocument *document, const lxb_dom_node_t *node)
{
    return document != NULL && node != NULL
        && (node == document->declared_video_card_node
            || node == document->reader_declared_video_card_node);
}

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
    size_t retained_json_script_bytes;
    size_t maximum_json_script_bytes;
    size_t retained_current_script_bytes;
    size_t maximum_inline_script_bytes;
    lxb_dom_node_t *current_script_node;
    lxb_dom_node_t *truncated_script_nodes[64];
    size_t truncated_script_count;
    bool truncated_script_overflow;
    bool discard_inert_script_text;
    bool scripting_enabled;
    bool active;
    bool failed;
    bool input_ended;
    /* Encoding sniffing (HTML 13.2.3): BOM, then the transport charset,
       then a <meta> prescan of the first 1024 bytes, then UTF-8. Bytes are
       never held back: while fewer than 1024 have arrived and all were
       ASCII, a declaration found later in that window still switches the
       decoder. */
    TilefinchDecoder decoder;
    uint8_t transport_encoding;
    bool encoding_decided;
    bool encoding_tentative;
    bool input_ascii;
    size_t encoding_skip;
    unsigned char *prescan_bytes;
    size_t prescan_length;
} DocumentParser;

bool document_parser_begin(DocumentParser *parser, Budget *budget);
/* Select the HTML parsing model before feeding the first byte.  In
   particular, enabled scripting makes <noscript> raw text as required by the
   HTML tree builder instead of exposing its fallback elements to style and
   layout consumers. */
bool document_parser_set_scripting(DocumentParser *parser, bool enabled);
/* When author scripts are disabled, avoid materializing their potentially
   multi-megabyte raw-text payloads in the DOM. A bounded amount of JSON-LD is
   retained for semantic media and Reader discovery. */
bool document_parser_set_inert_script_policy(
    DocumentParser *parser, bool discard, size_t maximum_json_bytes,
    size_t maximum_inline_script_bytes);
bool document_parser_script_was_truncated(
    const DocumentParser *parser, const lxb_dom_node_t *script);
/* Preserve the active HTML tree-builder model while discarding only future
   raw script payload text after navigation has shed optional author work. */
bool document_parser_discard_remaining_script_text(DocumentParser *parser);
/* The Content-Type charset of the response (tilefinch_encoding_from_content_
   type). Must precede the first feed; NONE leaves the decision to the BOM
   and the prescan. */
bool document_parser_set_transport_encoding(DocumentParser *parser,
                                            TilefinchEncoding encoding);
bool document_parser_feed(DocumentParser *parser, const char *data,
                          size_t length);
void document_parser_set_element_closed_callback(
    DocumentParser *parser, DocumentElementClosedCallback callback,
    void *opaque);
/* Deliver EOF to the tree builder without finalizing, so a caller can run a
   last parser mutation checkpoint over the insertions EOF performs while the
   document is still parser-owned. document_parser_finish does this itself
   when the caller has not. */
bool document_parser_end_input(DocumentParser *parser);
bool document_parser_finish(DocumentParser *parser, PocDocument *document);
void document_parser_abort(DocumentParser *parser);

/* Parser-insertion journal (see DocumentParserInsertionJournal). Arming is
   cheap and idempotent; disarming releases the journal. Clearing keeps the
   armed state and capacity for the next checkpoint. Any native destruction
   of a detached subtree must discard it from the journal first. */
void document_parser_insertions_arm(PocDocument *document, bool armed);
bool document_parser_insertions_pending(const PocDocument *document);
void document_parser_insertions_clear(PocDocument *document);
void document_parser_insertions_discard_subtree(PocDocument *document,
                                                lxb_dom_node_t *root);

/* HTML nonce attributes. An element's [[CryptographicNonce]] is its nonce
   attribute until the element is given a slot: by connecting to a document
   whose header-delivered CSP enabled hiding (the attribute then reads as
   empty), or by the nonce IDL setter. An author write to the attribute
   drops the slot again. Elements whose node->user already carries other
   native state, or past the bounded slot table, keep a visible attribute:
   hiding is a confidentiality measure, the CSP check stays correct. */
const char *document_element_nonce(lxb_dom_node_t *element, size_t *length);
bool document_element_set_nonce(PocDocument *document,
                                 lxb_dom_node_t *element, const char *value,
                                 size_t length);
/* Called once a header-delivered policy is known; hides the nonces of
   already connected elements and of every element connected later. */
bool document_nonce_hiding_enable(PocDocument *document);
/* HTML cloning steps: copy each source element's [[CryptographicNonce]]
   to its clone (after Lexbor copied attributes and the caller cleared the
   clone's native state). False when the bounded walk is exceeded. */
bool document_nonce_clone_subtree(lxb_dom_node_t *source,
                                  lxb_dom_node_t *clone, bool deep);

/* Constructed stylesheets (new CSSStyleSheet()) and adoptedStyleSheets.

   A constructed sheet's text lives once, in a detached <style> element the
   script bridge owns and pins while any list adopts it; this registry knows
   those elements, their text revision, and each adopting root's list (the
   document, or a shadow root's native carrier). The stylesheet builders
   parse every active sheet once, after the document's own <style> and
   <link> sources: however many roots adopt a sheet, its rules enter the
   cascade once. Tilefinch's cascade is document-wide (shadow trees are not
   style scopes), so adoption by one root or by fifty is the same cascade.

   The active tier is the adopted sheets of the document and of connected
   shadow roots: lists in the order they were first set, each list in array
   order, a sheet at its first appearance. Bounded: a list holds at most
   DOCUMENT_ADOPTED_SHEETS_PER_ROOT sheets, and the registry at most
   DOCUMENT_ADOPTION_ROOT_LIMIT lists, DOCUMENT_CONSTRUCTED_SHEET_LIMIT
   sheets and DOCUMENT_CONSTRUCTED_TEXT_LIMIT bytes of sheet text; a
   refused registration leaves everything as it was. */
#define DOCUMENT_ADOPTED_SHEETS_PER_ROOT 16u
/* Lit gives every component instance's shadow root its class's sheets, so
   a component page needs one list per live root (MDN: ~100 when its
   budget admits that many roots, DOM_BRIDGE_SHADOW_ROOT_LIMIT). Kept
   within uint8_t for the sorted index; the registry exists only on pages
   that adopt sheets. */
#define DOCUMENT_ADOPTION_ROOT_LIMIT 128u
#define DOCUMENT_CONSTRUCTED_SHEET_LIMIT 128u
#define DOCUMENT_CONSTRUCTED_TEXT_LIMIT (1024u * 1024u)
#define DOCUMENT_ADOPTED_TIER_LIMIT 64u
/* Bytes of parsed form (structural IR plus compiled selector fragment)
   kept for all of a document's constructed sheets together, so a full
   stylesheet rebuild replays them instead of reparsing. Past it a sheet
   keeps part of its form, or none, and is reparsed. */
#define DOCUMENT_CONSTRUCTED_CACHE_LIMIT (640u * 1024u)
/* Records that `sheet` (a detached <style>) now holds `text_bytes` of text,
   registering it on first use; its revision advances. False when a bound
   refuses it (nothing changes). `active` reports whether a connected root
   adopts it, so the cascade must change. */
bool document_constructed_sheet_note_text(PocDocument *document,
                                          lxb_dom_node_t *sheet,
                                          size_t text_bytes, bool *active);
/* Whether `text_bytes` of text for `sheet` would fit the text bound. */
bool document_constructed_sheet_text_fits(const PocDocument *document,
                                          const lxb_dom_node_t *sheet,
                                          size_t text_bytes);
bool document_constructed_sheet_known(const PocDocument *document,
                                      const lxb_dom_node_t *node);
/* Replaces `root`'s list (NULL root: the document) with `sheets`, each
   already registered. An empty list keeps the root registered (see
   document_adoption_root_known). Sheets no list names
   any more are written to `released` (capacity
   DOCUMENT_ADOPTED_SHEETS_PER_ROOT) for the owner to unpin. False when a
   bound refuses the change (nothing changes). */
bool document_adoption_set(PocDocument *document, lxb_dom_node_t *root,
                           lxb_dom_node_t *const *sheets, size_t count,
                           lxb_dom_node_t **released,
                           size_t *released_count);
/* Whether any list adopts `sheet`. */
bool document_constructed_sheet_adopted(const PocDocument *document,
                                        const lxb_dom_node_t *sheet);
/* Whether a list was ever set for shadow-root carrier `root` and the
   carrier still lives: an emptied list stays registered until its
   subtree is destroyed. Compares addresses only. */
bool document_adoption_root_known(const PocDocument *document,
                                  const lxb_dom_node_t *root);
/* Whether `node` is a shadow-root carrier whose list is not empty. */
bool document_adoption_root_has_sheets(const PocDocument *document,
                                       const lxb_dom_node_t *node);
/* Whether any shadow root (not the document) adopts a sheet: a cheap
   guard before per-node document_adoption_root_has_sheets() lookups. */
bool document_adoption_shadow_roots_present(const PocDocument *document);
/* The active tier in cascade order, with each sheet's text revision
   (`revisions` may be NULL). Returns the count, or SIZE_MAX when it does
   not fit `capacity`. */
size_t document_adopted_sheets_active(const PocDocument *document,
                                      lxb_dom_node_t **sheets,
                                      uint32_t *revisions, size_t capacity);
/* The document_adopted_tier_hash fold of one tier entry. The page sheet
   keeps the fold of the entries it parsed (Stylesheet). */
uint64_t document_adopted_tier_hash(uint64_t hash,
                                    const lxb_dom_node_t *sheet,
                                    uint32_t revision);
/* Identity of everything adoption contributes to the cascade: each active
   list's root and sheets with their text revisions, in order. 0 when no
   list is active. */
uint64_t document_adopted_sheets_signature(const PocDocument *document);
/* Before `root`'s subtree is destroyed: forgets the lists of roots inside
   it and the sheets inside it. Sheets no list names any more are written to
   `released` (up to `capacity`; the count is returned) for unpinning. */
size_t document_adoptions_discard_subtree(PocDocument *document,
                                          const lxb_dom_node_t *root,
                                          lxb_dom_node_t **released,
                                          size_t capacity);
/* The adoption lists in cascade order, for scoping: the index-th list's
   root (NULL: the document), sheets and whether it is active (the document,
   or a connected shadow root). False past the last list. */
bool document_adoption_list_at(const PocDocument *document, size_t index,
                               const lxb_dom_node_t **root,
                               lxb_dom_node_t *const **sheets, size_t *count,
                               bool *active);
/* The parsed form cached for `sheet` at text `revision`: borrowed, valid
   until the next registry change. False when there is none. */
bool document_constructed_sheet_cache(const PocDocument *document,
                                      const lxb_dom_node_t *sheet,
                                      uint32_t revision,
                                      const unsigned char **ir,
                                      size_t *ir_bytes,
                                      const unsigned char **fragment,
                                      size_t *fragment_bytes);
/* Copies a freshly built parsed form for `sheet` at `revision` into the
   registry (document-owned, charged to style), replacing any older one;
   within the cache bound, the IR first. A text change drops it. False
   when nothing was kept. A cache, not document content: stylesheet
   builders holding a const document may fill it. */
bool document_constructed_sheet_cache_store(const PocDocument *document,
                                            const lxb_dom_node_t *sheet,
                                            uint32_t revision,
                                            const unsigned char *ir,
                                            size_t ir_bytes,
                                            const unsigned char *fragment,
                                            size_t fragment_bytes);
/* Counts a parse of `sheet`'s text at `revision`; true from the second
   parse on, when caching its parsed form starts to pay. */
bool document_constructed_sheet_note_parse(const PocDocument *document,
                                           const lxb_dom_node_t *sheet,
                                           uint32_t revision);
size_t document_constructed_sheet_cache_bytes(const PocDocument *document);
/* Registered constructed sheets (diagnostics and tests). */
size_t document_constructed_sheet_count(const PocDocument *document);
void document_adopted_sheets_destroy(PocDocument *document);

/* Scope raw Lexbor mutations to the document that owns the resulting DOM
   allocations. Callers which mutate parser.document outside parser APIs must
   use this pair; normal script/controller paths scope this automatically. */
BudgetAllocationOwner document_allocation_owner_enter(
    const PocDocument *document);
void document_allocation_owner_leave(const PocDocument *document,
                                     BudgetAllocationOwner previous_owner);

/* Host-side style inputs.
 *
 * Script DOM writes reach style caches as the bridge's mutation notes. Every
 * other DOM change (parser, controller, reader mode, native media cards, ...)
 * advances this process-wide generation instead: page documents route their
 * Lexbor insertion, removal and attribute callbacks through it, parser input
 * advances it per fed chunk, and a host input that changes computed style
 * without touching the DOM calls document_style_changed() directly. Caches
 * that outlive one script entry key on it. Monotonic; only equality means
 * "nothing changed". */
uint64_t document_style_generation(void);
void document_style_changed(void);
/* True while no <iframe> or <frame> can be in any of `document`'s trees
   (connected, detached or template contents): every element is inserted
   somewhere when created, and page documents count frame-container
   insertions process-wide, so none has been inserted anywhere since the
   document's parse began. A document whose insertions are not counted
   answers false. Lets frame walks skip pages without frames. */
bool document_frames_impossible(const PocDocument *document);
/* Lexbor writes between these do not advance the generation: the script
   bridge journals its own writes as mutation notes, and a probe that
   restores what it changed leaves nothing to see. Nesting is allowed. An
   unbracketed write is still correct, only conservative. */
void document_style_quiet_begin(void);
void document_style_quiet_end(void);
/* Called for every node Lexbor removes from its parent, including each node
   of a subtree being destroyed, whatever the cause. A style cache keyed by
   node address evicts the node here, before the address can be reused.
   Bounded: registration fails when every slot is taken. */
typedef void (*DocumentNodeRemovalListener)(void *opaque,
                                            const lxb_dom_node_t *node);
bool document_node_removal_listen(DocumentNodeRemovalListener listener,
                                  void *opaque);
void document_node_removal_unlisten(DocumentNodeRemovalListener listener,
                                    void *opaque);

bool document_parse(PocDocument *document, Budget *budget,
                    const char *html, size_t html_length, size_t chunk_size);
/* document_parse with the response's Content-Type charset (see
   document_parser_set_transport_encoding). */
bool document_parse_with_transport_encoding(
    PocDocument *document, Budget *budget, const char *html,
    size_t html_length, size_t chunk_size, TilefinchEncoding transport);
bool document_refresh(PocDocument *document);
/* When a bounded script pass cannot start authored client-side UI, preserve
   useful static content: materialize a server-authored header navigation or
   replace a loading-only metadata-rich body with a small summary. Returns
   true only when it changed the connected document. */
bool document_install_static_shell_fallback(PocDocument *document);
void document_note_connected_mutation(PocDocument *document);
/* A connected attribute value changed (setAttribute, removeAttribute, an
   inline style write). No count, text or title statistic can change, so
   callers need not refresh the document: this folds the name's presence
   flags in conservatively and marks the attribute totals stale. */
void document_note_attribute_mutation(PocDocument *document,
                                      const char *name, size_t length);
/* Recompute attribute_count/attribute_value_bytes if they are stale. */
void document_refresh_attribute_totals(PocDocument *document);
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
/* The same bounded body with its <script> elements left out: what a page
   whose realm has been retired can still show (memory rescue). */
bool document_body_snapshot_capture_without_scripts(
    PocDocument *document, DocumentBodySnapshot *snapshot);
/* The server text and actions under <body>, outside script, style,
   template, svg and noscript: what a reader sees before author script has
   built anything. One allocation-free walk of at most node_limit nodes
   (truncated when it stopped there) serves every "does the server page
   have content" question: heavy pages, the body snapshot and memory
   rescue. Each caller keeps its own threshold. */
typedef struct {
    /* Non-space text bytes, and text bytes with each whitespace run
       counted once. */
    size_t text_bytes;
    size_t collapsed_text_bytes;
    /* Links with an href, buttons, inputs, selects, textareas and forms. */
    size_t action_count;
    bool truncated;
} DocumentVisibleContent;
#define DOCUMENT_SNAPSHOT_VISIT_LIMIT 4096u
DocumentVisibleContent document_body_visible_content(
    const PocDocument *document, size_t node_limit);
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
/* An HTML <meta http-equiv=refresh> (ASCII case-insensitive). */
bool document_is_refresh_meta(lxb_dom_node_t *node);
/* CSP distinguishes authored/setAttribute style text from declarations made
   through the CSSOM. Lexbor attribute nodes provide a stable, clone-safe
   provenance slot without adding a page-sized side table. */
bool document_style_attribute_cssom_authorized(lxb_dom_node_t *node);
void document_style_attribute_set_cssom_authorized(lxb_dom_node_t *node,
                                                    bool authorized);
/* Shadow-root carriers: the display:contents element the script bridge
   appends to a shadow host to hold its shadow tree. Only native code can
   mark one. Layout walks the flat tree (document_flat_first_child below);
   a light child the flat tree leaves out (no slot is assigned it, or its
   slot does not render because it or a shadow ancestor is `hidden`) is
   also resolved display:none, so no path gives it a box. */
bool document_mark_shadow_carrier(lxb_dom_node_t *node);
bool document_node_is_shadow_carrier(const lxb_dom_node_t *node);
lxb_dom_node_t *document_shadow_carrier_of_host(const lxb_dom_node_t *host);
/* The nearest carrier at or above `node` (bounded walk), or NULL. */
lxb_dom_node_t *document_shadow_carrier_containing(
    const lxb_dom_node_t *node);
bool document_shadow_light_child_rendered(const lxb_dom_node_t *carrier,
                                          lxb_dom_node_t *child);
/* False for a host's light text child the flat tree leaves out. */
bool document_shadow_text_rendered(lxb_dom_node_t *text);
/* Flat-tree traversal (DOM 4.2.2.3 / CSS Scoping 2): a shadow host's
   children are its shadow tree (the carrier), a slot's are its assigned
   light nodes (else its fallback content), and a light child's parent is
   its slot. Plain DOM order on pages without shadow trees. Layout walks
   boxes in this order, so slotted content lays out at its slot. */
bool document_shadow_trees_present(void);
lxb_dom_node_t *document_flat_first_child(lxb_dom_node_t *node);
lxb_dom_node_t *document_flat_next_sibling(lxb_dom_node_t *node);
lxb_dom_node_t *document_flat_parent(lxb_dom_node_t *node);
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
