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
} StylesheetDocumentResource;

typedef struct {
    Budget *budget;
    StylesheetDocumentResource items[STYLESHEET_DOCUMENT_RESOURCE_LIMIT];
    size_t count;
    size_t retained_body_hits;
    size_t transient_retries;
    size_t transient_failures;
    size_t terminal_failures;
    size_t retry_suppressed;
    size_t final_retry_grants;
    size_t pressure_serializations;
    /* A generated theme registry can be discovered during the viewport-first
       pass and referenced by href links in a later parser continuation. Keep
       the bounded selection beside the page-lifetime response ledger so each
       continuation applies the same author choice and resource budget. */
    uint64_t alternate_theme_hashes[STYLESHEET_ALTERNATE_THEME_LIMIT];
    bool alternate_theme_active[STYLESHEET_ALTERNATE_THEME_LIMIT];
    size_t alternate_theme_count;
    bool alternate_theme_selection_valid;
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
    size_t imports_discovered;
    size_t imports_loaded;
    size_t imports_skipped_conditions;
    size_t imports_skipped_depth;
    size_t bytes;
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
    PseudoElement pseudo;
    bool owns_pixels;
    bool owns_encoded;
} ImageResource;

typedef struct {
    size_t discovered;
    size_t attempted;
    size_t loaded;
    size_t failed;
    size_t unsupported;
    /* Explicit .webp paths rewritten to a cheaper same-origin JPEG sibling;
       signed and content-negotiated WebP remains supported. */
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
} ExternalImageStats;

typedef struct {
    Budget *budget;
    ImageResource *items;
    size_t count;
    size_t capacity;
    ExternalImageStats stats;
    bool priority_staged;
} ImageResources;

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
    /* Authoritative visual-pixel box from a bounded layout. Zero means the
       loader must retain its ordinary viewport/source policy. */
    uint16_t display_width;
    uint16_t display_height;
    uint8_t kind;
    uint8_t pseudo;
} ImagePriorityTarget;

typedef enum {
    IMAGE_PRIORITY_LOAD_PENDING = 0,
    IMAGE_PRIORITY_LOAD_COMPLETE,
    IMAGE_PRIORITY_LOAD_FAILED
} ImagePriorityLoadStatus;

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
ImagePriorityLoadStatus images_priority_load_pump(
    ImagePriorityLoadJob *job);
void images_priority_load_destroy(ImagePriorityLoadJob *job);
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
