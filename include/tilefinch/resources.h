#ifndef TILEFINCH_RESOURCES_H
#define TILEFINCH_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"

typedef struct FetchScheduler FetchScheduler;
typedef struct ImagePriorityLoadJob ImagePriorityLoadJob;
struct LayoutReuseCache;

#define STYLESHEET_DOCUMENT_RESOURCE_LIMIT 32
#define STYLESHEET_TRANSIENT_ATTEMPT_LIMIT 3
#define STYLESHEET_REFERRER_POLICY_LIMIT 40
#define STYLESHEET_ALTERNATE_THEME_LIMIT 16

typedef enum {
    STYLESHEET_DOCUMENT_RESOURCE_EMPTY = 0,
    STYLESHEET_DOCUMENT_RESOURCE_TRANSIENT_FAILURE,
    STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE,
    STYLESHEET_DOCUMENT_RESOURCE_LOADED
} StylesheetDocumentResourceState;

/* A stylesheet response belongs to the document which requested it, not to
   an individual parser-blocking script checkpoint.  Keeping this small
   page-lifetime ledger prevents every subsequent closing script tag from
   refetching a failed URL, and pins successful immutable bodies even when
   the shared HTTP cache has to evict its own reference. */
typedef struct {
    char *url;
    char *response_url;
    char response_referrer_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
    BrowserSharedBody *body;
    size_t length;
    size_t attempts;
    StylesheetDocumentResourceState state;
    bool final_retry_granted;
    /* True once this response has contributed rules to the sheet.  A
       media-mismatched or failed earlier reference may be promoted by a
       later matching duplicate during suffix continuation. */
    bool rules_applied;
    /* Both the final response URL and the response's normalized
       Referrer-Policy are known.  Retained CSS is never reapplied without
       this pair because request-key/document-policy fallbacks can leak or
       resolve descendants against the wrong stylesheet. */
    bool response_provenance_known;
    /* Speculative responses are reusable only by a link with the same fetch
       mode and credentials.  In particular, a no-CORS preload must never
       satisfy a later crossorigin stylesheet request. */
    bool cors_validated;
    TilefinchCredentialsMode credentials;
    /* A style preload charged this response to the current pass's preload
       lane; the first active link that replays it takes over the charge. */
    bool preload_charged;
    size_t preload_charge;
    /* The response reached its byte cap; only its complete rules (the
       stylesheet_complete_rules_prefix of the received bytes) are retained
       and applied. */
    bool truncated;
} StylesheetDocumentResource;

typedef struct {
    Budget *budget;
    StylesheetDocumentResource items[STYLESHEET_DOCUMENT_RESOURCE_LIMIT];
    /* Completion belongs to the link, not just its shared response URL:
       media and integrity can admit one preload while refusing another. */
    struct {
        lxb_dom_node_t *element;
        StylesheetDocumentResourceState state;
    } style_preloads[STYLESHEET_DOCUMENT_RESOURCE_LIMIT];
    size_t style_preload_count;
    size_t count;
    size_t retained_body_hits;
    size_t transient_retries;
    size_t transient_failures;
    size_t terminal_failures;
    size_t retry_suppressed;
    size_t final_retry_grants;
    size_t pressure_serializations;
    /* Responses cut at their byte cap and applied up to their last complete
       rule, with the bytes received and the bytes kept. */
    size_t truncated;
    size_t truncated_received_bytes;
    size_t truncated_applied_bytes;
    /* A generated theme registry can be discovered during the viewport-first
       pass and referenced by href links in a later parser continuation. Keep
       the bounded selection beside the page-lifetime response ledger so each
       continuation applies the same author choice and resource budget. */
    uint64_t alternate_theme_hashes[STYLESHEET_ALTERNATE_THEME_LIMIT];
    bool alternate_theme_active[STYLESHEET_ALTERNATE_THEME_LIMIT];
    size_t alternate_theme_count;
    bool alternate_theme_selection_valid;
    /* Parser checkpoints may have to sample a large utility sheet before
       the body exists. The authoritative pass flips this fact, then rebuilds
       once from retained bodies if an early sample was taken. */
    bool selector_census_complete;
    bool final_resample_required;
    bool final_resample_completed;
} StylesheetDocumentResources;

typedef struct {
    size_t discovered;
    size_t attempted;
    size_t loaded;
    size_t failed;
    size_t skipped_limit;
    /* Source bytes whose parse working set would consume the layout reserve.
       These are deliberately omitted from the cascade rather than turning an
       optional author sheet into a page-level allocation failure. */
    size_t skipped_pressure;
    size_t skipped_media;
    /* Declarative theme registries can point at several mutually-exclusive
       sheets. Only the document-selected variant consumes the PSP's bounded
       stylesheet count, byte, and connection budgets. */
    size_t skipped_alternate_theme;
    size_t duplicate;
    size_t cache_hits;
    size_t compiled_fragment_hits;
    size_t compiled_fragment_misses;
    size_t compiled_fragment_stores;
    size_t compiled_fragment_rules_reused;
    size_t compiled_fragment_bytes;
    size_t parsed_ir_hits;
    size_t parsed_ir_misses;
    size_t parsed_ir_stores;
    size_t parsed_ir_operations_reused;
    size_t parsed_ir_bytes;
    size_t retained_body_hits;
    size_t transient_retries;
    size_t transient_failures;
    size_t terminal_failures;
    size_t retry_suppressed;
    size_t final_retry_grants;
    size_t pressure_serializations;
    size_t truncated;
    size_t truncated_received_bytes;
    size_t truncated_applied_bytes;
    size_t imports_discovered;
    size_t imports_loaded;
    size_t imports_skipped_conditions;
    size_t imports_skipped_depth;
    size_t bytes;
    /* <link rel=preload as=style> responses: a separate, smaller lane so
       optional hints never refuse an active stylesheet. A matching active
       link moves the charge to attempted/bytes. */
    size_t preload_attempted;
    size_t preload_bytes;
    size_t rules_added;
    size_t variables_added;
    size_t batches;
    size_t first_batch_loaded;
    size_t deadline_cancelled;
    bool deadline_exceeded;
    uint64_t elapsed_ms;
    uint64_t max_slice_us;
    size_t work_units;
    size_t max_slice_work_units;
    size_t cooperative_yields;
} ExternalStylesheetStats;

typedef struct {
    size_t declarations_discovered;
    size_t sources_discovered;
    size_t attempted;
    size_t loaded_faces;
    size_t failed;
    size_t unsupported;
    size_t skipped_limit;
    size_t duplicate_sources;
    size_t cache_hits;
    size_t encoded_bytes;
    size_t retained_encoded_bytes;
    size_t deadline_cancelled;
    bool deadline_exceeded;
    uint64_t elapsed_ms;
} ExternalFontStats;

/* One-request, page-local continuation for fallback-first web fonts.  The
   fetch scheduler owns request strings and response storage; this cursor
   retains no encoded body and is therefore fixed-size regardless of page
   complexity. */
typedef struct {
    size_t next_source;
    size_t pending_source;
    uint64_t request_id;
    double started_ms;
    double deadline_ms;
    bool active;
} ExternalFontLoader;

typedef struct ImageCanvasNativeSurface {
    lxb_dom_node_t *node;
    const unsigned char *pixels;
    size_t stride;
    uint32_t epoch;
    bool authoritative;
} ImageCanvasNativeSurface;

typedef struct ImageResource {
    lxb_dom_node_t *node;
    uint64_t url_hash;
    /* Hash of the authored source token. Unlike url_hash this distinguishes
       multiple CSS paint layers attached to the same element while keeping
       the retained resource fixed-size. */
    uint64_t source_hash;
    unsigned char *pixels;
    /* Non-NULL when pixels are an immutable session-cache lease. The one
       resource with owns_pixels releases this body; aliases merely borrow. */
    BrowserSharedBody *pixel_body;
    unsigned char *encoded;
    BrowserSharedBody *encoded_body;
    size_t encoded_length;
    int source_width;
    int source_height;
    int width;
    int height;
    bool is_mask;
    bool is_background;
    /* Mutable page-authored canvas snapshots use the same bounded decoded
       surface path as images, but are never refetched or colour-remapped. */
    bool is_canvas;
    /* WebGL advertises alpha:false and both native backends force every
       resolved pixel opaque. This lets publication avoid rescanning the
       complete RGBA surface on every animation frame. Canvas 2D leaves the
       bit clear because its backing may contain transparency. */
    bool canvas_opaque;
    /* A PSP WebGL surface may remain authoritative in EDRAM until normal
       painting consumes it. The page-owned RGBA allocation remains the
       bounded fallback/readback store; this borrowed pointer is never freed
       by ImageResources and is trusted only for its matching display epoch. */
    ImageCanvasNativeSurface *canvas_native_surface;
    /* A no-CORS image whose final response origin differs from the owning
       document may be painted, but it taints any canvas that consumes it. */
    bool cross_origin;
    PseudoElement pseudo;
    bool owns_pixels;
    bool owns_encoded;
    /* Set only inside a refresh's replacement table: the pixels are an
       unchanged inline-SVG raster borrowed from the outgoing table. The
       refresh commit verifies the lender is still present, hands ownership
       over when the lender retires, and clears the bit. */
    bool borrows_previous;
    /* Display retargeting (src/image_retarget.c) once layout knows the
       painted size: the decoded size before the first retarget (zero until
       then; the surface never grows past it) and IMAGE_RETARGET_* state. */
    uint8_t retarget_flags;
    int full_width;
    int full_height;
} ImageResource;

/* The surface was reduced to its painted size. */
#define IMAGE_RETARGET_SHRUNK UINT8_C(1)
/* It was restored after a reduction; it is never reduced again, so
   alternating responsive layouts cost at most one reduction and one decode. */
#define IMAGE_RETARGET_GROWN UINT8_C(2)
/* Unpainted: the pixels were released and the encoded bytes kept. */
#define IMAGE_RETARGET_DROPPED UINT8_C(4)
/* Page script read the pixels (canvas drawImage, WebGL texImage2D); they
   stay at full resolution. */
#define IMAGE_RETARGET_PINNED UINT8_C(8)
/* A decode failed for good; the surface is left alone. */
#define IMAGE_RETARGET_REFUSED UINT8_C(16)
/* The encoded bytes are SVG markup (a response, a data: body, a referenced
   symbol or an inline <svg> serialization): decoding rasterizes them at the
   resource's width x height, which may be above its first raster's size. */
#define IMAGE_RETARGET_VECTOR UINT8_C(32)

typedef enum {
    IMAGE_CANVAS_COMMIT_REFUSED = 0,
    IMAGE_CANVAS_COMMIT_CREATED,
    IMAGE_CANVAS_COMMIT_UPDATED,
    IMAGE_CANVAS_COMMIT_RESIZED
} ImageCanvasCommitResult;

typedef struct {
    size_t discovered;
    size_t attempted;
    /* Attempts the image-count cap does not charge: small rasters that never
       touch the network (inline-SVG icons, short data: URLs). They remain in
       `attempted` and are still charged to the decoded/encoded byte quotas;
       the count cap applies to attempted - count_exempt. */
    size_t count_exempt;
    size_t loaded;
    size_t failed;
    size_t unsupported;
    /* "name.jpg.webp"-style URLs rewritten to the original they name
       ("name.jpg"); other WebP URLs are fetched and decoded as written. */
    size_t compatible_format_rewrites;
    size_t skipped_limit;
    /* A document-wide continuation may fail after the first viewport has
       already loaded.  The loader retains that committed prefix and reports
       the bounded fallback here instead of discarding useful resources. */
    size_t priority_retained_on_failure;
    size_t duplicate;
    size_t cache_hits;
    size_t decoded_cache_hits;
    size_t encoded_bytes;
    size_t decoded_bytes;
    size_t downsampled;
    size_t largest_source_decode_bytes;
    size_t largest_target_decode_bytes;
    /* Measured: the largest working set (decoder allocations plus output)
       of one raster decode. */
    size_t largest_decode_working_bytes;
    size_t masks_loaded;
    size_t backgrounds_loaded;
    size_t deadline_cancelled;
    bool deadline_exceeded;
    uint64_t elapsed_ms;
    size_t fetch_failures_http_4xx;
    size_t fetch_failures_http_5xx;
    size_t fetch_failures_timeout;
    size_t fetch_failures_cancelled;
    size_t fetch_failures_quota;
    size_t fetch_failures_transport;
    size_t progress_samples;
    size_t progress_events;
    size_t progress_bytes;
    size_t stalled_polls;
    size_t no_progress_cancelled;
    size_t no_progress_origin_cooldowns;
    size_t no_progress_origin_skipped;
    uint64_t maximum_no_progress_ms;
    uint64_t maximum_request_ms;
    uint64_t max_slice_us;
    uint64_t traversal_us;
    uint64_t style_resolve_us;
    uint64_t node_style_resolve_us;
    uint64_t pseudo_style_resolve_us;
    size_t node_style_cache_hits;
    size_t node_style_cache_misses;
    /* Elements whose ::before/::after candidates cannot supply an image and
       whose pseudo-element styles were therefore not resolved. */
    size_t discovery_prefilter_skips;
    size_t discovery_prefilter_checks;
    uint64_t admission_us;
    uint64_t admission_resolve_us;
    uint64_t admission_cache_us;
    uint64_t admission_context_us;
    uint64_t admission_enqueue_us;
    uint64_t scheduler_us;
    uint64_t finish_us;
    uint64_t drain_us;
    size_t work_units;
    size_t max_slice_work_units;
    size_t cooperative_yields;
    /* Inline <svg> rasterizations performed, and those a mutation-driven
       refresh satisfied from the outgoing table's identical raster. The
       timings are collected with the image profile. */
    size_t inline_svg_rasterized;
    size_t inline_svg_refresh_reused;
    uint64_t inline_svg_serialize_us;
    uint64_t inline_svg_rasterize_us;
} ExternalImageStats;

typedef struct {
    Budget *budget;
    ImageResource *items;
    size_t count;
    size_t capacity;
    struct {
        lxb_dom_node_t *node;
        uint16_t *values;
        int width;
        int height;
    } canvas_depth[2];
    ImageCanvasNativeSurface canvas_native[2];
    ExternalImageStats stats;
    /* Display retargeting (src/image_retarget.c), never rolled back with
       the loader's stats. */
    struct {
        size_t scans;
        size_t shrinks;
        size_t grows;
        size_t drops;
        /* SVG markup rasterized again (a reduction or a restore). */
        size_t rasters;
        size_t deferred;
        size_t released_bytes;
        size_t restored_bytes;
        uint64_t us;
    } retarget;
    /* Bounded, generation-keyed display-size scan, owned by this table. */
    struct ImageRetargetPlan *retarget_plan;
    bool priority_staged;
} ImageResources;

/* Alias insertion can outgrow the bounded resource table after a layout has
   retained pointers into it. Keep the previous table alive until that layout
   has rebound its pointers, then release only the table allocation (the
   copied ImageResource ownership remains with ImageResources.items). */
typedef struct {
    ImageResource *previous_items;
    size_t previous_count;
    size_t aliased;
} ImageAliasResult;

#define IMAGE_PRIORITY_KIND_DOCUMENT UINT8_C(0)
#define IMAGE_PRIORITY_KIND_MASK UINT8_C(1)
#define IMAGE_PRIORITY_KIND_BACKGROUND UINT8_C(2)

/* Exact resource identity retained by a transient bounded layout.  CSS
   source pointers are interned in the page stylesheet; document-image
   targets leave source NULL so srcset selection remains the image loader's
   responsibility. */
typedef struct {
    lxb_dom_node_t *node;
    const char *source;
    /* Optional native weak handle for a mutable page's retained idle queue.
       Zero denotes the existing immutable/static-layout contract. */
    long weak_handle;
    /* Authoritative visual-pixel box from a bounded layout. Zero means the
       loader must retain its ordinary viewport/source policy. */
    uint16_t display_width;
    uint16_t display_height;
    uint8_t kind;
    uint8_t pseudo;
    /* Continuation-local failure state travels with the target when the
       remaining queue is re-ranked after a scroll. These bytes occupy the
       structure's existing alignment tail on both PSP and host builds. */
    uint8_t retry_count;
    uint8_t retry_delay;
} ImagePriorityTarget;

_Static_assert(
    sizeof(ImagePriorityTarget) == 2u * sizeof(void *) + sizeof(long) + 8u,
    "deferred image targets retain one bounded native weak identity");

typedef enum {
    IMAGE_PRIORITY_LOAD_PENDING = 0,
    IMAGE_PRIORITY_LOAD_COMPLETE,
    IMAGE_PRIORITY_LOAD_FAILED
} ImagePriorityLoadStatus;

#define IMAGE_PRIORITY_LOAD_BATCH_LIMIT 2u

static inline int image_resource_intrinsic_width(const ImageResource *image)
{
    return image == NULL ? 0
        : (image->source_width > 0 ? image->source_width : image->width);
}

static inline int image_resource_intrinsic_height(const ImageResource *image)
{
    return image == NULL ? 0
        : (image->source_height > 0 ? image->source_height : image->height);
}

typedef enum {
    IMAGE_DECODE_SUCCEEDED = 0,
    IMAGE_DECODE_DETERMINISTIC_FAILURE,
    IMAGE_DECODE_TRANSIENT_FAILURE
} ImageDecodeStatus;

/* Upper estimate of the Budget bytes that parsing `css` adds to a sheet,
   including its share of the rule index built on first use. Stylesheet
   admission compares it, plus a layout reserve, with the remaining Budget.
   Computed by one allocation-free pass over the text. */
size_t stylesheet_source_cost_estimate(const char *css, size_t length);

/* For a response truncated at its byte cap: the length of the prefix that
   ends with the last complete top-level rule, at-rule block or statement.
   Anything after it (a half rule, an unclosed @media/@supports block, an
   unterminated string or comment) is unfinished and must not be parsed. */
size_t stylesheet_complete_rules_prefix(const char *css, size_t length);
/* The scanner behind it, one top-level statement at a time: true with
   *offset moved just past the next complete statement, or false when the
   input ends first, with *open_blocks (optional) set to the number of blocks
   the unfinished tail left open. */
bool stylesheet_next_statement(const char *css, size_t length,
                               size_t *offset, size_t *open_blocks);

bool stylesheets_load_external(const PocDocument *document, Stylesheet *sheet,
                               Budget *budget, const char *base_url,
                               size_t maximum_count,
                               size_t maximum_total_bytes,
                               size_t maximum_single_bytes,
                               long timeout_ms,
                               FetchScheduler *scheduler,
                               BrowserSession *session,
                               ExternalStylesheetStats *stats);
bool stylesheets_load_external_tracked(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats);
/* Context-preserving variants separate the document's resolution base from
   its actual URL/security context.  External sheets and imports retain their
   own final response URL and normalized response Referrer-Policy for child
   CSS requests. */
bool stylesheets_load_external_with_context(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, const char *document_url,
    const char *document_referrer_policy, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    ExternalStylesheetStats *stats);
bool stylesheets_load_external_tracked_with_context(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, const char *document_url,
    const char *document_referrer_policy, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats);
/* Extends an already ordered author sheet with a newly parsed source-order
   suffix. Unlike the full loader this never resets prefix rules; links,
   inline blocks, and @imports still share the ordinary bounded fetch and
   document-resource machinery. */
bool stylesheets_append_ordered_suffix_with_context(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats);
/* Fetches and settles style preload links (rel=preload as=style, every
   node) through the same ledger, so their load or error event can be
   dispatched; the sheet's rules are untouched. */
bool stylesheets_settle_style_preloads_with_context(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats);
void stylesheet_document_resources_destroy(
    StylesheetDocumentResources *resources);
/* Retains one successfully fetched speculative stylesheet for the ordinary
   ordered loader. The caller supplies normalized response provenance; this
   function never parses or applies CSS. */
bool stylesheet_document_resources_retain(
    StylesheetDocumentResources *resources, const char *request_url,
    const char *response_url, const char *response_referrer_policy,
    struct BrowserSharedBody *body, size_t length, bool cors_validated,
    TilefinchCredentialsMode credentials);
/* Once the top-level response has left the transport, allow a URL which
   exhausted its transient parser-time attempts one last bounded retry. */
void stylesheet_document_resources_open_final_retry(
    StylesheetDocumentResources *resources);
/* Marks the current DOM as complete. Returns true only when an early large-
   sheet sample requires one ordered rebuild; all successfully retained
   sources are made eligible so that rebuild preserves cascade order. */
bool stylesheet_document_resources_prepare_complete_census(
    StylesheetDocumentResources *resources);
/* Whether the sheet a link's href names has had its rules applied (the
   ordered loaders apply one URL once; a later reference is a duplicate). */
bool stylesheet_document_resources_link_applied(
    const StylesheetDocumentResources *resources, const char *base_url,
    const char *href, size_t href_length);
/* Resolve a link's bounded href against its document base and return the
   transport ledger state. Present-but-invalid href values are deterministic
   terminal failures, allowing DOM event dispatchers to settle them without
   inventing a second URL parser. A missing href remains EMPTY. */
StylesheetDocumentResourceState stylesheet_document_resources_link_state(
    const StylesheetDocumentResources *resources, const char *base_url,
    const char *href, size_t href_length);

/* Loads at most one regular and one bold face per bounded stylesheet family.
   Page font failure is always nonfatal: CSS falls back to its encoded generic
   family. Requests use anonymous CORS semantics and deliberately bypass the
   generic HTTP cache until it can retain font-CORS and redirect provenance.
   Successfully decoded faces remain page-local for repeated layout/raster. */
bool fonts_load_external(Stylesheet *sheet, Budget *budget,
                         const char *document_base_url,
                         const char *document_url,
                         const char *referrer_policy,
                         size_t maximum_attempts,
                         size_t maximum_total_encoded_bytes,
                         size_t maximum_single_encoded_bytes,
                         size_t maximum_face_backend_bytes,
                         long timeout_ms, FetchScheduler *scheduler,
                         BrowserSession *session,
                         const TilefinchContentSecurityPolicy *csp,
                         ExternalFontStats *stats);
/* Initializes discovery without issuing network traffic.  Each subsequent
   step enqueues or settles at most one request and may decode at most one
   face.  This lets navigation paint generic fallback text before font I/O,
   then relayout only after a usable face arrives. */
bool fonts_external_loader_begin(ExternalFontLoader *loader,
                                 Stylesheet *sheet, long timeout_ms,
                                 ExternalFontStats *stats);
bool fonts_external_loader_step(
    ExternalFontLoader *loader, Stylesheet *sheet, Budget *budget,
    const char *document_base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, FetchScheduler *scheduler,
    BrowserSession *session, const TilefinchContentSecurityPolicy *csp,
    ExternalFontStats *stats,
    unsigned maximum_wait_ms, bool *face_loaded);
void fonts_external_loader_cancel(ExternalFontLoader *loader,
                                  FetchScheduler *scheduler,
                                  ExternalFontStats *stats);
bool fonts_external_loader_pending(const ExternalFontLoader *loader);

bool images_load_external(const PocDocument *document, Stylesheet *stylesheet,
                          ImageResources *images,
                          Budget *budget, const char *base_url,
                          const char *document_url,
                          const char *referrer_policy,
                          size_t maximum_count,
                          size_t maximum_total_encoded_bytes,
                          size_t maximum_single_encoded_bytes,
                          size_t maximum_decoded_bytes,
                          long timeout_ms, FetchScheduler *scheduler,
                          BrowserSession *session);
/* Shares the retained computed-style cache used by provisional and
   authoritative layout. Image discovery can therefore consume preview work
   and seed the final layout instead of resolving the same cascade twice.
   The ordinary entry point remains available to embedders without layout
   retention. */
bool images_load_external_reusing_layout_styles(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    struct LayoutReuseCache *style_cache, const FontSet *fonts,
    int viewport_width);
/* Same complete CSS/SVG traversal, optionally leaving ordinary <img> loads
   to an owner-supplied weak-handle queue. Does not defer stylesheet images. */
bool images_load_external_reusing_layout_styles_deferred(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    struct LayoutReuseCache *style_cache, const FontSet *fonts,
    int viewport_width, bool defer_document_images);
/* images_destroy + images_load_external_reusing_layout_styles_excluding in
   one step, except that an inline SVG whose serialized markup is unchanged
   takes over its outgoing raster instead of being rasterized again. On
   failure the table is left destroyed, as the two-step form would. */
bool images_rebuild_external_reusing_rasters(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    struct LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    lxb_dom_node_t *const *deferred_nodes, size_t deferred_node_count);
/* Additive discovery excludes at most 128 live deferred targets. New nodes,
   CSS and SVG retain the authoritative traversal. Exclusion pointers are
   borrowed for this synchronous call and never dereferenced by the filter. */
bool images_load_external_reusing_layout_styles_excluding(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    struct LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    lxb_dom_node_t *const *deferred_nodes, size_t deferred_node_count);
/* Loads a bounded set of nodes already proven visible by provisional layout.
   A subsequent images_load_external() call remains authoritative and
   deduplicates these retained resources while discovering the rest of the
   document. */
bool images_load_external_priority_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes, size_t maximum_single_encoded_bytes,
    size_t maximum_decoded_bytes, long timeout_ms, FetchScheduler *scheduler,
    BrowserSession *session);
/* Transactionally refreshes exact resource-bearing nodes, including <img>,
   inline SVG, and CSS image/mask owners. Fetch/decode runs against a
   temporary table while the committed layout keeps its old surfaces; only a
   completed refresh swaps ownership into the page table. Per-image
   network/decode failures are soft and remove the stale surface, while
   structural allocation failure leaves the original table untouched and
   returns false. */
bool images_refresh_external_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session);
/* Same transactional refresh, sharing the owner's already-invalidated style
   cache with additive discovery and final layout. Uses their canonical html
   inheritance root. The caller must invalidate mutation-dependent entries
   before calling, exactly as for complete image discovery. */
bool images_refresh_external_nodes_reusing_layout_styles(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    struct LayoutReuseCache *style_cache, const FontSet *fonts,
    int viewport_width);
/* Allocation-free rollback for trusted native DOM transactions. Removes
   resources owned by the exact nodes, transferring shared backing ownership
   to retained aliases before releasing anything. */
void images_discard_nodes(
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count);
bool images_load_external_priority_targets(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *targets,
    size_t target_count, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session);
/* One externally pumped document-image load. Admission, transport, and one
   decode completion are separate bounded calls, so an idle-work owner never
   waits for the network on the browser thread. The target and all URL/context
   pointers are borrowed from the page and must outlive the job. */
ImagePriorityLoadJob *images_priority_load_begin(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *target,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session);
/* The same ownership transaction may admit two already-proven-visible
   targets. Both share one rollback snapshot; the first completed response is
   still decoded and publishable while transport advances the second. */
ImagePriorityLoadJob *images_priority_load_begin_batch(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *targets,
    size_t target_count, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session);
ImagePriorityLoadStatus images_priority_load_pump(
    ImagePriorityLoadJob *job);
/* Caller must first prove target node liveness. Checks src/srcset selection
   only after the document mutation generation changes. */
bool images_priority_load_sources_current(ImagePriorityLoadJob *job);
/* Advance the job's rollback boundary after the owner has successfully
   rebuilt layout around newly decoded pixels. */
void images_priority_load_commit_progress(ImagePriorityLoadJob *job);
size_t images_priority_load_target_count(const ImagePriorityLoadJob *job);
bool images_priority_load_contains_node(
    const ImagePriorityLoadJob *job, const lxb_dom_node_t *node);
void images_priority_load_destroy(ImagePriorityLoadJob *job);
/* Stop the process-wide PSP JPEG worker before its owning Budget is torn
   down. Navigation cancellation itself never waits for the worker: a stale
   completion is discarded by generation on a later browser-thread pump. */
bool images_decode_worker_shutdown(Budget *budget);
/* Browser-thread reap for a navigation-cancelled completion. False means a
   lower-priority decode is still running; callers never wait for it. */
bool images_decode_worker_reap_cancelled(Budget *budget);
/* Whether an inline <svg> draws anything itself: a shape, text, image,
   <use> or foreignObject outside the non-rendering containers (<defs>,
   <symbol>, gradients, patterns, clip paths, masks, markers, filters,
   metadata). Symbol and definition content renders only through <use>.
   Visits at most `visit_limit` nodes; when that bound cuts the walk short
   `*bounded_out` is set and the answer is not a proof either way. */
bool image_inline_svg_draws_content(lxb_dom_node_t *svg, size_t visit_limit,
                                    bool *bounded_out);
const ImageResource *images_find_node(const ImageResources *images,
                                      const lxb_dom_node_t *node);
const ImageResource *images_find_mask_node(const ImageResources *images,
                                           const lxb_dom_node_t *node);
const ImageResource *images_find_background_node(const ImageResources *images,
                                                 const lxb_dom_node_t *node);
const ImageResource *images_find_background_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo);
const ImageResource *images_find_mask_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo);
const ImageResource *images_find_pseudo_mask(const ImageResources *images,
                                             const lxb_dom_node_t *node,
                                             PseudoElement pseudo);
const ImageResource *images_find_pseudo_background(
    const ImageResources *images, const lxb_dom_node_t *node,
    PseudoElement pseudo);
/*
 * Select the responsive image candidate used by the loader without starting
 * a request. The returned slice is owned by the document DOM and remains
 * valid until that attribute or document is mutated/destroyed.
 */
const char *image_select_source(const Stylesheet *stylesheet,
                                lxb_dom_node_t *node, size_t *length);
/* Associate newly-created document image nodes with an already-admitted
   resource from the same committed document. This alias-only operation
   resolves authored sources with the original document provenance, but never
   fetches, decodes, or duplicates pixels. Traversal and aliases are bounded;
   aliases borrow the backing through ImageResources rather than retaining a
   source DOM node. */
ImageAliasResult images_alias_existing_document_subtree(
    const Stylesheet *stylesheet, ImageResources *images,
    lxb_dom_node_t *root, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_aliases);
void images_alias_result_release(ImageResources *images,
                                 ImageAliasResult *result);
/*
 * Adopt one already-decoded RGBA surface as a replaced-element resource.
 * This is the generic handoff used by media backends: the producer retains
 * no ownership after success, and may update the fixed allocation in place
 * while invalidating affected render tiles. Geometry and allocation identity
 * must remain stable for the lifetime of the resource.
 */
bool images_adopt_decoded_surface(ImageResources *images, Budget *budget,
                                  lxb_dom_node_t *node,
                                  unsigned char *rgba_pixels,
                                  int width, int height);
/*
 * Replace a document image already associated with node, if any, with a
 * mutable decoded surface.  Video uses this to hand off from its poster to
 * the first decoded frame without retaining both allocations or making a
 * poster prevent playback.  Ownership transfers only on success.
 */
bool images_replace_with_decoded_surface(ImageResources *images,
                                         Budget *budget,
                                         lxb_dom_node_t *node,
                                         unsigned char *rgba_pixels,
                                         int width, int height);
/* Reserve stable metadata slots before a committed layout borrows resource
   pointers. Later frame/canvas publication can then append without moving
   the table underneath that layout. */
bool images_reserve_capacity(ImageResources *images, Budget *budget,
                             size_t capacity);
/* Copies a bounded dirty rectangle from one script-owned RGBA canvas into a
   native, budget-owned page surface. At most four surfaces and 1 MiB of
   native canvas pixels may be retained per page. The caller keeps ownership
   of rgba_pixels. */
ImageCanvasCommitResult images_commit_canvas_surface(
    ImageResources *images, Budget *budget, lxb_dom_node_t *node,
    const unsigned char *rgba_pixels, size_t rgba_length,
    int width, int height, int dirty_left, int dirty_top,
    int dirty_right, int dirty_bottom);
/* Reserve the native canvas surface without a script-owned pixel copy.
   WebGL renders into a bounded native backend and asks for this destination
   only after the command list completes. The returned pointer remains owned
   by ImageResources and is valid until resize or page teardown. */
ImageCanvasCommitResult images_prepare_canvas_surface(
    ImageResources *images, Budget *budget, lxb_dom_node_t *node,
    int width, int height, unsigned char **rgba_pixels);
/* Persistent depth is separate from the hot ImageResource entries and exists
   only for the at-most-two bounded WebGL contexts. Values use the PSP GE's
   reversed 16-bit depth representation (near is larger). */
bool images_prepare_canvas_depth(ImageResources *images, Budget *budget,
                                 lxb_dom_node_t *node, int width, int height,
                                 uint16_t **depth_values);
bool images_set_canvas_opaque(ImageResources *images,
                              lxb_dom_node_t *node, bool opaque);
bool images_set_canvas_native_surface(ImageResources *images,
                                      lxb_dom_node_t *node,
                                      const unsigned char *pixels,
                                      size_t stride, uint32_t epoch);
bool images_materialize_canvas_native_surface(ImageResources *images,
                                              lxb_dom_node_t *node);
/* Advances whenever any native canvas surface may change (a WebGL frame is
   rendered into EDRAM, a surface is published or forgotten). A deferred page
   publication that recorded an older value no longer describes the surface. */
uint32_t image_canvas_native_serial(void);
void image_canvas_native_serial_advance(void);
bool image_resource_native_canvas_source(const ImageResource *image,
                                         const unsigned char **pixels,
                                         size_t *stride);
bool image_resource_materialize_native_canvas(ImageResource *image);
/* Deterministically retire one canvas's page-owned color/depth storage.
   Used by WebGL context loss so a terminal context cannot retain one of the
   two bounded depth slots until whole-page teardown. */
bool images_release_canvas(ImageResources *images, Budget *budget,
                           lxb_dom_node_t *node);
bool image_resource_available(const ImageResource *image);
const void *image_resource_backing_identity(const ImageResource *image);
/* Distinguishes corrupt/unsupported data, which is safe to negative-cache,
   from allocator pressure or a busy bounded decoder, which must be retried.
   On success, the caller owns *pixels and releases it with
   image_resource_free_decoded(). */
ImageDecodeStatus image_resource_decode_checked(
    const ImageResource *image, Budget *budget, unsigned char **pixels);
unsigned char *image_resource_decode(const ImageResource *image,
                                     Budget *budget);
void image_resource_free_decoded(Budget *budget, unsigned char *pixels);
void images_destroy(ImageResources *images);

#endif
