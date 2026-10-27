#include "tilefinch/resources.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "data_url.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"
#include "tilefinch/integer_math.h"
#include "tilefinch/layout.h"
#include "tilefinch/image_retarget.h"
#include "tilefinch/media_discovery.h"
#include "tilefinch/platform.h"
#if defined(__PSP__)
#include "tilefinch/psp_display.h"
#endif
#include "tilefinch/url.h"
#include "image_decode_internal.h"
#include "image_svg_decode_internal.h"
#include "style_paint_internal.h"
#include "style_cache_internal.h"
#include "diagnostic_trace.h"
#include "tilefinch_test_faults.h"

#include <lexbor/html/serialize.h>

#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#define budget_malloc(b, s) \
    budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) \
    budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) \
    budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

#define MAX_IMAGE_FETCH_ATTEMPTS 128
#define MAX_TRACKED_IMAGE_NODES IMAGE_TRACKED_NODE_LIMIT
/* Image-count policy. The per-page image count (24 on the PSP) bounds the
   work a page can start against the network: every counted attempt may hold
   a fetch slot, an encoded-byte reservation and a full raster decode. Memory
   is bounded separately and honestly by the decoded- and encoded-byte quotas
   and by MAX_TRACKED_IMAGE_NODES, which every image still pays. The table
   includes shared-source aliases, so its metadata bound is independent of
   the unchanged network-attempt bound. Icons that
   never touch the network would otherwise exhaust the count before the first
   real photograph (a news header carries twenty-odd inline-SVG glyphs), so
   an inline SVG whose raster is no larger than a 64x64 icon, and a data: URL
   no longer than a small inline placeholder, are recorded as count_exempt.
   A larger fresh inline-SVG decode still counts, and once the count is
   spent only an icon-sized raster is decoded. Reusing an already-admitted
   surface starts neither a fetch nor a decode and spends no new attempt. */
#define IMAGE_COUNT_EXEMPT_DECODED_BYTES (64u * 64u * 4u)
#define IMAGE_COUNT_EXEMPT_DATA_URL_BYTES 4096u
#define MAX_RASTER_DECODE_WORKING_BYTES (8u * 1024u * 1024u)
#define MAX_PRIORITY_RASTER_DECODE_WORKING_BYTES (10u * 1024u * 1024u)
#define IMAGE_FETCH_CONCURRENCY 4
#define IMAGE_FETCH_PUMP_COMPLETIONS 1u
#define IMAGE_FETCH_POLL_WAIT_MS 1u
#define IMAGE_FETCH_PUMP_CALLBACKS 1u
#define IMAGE_FETCH_PUMP_BYTES (16u * 1024u)
#define IMAGE_FETCH_PUMP_TIME_US UINT64_C(2000)
#define IMAGE_FETCH_MAX_REPLAY_IDLE_POLLS 256u
#define IMAGE_FETCH_NO_PROGRESS_FLOOR_MS 1000.0
#define IMAGE_FETCH_NO_PROGRESS_CEILING_MS 3000.0
#define IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT 4u
#define IMAGE_FETCH_ORIGIN_COOLDOWN_STRIKES 2u
static void image_trace(const char *reason, const char *source,
                        size_t length)
{
    /* tilefinch_trace_images() caches its own answer and is constant false
       under TILEFINCH_NO_TRACE. */
    if (!tilefinch_trace_images()) return;
    if (source == NULL) source = "";
    if (length > 200) length = 200;
    fprintf(stderr, "tilefinch: image %s %.*s\n", reason, (int) length, source);
}

/* Chrome icons and wordmarks; a larger SVG raster would evict encoded
   responses from the session cache for a one-off illustration. */
#define IMAGE_SVG_DECODED_CACHE_BYTES (128u * 1024u)
#define INLINE_SVG_SOURCE_LIMIT (64u * 1024u)
#define INLINE_SVG_USE_LIMIT 32u
#define INLINE_SVG_USE_DEPTH_LIMIT 8u
#define INLINE_SVG_EXPANSION_NODE_LIMIT 512u
#define INLINE_SVG_STYLE_DEPTH_LIMIT 48u
#define INLINE_SVG_STYLE_NODE_LIMIT 2048u
#define EXTERNAL_SVG_SYMBOL_LIMIT (64u * 1024u)
#define EXTERNAL_SVG_SYMBOL_ID_LIMIT 96u
#define EXTERNAL_SVG_SYMBOL_TAG_LIMIT 4096u
#define EXTERNAL_SVG_SPRITE_FETCH_LIMIT (768u * 1024u)
#define IMAGE_DATA_URL_SOURCE_LIMIT (256u * 1024u)
#define IMAGE_TRAVERSAL_PENDING_LIMIT 2048u
#define IMAGE_TRAVERSAL_NODE_LIMIT 65536u
#define IMAGE_EXISTING_ALIAS_VISIT_LIMIT 4096u

typedef struct {
    lxb_dom_node_t *node;
    uint64_t source_hash;
    uint16_t display_width;
    uint16_t display_height;
    bool is_mask;
    bool is_background;
    PseudoElement pseudo;
} PendingImageTarget;

typedef struct {
    uint64_t request_id;
    uint64_t url_hash;
    char *url;
    size_t reserved_bytes;
    size_t last_received_bytes;
    long last_status_code;
    unsigned last_replay_delay_pumps;
    double started_ms;
    double last_progress_ms;
    bool no_progress_cancelled;
    bool from_resource_cache;
    bool resource_grant_valid;
    TilefinchResourceGrant resource_grant;
    /* A scheduler completion whose metadata probe encountered the decode
       gate remains owned here and is retried on a later pump. */
    FetchResult *retained_result;
    bool retained_success;
    bool response_side_effects_applied;
    PendingImageTarget *targets;
    size_t target_count;
    size_t target_capacity;
} PendingImageFetch;

typedef struct {
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    size_t strikes;
    bool cooled;
} ImageOriginHealth;

typedef struct {
    char reference[2048];
    char resolved[4096];
    FetchPreparedPageRequest prepared;
} ImageRequestScratch;

typedef struct {
    ImageResource resource;
    PendingImageTarget *targets;
    char *url;
    size_t target_count;
    size_t encoded_length;
    size_t source_decoded_bytes;
    size_t decoded_bytes;
    double prepared_ms;
    uint32_t token;
    bool active;
} ImagePendingRasterDecode;

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle style;
} ImageRefreshAncestorStyle;
enum { IMAGE_REFRESH_ANCESTOR_LIMIT = 8 };
_Static_assert(sizeof(ImageRefreshAncestorStyle) * IMAGE_REFRESH_ANCESTOR_LIMIT
                   <= 4096u,
               "refresh ancestor scratch must remain within 4 KiB");

typedef struct {
    const PocDocument *document;
    ImageResources *images;
    Stylesheet *stylesheet;
    Budget *budget;
    const char *base_url;
    const char *document_url;
    const char *referrer_policy;
    size_t maximum_count;
    size_t maximum_total_encoded_bytes;
    size_t maximum_single_encoded_bytes;
    size_t maximum_decoded_bytes;
    long timeout_ms;
    FetchScheduler *scheduler;
    BrowserSession *session;
    LayoutReuseCache *style_cache;
    const FontSet *fonts;
    int viewport_width;
    const ImagePriorityTarget *priority_targets;
    size_t priority_target_count;
    uint16_t current_display_width;
    uint16_t current_display_height;
    bool preserve_staged_document_images;
    bool defer_document_images;
    lxb_dom_node_t *const *deferred_nodes;
    size_t deferred_node_count;
    /* A refresh builds into a fresh table; this is the outgoing one (read
       only) and the nodes whose entries it is about to retire. */
    const ImageResources *refresh_previous;
    lxb_dom_node_t *const *refresh_retired_nodes;
    size_t refresh_retired_count;
    /* A whole-table rebuild: every entry of refresh_previous retires. */
    bool refresh_retires_all;
    /* Scratch for a single synchronous refresh batch, never retained by the
       image table. Only the first eight ancestors are worth memoizing on the
       PSP; deeper or allocation-refused chains still resolve normally. */
    ImageRefreshAncestorStyle *refresh_ancestors;
    size_t refresh_ancestor_capacity;
    size_t refresh_ancestor_count;
    uint64_t refresh_document_generation;
    uint64_t refresh_stylesheet_generation;
    uint64_t refresh_container_generation;
    PendingImageFetch pending[IMAGE_FETCH_CONCURRENCY];
    ImageOriginHealth unhealthy_origins[
        IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT];
    size_t unhealthy_origin_count;
    size_t pending_count;
    size_t pending_reserved_bytes;
    size_t external_svg_sprite_attempts;
    uint64_t external_svg_sprite_sources[4];
    /* Symbol tags of the sprite whose siblings are being materialized. */
    const struct ExternalSvgSymbolIndex *external_svg_symbols;
    double deadline_ms;
    ImageRequestScratch *request_scratch;
    uint64_t slice_started_us;
    size_t slice_work_units;
    bool eager_decode_rasters;
    bool deadline_cancelled;
    ImagePendingRasterDecode raster_decode;
    /* The owner supplies the scheduling boundary. A decoded response is one
       complete idle unit, so do not call the global cooperation hook from
       inside that unit and accidentally turn benign input into image failure. */
    bool externally_pumped;
    bool cooperate_cancelled;
} ImageLoadContext;

static bool image_count_exhausted(const ImageLoadContext *context)
{
    const ExternalImageStats *stats = &context->images->stats;
    size_t exempt = stats->count_exempt <= stats->attempted
        ? stats->count_exempt : stats->attempted;
    return stats->attempted - exempt >= context->maximum_count;
}

static bool image_response_cross_origin(const char *document_url,
                                        const char *response_url)
{
    TilefinchUrl response;
    if (response_url != NULL
        && strncasecmp(response_url, "data:", 5u) == 0) return false;
    if (response_url == NULL
        || !tilefinch_url_parse(response_url, &response)) return true;
    /* Inline/data images inherit the document's canvas authority. Only
       network responses can carry cross-origin authenticated pixels. */
    if (response.scheme != TILEFINCH_URL_SCHEME_HTTP
        && response.scheme != TILEFINCH_URL_SCHEME_HTTPS) return false;
    return document_url == NULL
        || !tilefinch_url_same_origin(document_url, response_url);
}

struct ImagePriorityLoadJob {
    ImageLoadContext context;
    ImagePriorityTarget targets[IMAGE_PRIORITY_LOAD_BATCH_LIMIT];
    uint64_t source_hashes[IMAGE_PRIORITY_LOAD_BATCH_LIMIT];
    uint64_t document_generation;
    size_t target_count;
    ExternalImageStats retained_stats;
    size_t retained_count;
    double started_ms;
    uint32_t retained_failure_serial;
    bool retained_priority_staged;
    bool admitted;
    bool terminal;
    bool failed;
    bool rolled_back;
};

static TilefinchRequestContext image_request_context(
    const ImageLoadContext *context, const char *url)
{
    return (TilefinchRequestContext) {
        .target_url = url,
        .initiator_url = context == NULL ? NULL : context->document_url,
        .top_level_url = context == NULL ? NULL : context->document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
}

static void image_accept_response_cookies(
    ImageLoadContext *context, const char *fallback_url,
    const FetchResult *fetch)
{
    if (context == NULL || context->session == NULL || fetch == NULL) return;
    TilefinchRequestContext response = image_request_context(
        context, fallback_url);
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        response.target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            context->session, &response, fetch->set_cookies[i]);
    }
}

static double image_now_ms(void)
{
    return (double) tilefinch_platform_monotonic_time_us() * 0.001;
}

static double image_no_progress_limit_ms(const ImageLoadContext *context)
{
    double timeout = context == NULL ? 0.0 : (double) context->timeout_ms;
    if (timeout <= 0.0) return 0.0;
    double limit = timeout / 2.0;
    if (limit < IMAGE_FETCH_NO_PROGRESS_FLOOR_MS) {
        limit = IMAGE_FETCH_NO_PROGRESS_FLOOR_MS;
    }
    if (limit > IMAGE_FETCH_NO_PROGRESS_CEILING_MS) {
        limit = IMAGE_FETCH_NO_PROGRESS_CEILING_MS;
    }
    return limit < timeout ? limit : timeout;
}

static ImageOriginHealth *image_origin_health(ImageLoadContext *context,
                                              const char *url,
                                              bool create)
{
    if (context == NULL || url == NULL) return NULL;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(url, origin, sizeof(origin))) return NULL;
    for (size_t i = 0; i < context->unhealthy_origin_count; i++) {
        if (strcmp(context->unhealthy_origins[i].origin, origin) == 0) {
            return &context->unhealthy_origins[i];
        }
    }
    if (!create
        || context->unhealthy_origin_count
             == IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT) return NULL;
    ImageOriginHealth *health =
        &context->unhealthy_origins[context->unhealthy_origin_count++];
    snprintf(health->origin, sizeof(health->origin), "%s", origin);
    return health;
}

static bool image_origin_cooled(ImageLoadContext *context, const char *url)
{
    ImageOriginHealth *health = image_origin_health(context, url, false);
    return health != NULL && health->cooled;
}

static void image_note_origin_no_progress(ImageLoadContext *context,
                                          const char *url)
{
    ImageOriginHealth *health = image_origin_health(context, url, true);
    if (health == NULL || health->cooled) return;
    if (health->strikes != SIZE_MAX) health->strikes++;
    if (health->strikes < IMAGE_FETCH_ORIGIN_COOLDOWN_STRIKES) return;
    health->cooled = true;
    context->images->stats.no_progress_origin_cooldowns++;
    image_trace("origin-cooldown", health->origin, strlen(health->origin));
}

static bool image_note_pending_progress(ImageLoadContext *context)
{
    double now = image_now_ms();
    bool advanced = false;
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->request_id == 0
            || pending->retained_result != NULL) continue;
        FetchRequestProgress progress;
        if (!fetch_scheduler_request_progress(
                context->scheduler, pending->request_id, &progress)) {
            continue;
        }
        ExternalImageStats *stats = &context->images->stats;
        stats->progress_samples++;
        /* Captured latency advances in virtual scheduling steps, without
           headers or bytes until the recorded response is released. Do not
           mistake that finite countdown for an externally stuck request. */
        bool replay_progressed = pending->last_replay_delay_pumps != 0
            && progress.replay_delay_pumps_remaining
                   < pending->last_replay_delay_pumps;
        pending->last_replay_delay_pumps =
            progress.replay_delay_pumps_remaining;
        advanced = advanced || replay_progressed;
        bool headers_progressed = progress.status_code != 0
            && progress.status_code != pending->last_status_code;
        if (headers_progressed) {
            advanced = true;
            pending->last_status_code = progress.status_code;
            pending->last_progress_ms = now;
            stats->progress_events++;
        }
        if (progress.received_body_bytes > pending->last_received_bytes) {
            advanced = true;
            size_t added =
                progress.received_body_bytes - pending->last_received_bytes;
            if (added <= SIZE_MAX - stats->progress_bytes) {
                stats->progress_bytes += added;
            } else {
                stats->progress_bytes = SIZE_MAX;
            }
            stats->progress_events++;
            pending->last_received_bytes = progress.received_body_bytes;
            pending->last_progress_ms = now;
        } else if (!headers_progressed && !progress.complete) {
            stats->stalled_polls++;
            double stalled = now - pending->last_progress_ms;
            if (stalled > 0.0
                && (uint64_t) stalled > stats->maximum_no_progress_ms) {
                stats->maximum_no_progress_ms = (uint64_t) stalled;
            }
        }
    }
    return advanced;
}

static void image_cancel_no_progress_pending(ImageLoadContext *context)
{
    double limit = image_no_progress_limit_ms(context);
    if (limit <= 0.0) return;
    double now = image_now_ms();
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->request_id == 0 || pending->retained_result != NULL
            || pending->no_progress_cancelled
            || now - pending->last_progress_ms < limit) continue;
        FetchRequestProgress progress;
        if (!fetch_scheduler_request_progress(
                context->scheduler, pending->request_id, &progress)
            || progress.complete) continue;
        pending->no_progress_cancelled = true;
        if (!fetch_scheduler_cancel(
                context->scheduler, pending->request_id,
                "image request made no progress")) continue;
        context->images->stats.no_progress_cancelled++;
        image_note_origin_no_progress(context, pending->url);
        double stalled = now - pending->last_progress_ms;
        if (stalled > 0.0
            && (uint64_t) stalled
                 > context->images->stats.maximum_no_progress_ms) {
            context->images->stats.maximum_no_progress_ms =
                (uint64_t) stalled;
        }
        image_trace("no-progress-cancel", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
    }
}

static void image_note_request_finished(ImageLoadContext *context,
                                        PendingImageFetch *pending,
                                        const FetchResult *fetched,
                                        bool success)
{
    ExternalImageStats *stats = &context->images->stats;
    size_t received = fetched->received_body_bytes;
    if (received < fetched->length) received = fetched->length;
    if (received > pending->last_received_bytes) {
        size_t added = received - pending->last_received_bytes;
        stats->progress_bytes = added <= SIZE_MAX - stats->progress_bytes
            ? stats->progress_bytes + added : SIZE_MAX;
        stats->progress_events++;
        pending->last_received_bytes = received;
    }
    double elapsed = image_now_ms() - pending->started_ms;
    if (elapsed > 0.0 && (uint64_t) elapsed > stats->maximum_request_ms) {
        stats->maximum_request_ms = (uint64_t) elapsed;
    }
    if (success) return;
    if (fetched->timed_out) {
        stats->fetch_failures_timeout++;
    } else if (fetched->status_code >= 400
               && fetched->status_code < 500) {
        stats->fetch_failures_http_4xx++;
    } else if (fetched->status_code >= 500
               && fetched->status_code < 600) {
        stats->fetch_failures_http_5xx++;
    } else if (fetched->trace_external_cancel
               || strstr(fetched->error, "cancel") != NULL
               || strstr(fetched->error, "Cancel") != NULL
               || strstr(fetched->error, "deadline") != NULL) {
        stats->fetch_failures_cancelled++;
    } else if (strstr(fetched->error, "quota exceeded") != NULL) {
        stats->fetch_failures_quota++;
    } else {
        stats->fetch_failures_transport++;
    }
    if (tilefinch_trace_images()) {
        fprintf(stderr,
                "tilefinch: image fetch-result status=%ld timed-out=%s "
                "bytes=%zu elapsed-ms=%llu no-progress-ms=%llu "
                "error=\"%.120s\" url=%.200s\n",
                fetched->status_code, fetched->timed_out ? "yes" : "no",
                received, (unsigned long long) (elapsed > 0.0
                    ? (uint64_t) elapsed : 0),
                (unsigned long long)
                    (image_now_ms() > pending->last_progress_ms
                       ? (uint64_t)
                           (image_now_ms() - pending->last_progress_ms) : 0),
                fetched->error,
                pending->url == NULL ? "" : pending->url);
    }
}

typedef struct {
    Stylesheet *sheet;
    Budget *budget;
    StyleAncestorBloomCache *ancestors;
    ImageRefreshAncestorStyle *refresh_ancestors;
    bool owned;
    bool selector_owned;
} ImageStyleCacheScope;

static void image_style_cache_scope_end(ImageStyleCacheScope *scope)
{
    if (scope->selector_owned) style_selector_cooperation_end(scope->sheet);
    if (scope->owned) style_variable_cache_end(scope->sheet);
    budget_free(scope->budget, scope->ancestors);
    budget_free(scope->budget, scope->refresh_ancestors);
}

static bool image_profile_enabled(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    return true;
#else
    /* Cached inside the helper; constant false under TILEFINCH_NO_TRACE. */
    return tilefinch_trace_image_profile();
#endif
}

static uint64_t image_profile_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static void image_finish_slice(ImageLoadContext *context)
{
    uint64_t finished = tilefinch_platform_monotonic_time_us();
    uint64_t elapsed_us = finished >= context->slice_started_us
        ? finished - context->slice_started_us : 0;
    if (elapsed_us > context->images->stats.max_slice_us) {
        context->images->stats.max_slice_us = elapsed_us;
        context->images->stats.max_slice_work_units =
            context->slice_work_units;
    }
    context->slice_started_us = finished;
    context->slice_work_units = 0;
}

/*
 * The three things this stage does between checkpoints have very different
 * costs, and the supervisor's max-gap line names the phase that ended the
 * widest one. Reporting all three as "resource" meant a wide gap said only
 * that it happened somewhere in image loading; these names say whether it
 * was a transport pump, one response's decode, or the style walk.
 */
static bool image_cooperate(ImageLoadContext *context, const char *phase)
{
    image_finish_slice(context);
    context->images->stats.cooperative_yields++;
    if (context->externally_pumped) return true;
    bool continued = tilefinch_platform_cooperate(
        phase, context->images->stats.work_units);
    if (!continued) context->cooperate_cancelled = true;
    return continued;
}

static bool image_work(ImageLoadContext *context, size_t units,
                       bool force_yield, const char *phase)
{
    ExternalImageStats *stats = &context->images->stats;
    if (units > SIZE_MAX - stats->work_units) stats->work_units = SIZE_MAX;
    else stats->work_units += units;
    if (units > SIZE_MAX - context->slice_work_units) {
        context->slice_work_units = SIZE_MAX;
    } else {
        context->slice_work_units += units;
    }
    return (!force_yield && context->slice_work_units < 32)
           || image_cooperate(context, phase);
}

static bool image_selector_cooperate(void *opaque, lxb_dom_node_t *node,
                                     size_t visits)
{
    (void) node;
    (void) visits;
    return image_work(opaque, 0, true, "image-style");
}

static bool image_stage_expired(const ImageLoadContext *context)
{
    return image_now_ms() >= context->deadline_ms;
}

static void cancel_expired_pending(ImageLoadContext *context)
{
    if (context->deadline_cancelled || !image_stage_expired(context)) return;
    context->deadline_cancelled = true;
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id != 0
            && context->pending[i].retained_result == NULL) {
            (void) fetch_scheduler_cancel(context->scheduler,
                                          context->pending[i].request_id,
                                          "image stage deadline exceeded");
        }
    }
}

static bool image_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static bool image_name_is_any(lxb_dom_node_t *node,
                              const char *const *names, size_t count)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    for (size_t i = 0; name != NULL && i < count; i++) {
        if (strlen(names[i]) == length
            && strncasecmp(name, names[i], length) == 0) return true;
    }
    return false;
}

bool image_inline_svg_draws_content(lxb_dom_node_t *svg, size_t visit_limit,
                                    bool *bounded_out)
{
    static const char *const drawing[] = {
        "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
        "use", "text", "image", "foreignobject"
    };
    static const char *const non_rendering[] = {
        "defs", "symbol", "lineargradient", "radialgradient", "pattern",
        "clippath", "mask", "marker", "filter", "title", "desc",
        "metadata", "style", "script"
    };
    if (bounded_out != NULL) *bounded_out = false;
    size_t visited = 0;
    for (lxb_dom_node_t *node = svg == NULL ? NULL : svg->first_child;
         node != NULL;) {
        if (visited++ == visit_limit) {
            if (bounded_out != NULL) *bounded_out = true;
            return false;
        }
        bool descend = true;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (image_name_is_any(node, drawing,
                                  sizeof(drawing) / sizeof(drawing[0]))) {
                return true;
            }
            descend = !image_name_is_any(
                node, non_rendering,
                sizeof(non_rendering) / sizeof(non_rendering[0]));
        }
        if (descend && node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != svg && node->next == NULL) node = node->parent;
        if (node == svg) break;
        node = node->next;
    }
    return false;
}

static bool image_svg_dimension_zero(lxb_dom_node_t *svg, const char *name)
{
    size_t length = 0;
    const char *value = document_attribute(svg, name, &length);
    if (value == NULL || length == 0) return false;
    size_t at = 0;
    while (at < length && isspace((unsigned char) value[at])) at++;
    if (at == length || value[at] != '0') return false;
    while (at < length && (value[at] == '0' || value[at] == '.')) at++;
    if (at + 2u <= length && strncasecmp(value + at, "px", 2) == 0) at += 2u;
    while (at < length && isspace((unsigned char) value[at])) at++;
    return at == length;
}

/* A sprite sheet: a zero-width or zero-height <svg> that only defines
   symbols. Nothing of it can paint, so it is not an image to decode. */
static bool image_inline_svg_is_sprite_sheet(lxb_dom_node_t *svg)
{
    enum { SPRITE_SHEET_VISIT_LIMIT = 4096 };
    if (!image_svg_dimension_zero(svg, "width")
        && !image_svg_dimension_zero(svg, "height")) return false;
    bool bounded_out = false;
    return !image_inline_svg_draws_content(
               svg, SPRITE_SHEET_VISIT_LIMIT, &bounded_out)
        && !bounded_out;
}

static uint64_t image_hash_url(const char *url)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; url[i] != '\0'; i++) {
        hash = (hash ^ (unsigned char) url[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t image_hash_request(const char *url,
                                   const char *referrer_source,
                                   const char *referrer_policy)
{
    uint64_t hash = image_hash_url(url);
    const char *parts[2] = {
        referrer_source == NULL ? "" : referrer_source,
        referrer_policy == NULL ? "" : referrer_policy
    };
    for (size_t part = 0; part < 2; part++) {
        hash = (hash ^ UINT8_C(0xff)) * UINT64_C(1099511628211);
        for (size_t i = 0; parts[part][i] != '\0'; i++) {
            hash = (hash ^ (unsigned char) parts[part][i])
                   * UINT64_C(1099511628211);
        }
    }
    return hash;
}

static uint64_t image_hash_bytes(const char *data, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (unsigned char) data[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

/* Records that `node`'s source `source_hash` failed for good, for its
   error event (ImageResources.failures); only an <img> element has one. A
   full list, or a Budget without room, drops the record, and the image
   then never reports the failure, as one past the table never loads. */
static void image_note_failure(ImageResources *images, Budget *budget,
                               lxb_dom_node_t *node, uint64_t source_hash,
                               bool is_mask, bool is_background,
                               PseudoElement pseudo)
{
    if (images == NULL || budget == NULL || node == NULL || is_mask
        || is_background || pseudo != PSEUDO_NONE
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->ns != LXB_NS_HTML || node->local_name != LXB_TAG_IMG)
        return;
    for (size_t i = 0; i < images->failure_count; i++) {
        /* Updated in place; a rollback keeps the newer failure. */
        if (images->failures[i].node != node) continue;
        images->failures[i].source_hash = source_hash;
        return;
    }
    if (images->failure_count == images->failure_capacity) {
        size_t capacity = images->failure_capacity == 0
            ? 8u : images->failure_capacity * 2u;
        if (capacity > IMAGE_TRACKED_NODE_LIMIT)
            capacity = IMAGE_TRACKED_NODE_LIMIT;
        /* Never let this optional record register as a refused load. */
        if (capacity <= images->failure_capacity
            || budget_pressure_required(
                   budget, capacity * sizeof(*images->failures), 0)) return;
        struct ImageFailure *grown = budget_realloc(
            budget, images->failures, capacity * sizeof(*grown));
        if (grown == NULL) return;
        images->failures = grown;
        images->failure_capacity = capacity;
    }
    images->failures[images->failure_count++] = (struct ImageFailure) {
        .node = node, .source_hash = source_hash,
        .serial = images->failure_serial++
    };
}

/* Drops `resource`'s element's failure record when the resource publishes
   the source that failed (a successful retry), so recovered elements never
   hold the bounded list's slots. Only an <img> element's own resource has
   a record. True when one was dropped. */
static bool image_failure_recover(ImageResources *images,
                                  const ImageResource *resource)
{
    if (images->failure_count == 0 || resource->node == NULL
        || resource->is_mask || resource->is_background
        || resource->pseudo != PSEUDO_NONE) return false;
    for (size_t i = 0; i < images->failure_count; i++) {
        if (images->failures[i].node != resource->node) continue;
        if (images->failures[i].source_hash != resource->source_hash)
            return false;
        memmove(&images->failures[i], &images->failures[i + 1],
                (images->failure_count - i - 1u) * sizeof(*images->failures));
        images->failure_count--;
        return true;
    }
    return false;
}

/* Undoes the failure records of a rolled-back load that began at
   `retained_serial` and kept the first `retained_count` resources: records
   noted since are dropped, and records the discarded resources recovered
   come back (without growth: they held slots when the load began). A
   failure the load both noted and recovered comes back as well; it did
   fail. */
static void image_failures_rollback(ImageResources *images,
                                    size_t retained_count,
                                    uint32_t retained_serial)
{
    uint32_t noted = images->failure_serial - retained_serial;
    size_t kept = 0;
    for (size_t i = 0; i < images->failure_count; i++) {
        if (images->failures[i].serial - retained_serial < noted) continue;
        images->failures[kept++] = images->failures[i];
    }
    images->failure_count = kept;
    for (size_t i = retained_count; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->recovered_failure || item->node == NULL) continue;
        bool present = false;
        for (size_t at = 0; at < images->failure_count && !present; at++)
            present = images->failures[at].node == item->node;
        if (present || images->failure_count == images->failure_capacity)
            continue;
        images->failures[images->failure_count++] = (struct ImageFailure) {
            .node = item->node, .source_hash = item->source_hash,
            .serial = retained_serial - 1u
        };
    }
    images->failure_serial = retained_serial;
}

/* Records the failure of a request's targets (image_note_failure). */
static void image_note_targets_failure(ImageLoadContext *context,
                                       const PendingImageTarget *targets,
                                       size_t target_count)
{
    for (size_t i = 0; i < target_count; i++) {
        const PendingImageTarget *target = &targets[i];
        image_note_failure(context->images, context->budget, target->node,
                           target->source_hash, target->is_mask,
                           target->is_background, target->pseudo);
    }
}

/* Records a failed fetch's targets. */
static void image_note_pending_failure(ImageLoadContext *context,
                                       const PendingImageFetch *pending)
{
    image_note_targets_failure(context, pending->targets,
                               pending->target_count);
}

static bool image_add(ImageResources *images, ImageResource resource)
{
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        return true;
    }
    if (images->count == images->capacity) {
        size_t capacity = images->capacity == 0 ? 16 : images->capacity * 2;
        if (capacity > MAX_TRACKED_IMAGE_NODES) capacity = MAX_TRACKED_IMAGE_NODES;
        ImageResource *items = budget_realloc(images->budget, images->items,
                                              capacity * sizeof(*items));
        if (items == NULL) return false;
        images->items = items;
        images->capacity = capacity;
    }
    resource.recovered_failure = image_failure_recover(images, &resource);
    images->items[images->count++] = resource;
    return true;
}

bool images_reserve_capacity(ImageResources *images, Budget *budget,
                             size_t capacity)
{
    if (images == NULL || budget == NULL) return false;
    if (capacity > MAX_TRACKED_IMAGE_NODES) {
        capacity = MAX_TRACKED_IMAGE_NODES;
    }
    if (capacity <= images->capacity) return true;
    if (images->budget != NULL && images->budget != budget) return false;
    ImageResource *items = budget_realloc(
        budget, images->items, capacity * sizeof(*items));
    if (items == NULL) return false;
    images->items = items;
    images->capacity = capacity;
    images->budget = budget;
    return true;
}

static void image_resource_release_owned_pixels(
    Budget *budget, ImageResource *resource)
{
    if (budget == NULL || resource == NULL || !resource->owns_pixels) return;
    if (resource->pixel_body != NULL) {
        browser_shared_body_release(resource->pixel_body);
    } else {
        budget_free(budget, resource->pixels);
    }
    resource->pixels = NULL;
    resource->pixel_body = NULL;
    resource->owns_pixels = false;
}

static bool image_node_already_tracked(const ImageResources *images,
                                       const lxb_dom_node_t *node,
                                       uint64_t source_hash,
                                       bool is_mask, bool is_background,
                                       PseudoElement pseudo);

#define IMAGE_CANVAS_SURFACE_LIMIT 4u
#define IMAGE_CANVAS_BYTES_LIMIT (1024u * 1024u)
#define IMAGE_CANVAS_SINGLE_BYTES_LIMIT (512u * 1024u)

static size_t images_canvas_bytes(const ImageResources *images,
                                  size_t *surface_count)
{
    size_t bytes = 0;
    size_t count = 0;
    if (images != NULL) {
        for (size_t i = 0; i < images->count; i++) {
            const ImageResource *item = &images->items[i];
            if (!item->is_canvas) continue;
            count++;
            if (item->width <= 0 || item->height <= 0
                || (size_t) item->width > SIZE_MAX / (size_t) item->height
                || (size_t) item->width * (size_t) item->height
                       > SIZE_MAX / 4u) {
                continue;
            }
            size_t item_bytes =
                (size_t) item->width * (size_t) item->height * 4u;
            if (item_bytes > SIZE_MAX - bytes) {
                bytes = SIZE_MAX;
                break;
            }
            bytes += item_bytes;
        }
    }
    if (surface_count != NULL) *surface_count = count;
    return bytes;
}

static void image_canvas_copy_rect(unsigned char *destination,
                                   const unsigned char *source,
                                   int width, int left, int top,
                                   int right, int bottom)
{
    size_t stride = (size_t) width * 4u;
    size_t row_bytes = (size_t) (right - left) * 4u;
    for (int row = top; row < bottom; row++) {
        size_t offset = (size_t) row * stride + (size_t) left * 4u;
        memcpy(destination + offset, source + offset, row_bytes);
    }
}

static uint32_t image_canvas_native_serial_value = 1u;

uint32_t image_canvas_native_serial(void)
{
    return image_canvas_native_serial_value;
}

void image_canvas_native_serial_advance(void)
{
    if (++image_canvas_native_serial_value == 0u)
        image_canvas_native_serial_value = 1u;
}
#define IMAGE_CANVAS_NATIVE_SERIAL_ADVANCE() \
    image_canvas_native_serial_advance()

static void image_canvas_native_forget(ImageResource *resource)
{
    IMAGE_CANVAS_NATIVE_SERIAL_ADVANCE();
    if (resource == NULL || resource->canvas_native_surface == NULL) return;
    memset(resource->canvas_native_surface, 0,
           sizeof(*resource->canvas_native_surface));
    resource->canvas_native_surface = NULL;
}

ImageCanvasCommitResult images_prepare_canvas_surface(
    ImageResources *images, Budget *budget, lxb_dom_node_t *node,
    int width, int height, unsigned char **rgba_pixels)
{
    if (rgba_pixels != NULL) *rgba_pixels = NULL;
    if (images == NULL || budget == NULL || node == NULL
        || rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u
        || (images->budget != NULL && images->budget != budget)) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (surface_bytes > IMAGE_CANVAS_SINGLE_BYTES_LIMIT) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }

    ImageResource *existing = NULL;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (item->node == node && !item->is_mask && !item->is_background
            && item->pseudo == PSEUDO_NONE) {
            existing = item;
            break;
        }
    }
    if (existing != NULL && !existing->is_canvas) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    if (existing != NULL && existing->pixels != NULL
        && existing->width == width && existing->height == height) {
        *rgba_pixels = existing->pixels;
        return IMAGE_CANVAS_COMMIT_UPDATED;
    }

    size_t surface_count = 0;
    size_t retained_bytes = images_canvas_bytes(images, &surface_count);
    size_t old_bytes = 0;
    if (existing != NULL && existing->width > 0 && existing->height > 0) {
        old_bytes = (size_t) existing->width
                    * (size_t) existing->height * 4u;
    }
    if (existing == NULL && surface_count >= IMAGE_CANVAS_SURFACE_LIMIT) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    if (old_bytes > retained_bytes) return IMAGE_CANVAS_COMMIT_REFUSED;
    retained_bytes -= old_bytes;
    if (retained_bytes > IMAGE_CANVAS_BYTES_LIMIT
        || surface_bytes > IMAGE_CANVAS_BYTES_LIMIT - retained_bytes) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    /* image_add() grows the resource table through its owning Budget. A
       canvas can be the first page image, so bind that owner before the first
       table growth rather than only after the pixel allocation succeeds. */
    images->budget = budget;
    unsigned char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, surface_bytes);
    if (copy == NULL) return IMAGE_CANVAS_COMMIT_REFUSED;
    memset(copy, 0, surface_bytes);

    if (existing == NULL) {
        size_t before = images->count;
        if (!image_add(images, (ImageResource) {
                .node = node,
                .pixels = copy,
                .source_width = width,
                .source_height = height,
                .width = width,
                .height = height,
                .is_canvas = true,
                .owns_pixels = true
            }) || images->count == before) {
            budget_free(budget, copy);
            return IMAGE_CANVAS_COMMIT_REFUSED;
        }
        images->stats.discovered++;
        images->stats.attempted++;
        images->stats.loaded++;
        images->stats.decoded_bytes += surface_bytes;
        *rgba_pixels = copy;
        return IMAGE_CANVAS_COMMIT_CREATED;
    }

    if (existing->owns_pixels) image_resource_release_owned_pixels(
        budget, existing);
    existing->pixels = copy;
    existing->pixel_body = NULL;
    existing->source_width = width;
    existing->source_height = height;
    existing->width = width;
    existing->height = height;
    existing->owns_pixels = true;
    image_canvas_native_forget(existing);
    if (old_bytes <= images->stats.decoded_bytes) {
        images->stats.decoded_bytes -= old_bytes;
    } else {
        images->stats.decoded_bytes = 0;
    }
    images->stats.decoded_bytes += surface_bytes;
    *rgba_pixels = copy;
    return IMAGE_CANVAS_COMMIT_RESIZED;
}

bool images_prepare_canvas_depth(ImageResources *images, Budget *budget,
                                 lxb_dom_node_t *node, int width, int height,
                                 uint16_t **depth_values)
{
    if (depth_values != NULL) *depth_values = NULL;
    if (images == NULL || budget == NULL || node == NULL
        || depth_values == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / sizeof(uint16_t)
        || (images->budget != NULL && images->budget != budget)) return false;
    size_t bytes = (size_t) width * (size_t) height * sizeof(uint16_t);
    size_t selected = 2u;
    for (size_t i = 0; i < 2u; i++) {
        if (images->canvas_depth[i].node == node) {
            selected = i;
            break;
        }
        if (selected == 2u && images->canvas_depth[i].node == NULL)
            selected = i;
    }
    if (selected == 2u) return false;
    if (images->canvas_depth[selected].values != NULL
        && images->canvas_depth[selected].width == width
        && images->canvas_depth[selected].height == height) {
        *depth_values = images->canvas_depth[selected].values;
        return true;
    }
    uint16_t *replacement = budget_malloc_category(
        budget, BUDGET_CATEGORY_RENDER, bytes);
    if (replacement == NULL) return false;
    memset(replacement, 0, bytes);
    budget_free(budget, images->canvas_depth[selected].values);
    images->canvas_depth[selected].node = node;
    images->canvas_depth[selected].values = replacement;
    images->canvas_depth[selected].width = width;
    images->canvas_depth[selected].height = height;
    images->budget = budget;
    *depth_values = replacement;
    return true;
}

bool images_release_canvas(ImageResources *images, Budget *budget,
                           lxb_dom_node_t *node)
{
    if (images == NULL || budget == NULL || node == NULL
        || (images->budget != NULL && images->budget != budget)) return false;
    for (size_t i = 0; i < images->count; ) {
        ImageResource *resource = &images->items[i];
        if (resource->node != node || !resource->is_canvas) {
            i++;
            continue;
        }
        size_t bytes = resource->width > 0 && resource->height > 0
            ? (size_t) resource->width * (size_t) resource->height * 4u : 0u;
        image_canvas_native_forget(resource);
        image_resource_release_owned_pixels(budget, resource);
        if (bytes <= images->stats.decoded_bytes)
            images->stats.decoded_bytes -= bytes;
        else images->stats.decoded_bytes = 0;
        images->count--;
        if (i != images->count) {
            memmove(&images->items[i], &images->items[i + 1u],
                    (images->count - i) * sizeof(images->items[0]));
        }
        memset(&images->items[images->count], 0, sizeof(images->items[0]));
    }
    for (size_t i = 0; i < 2u; i++) {
        if (images->canvas_depth[i].node != node) continue;
        budget_free(budget, images->canvas_depth[i].values);
        memset(&images->canvas_depth[i], 0,
               sizeof(images->canvas_depth[i]));
    }
    return true;
}

ImageCanvasCommitResult images_commit_canvas_surface(
    ImageResources *images, Budget *budget, lxb_dom_node_t *node,
    const unsigned char *rgba_pixels, size_t rgba_length,
    int width, int height, int dirty_left, int dirty_top,
    int dirty_right, int dirty_bottom)
{
    if (rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (rgba_length < surface_bytes) return IMAGE_CANVAS_COMMIT_REFUSED;
    if (dirty_left < 0) dirty_left = 0;
    if (dirty_top < 0) dirty_top = 0;
    if (dirty_right > width) dirty_right = width;
    if (dirty_bottom > height) dirty_bottom = height;
    if (dirty_right <= dirty_left || dirty_bottom <= dirty_top) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    unsigned char *destination = NULL;
    ImageCanvasCommitResult prepared = images_prepare_canvas_surface(
        images, budget, node, width, height, &destination);
    if (prepared == IMAGE_CANVAS_COMMIT_REFUSED || destination == NULL) {
        return IMAGE_CANVAS_COMMIT_REFUSED;
    }
    ImageResource *resource = NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && images->items[i].is_canvas) {
            resource = &images->items[i];
            break;
        }
    }
    if (resource != NULL) {
        image_canvas_native_forget(resource);
    }
    image_canvas_copy_rect(destination, rgba_pixels, width,
                           dirty_left, dirty_top,
                           dirty_right, dirty_bottom);
    (void) images_set_canvas_opaque(images, node, false);
    return prepared;
}

bool images_set_canvas_opaque(ImageResources *images,
                              lxb_dom_node_t *node, bool opaque)
{
    if (images == NULL || node == NULL) return false;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *resource = &images->items[i];
        if (resource->node != node || !resource->is_canvas
            || resource->is_mask || resource->is_background) continue;
        resource->canvas_opaque = opaque;
        return true;
    }
    return false;
}

bool image_resource_native_canvas_source(const ImageResource *image,
                                         const unsigned char **pixels,
                                         size_t *stride)
{
    if (pixels != NULL) *pixels = NULL;
    if (stride != NULL) *stride = 0u;
    if (image == NULL || pixels == NULL || stride == NULL
        || !image->is_canvas || image->canvas_native_surface == NULL
        || !image->canvas_native_surface->authoritative
        || image->canvas_native_surface->pixels == NULL || image->width <= 0
        || image->height <= 0
        || image->canvas_native_surface->stride
               < (size_t) image->width * 4u) {
        return false;
    }
#if defined(__PSP__)
    if (image->canvas_native_surface->epoch
        != psp_display_edram_content_epoch())
        return false;
#endif
    *pixels = image->canvas_native_surface->pixels;
    *stride = image->canvas_native_surface->stride;
    return true;
}

bool image_resource_materialize_native_canvas(ImageResource *image)
{
    const unsigned char *source = NULL;
    size_t stride = 0u;
    if (!image_resource_native_canvas_source(image, &source, &stride)
        || image->pixels == NULL) return false;
    size_t row_bytes = (size_t) image->width * 4u;
    for (int y = 0; y < image->height; y++) memcpy(
        image->pixels + (size_t) y * row_bytes,
        source + (size_t) y * stride, row_bytes);
    image_canvas_native_forget(image);
    return true;
}

bool images_set_canvas_native_surface(ImageResources *images,
                                      lxb_dom_node_t *node,
                                      const unsigned char *pixels,
                                      size_t stride, uint32_t epoch)
{
    if (images == NULL || node == NULL || pixels == NULL) return false;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *resource = &images->items[i];
        if (resource->node != node || !resource->is_canvas
            || resource->width <= 0
            || stride < (size_t) resource->width * 4u) continue;
        size_t slot = 2u;
        for (size_t native = 0; native < 2u; native++) {
            if (images->canvas_native[native].node == node) {
                slot = native;
                break;
            }
            if (slot == 2u && images->canvas_native[native].node == NULL)
                slot = native;
        }
        if (slot == 2u) return false;
        images->canvas_native[slot] = (ImageCanvasNativeSurface) {
            .node = node,
            .pixels = pixels,
            .stride = stride,
            .epoch = epoch,
            .authoritative = true
        };
        resource->canvas_native_surface = &images->canvas_native[slot];
        IMAGE_CANVAS_NATIVE_SERIAL_ADVANCE();
        return true;
    }
    return false;
}

bool images_materialize_canvas_native_surface(ImageResources *images,
                                              lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return false;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *resource = &images->items[i];
        if (resource->node == node && resource->is_canvas)
            return image_resource_materialize_native_canvas(resource);
    }
    return false;
}

bool images_adopt_decoded_surface(ImageResources *images, Budget *budget,
                                  lxb_dom_node_t *node,
                                  unsigned char *rgba_pixels,
                                  int width, int height)
{
    if (images == NULL || budget == NULL || node == NULL
        || rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return false;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (budget_usable_size(rgba_pixels) < surface_bytes
        || images->stats.decoded_bytes > SIZE_MAX - surface_bytes) {
        return false;
    }
    if (images->budget != NULL && images->budget != budget) return false;
    if (image_node_already_tracked(
            images, node, 0, false, false, PSEUDO_NONE)) {
        return false;
    }
    images->budget = budget;
    size_t before = images->count;
    bool added = image_add(images, (ImageResource) {
        .node = node,
        .pixels = rgba_pixels,
        .source_width = width,
        .source_height = height,
        .width = width,
        .height = height,
        .owns_pixels = true
    });
    if (!added || images->count == before) return false;
    images->stats.discovered++;
    images->stats.attempted++;
    images->stats.loaded++;
    images->stats.decoded_bytes += surface_bytes;
    return true;
}

bool images_replace_with_decoded_surface(ImageResources *images,
                                         Budget *budget,
                                         lxb_dom_node_t *node,
                                         unsigned char *rgba_pixels,
                                         int width, int height)
{
    if (images == NULL || budget == NULL || node == NULL
        || rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return false;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (budget_usable_size(rgba_pixels) < surface_bytes
        || images->stats.decoded_bytes > SIZE_MAX - surface_bytes
        || (images->budget != NULL && images->budget != budget)) {
        return false;
    }
    size_t index = images->count;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (item->node == node && !item->is_mask
            && !item->is_background && item->pseudo == PSEUDO_NONE) {
            index = i;
            break;
        }
    }
    if (index == images->count) {
        return images_adopt_decoded_surface(
            images, budget, node, rgba_pixels, width, height);
    }

    ImageResource *retired = &images->items[index];
    bool pixels_retained = false;
    bool encoded_retained = false;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *alias = &images->items[i];
        if (i == index) continue;
        if (retired->owns_pixels && alias->pixels == retired->pixels) {
            alias->pixel_body = retired->pixel_body;
            alias->owns_pixels = true;
            retired->owns_pixels = false;
            pixels_retained = true;
        }
        if (retired->owns_encoded
            && alias->encoded == retired->encoded
            && alias->encoded_body == retired->encoded_body) {
            alias->owns_encoded = true;
            retired->owns_encoded = false;
            encoded_retained = true;
        }
        if (!retired->owns_pixels && !retired->owns_encoded) break;
    }
    if (retired->owns_pixels && !pixels_retained) {
        size_t retired_bytes = 0;
        if (retired->width > 0 && retired->height > 0
            && (size_t) retired->width <= SIZE_MAX / (size_t) retired->height
            && (size_t) retired->width * (size_t) retired->height
                   <= SIZE_MAX / 4u) {
            retired_bytes =
                (size_t) retired->width * (size_t) retired->height * 4u;
        }
        image_resource_release_owned_pixels(budget, retired);
        images->stats.decoded_bytes =
            retired_bytes <= images->stats.decoded_bytes
            ? images->stats.decoded_bytes - retired_bytes : 0;
    }
    if (retired->owns_encoded && !encoded_retained) {
        if (retired->encoded_body != NULL) {
            browser_shared_body_release(retired->encoded_body);
        } else {
            budget_free(budget, retired->encoded);
        }
        images->stats.encoded_bytes =
            retired->encoded_length <= images->stats.encoded_bytes
            ? images->stats.encoded_bytes - retired->encoded_length : 0;
    }
    image_canvas_native_forget(retired);
    *retired = (ImageResource) {
        .node = node,
        .pixels = rgba_pixels,
        .source_width = width,
        .source_height = height,
        .width = width,
        .height = height,
        .owns_pixels = true
    };
    images->budget = budget;
    images->stats.decoded_bytes += surface_bytes;
    return true;
}

static const ImageResource *image_find_hash(const ImageResources *images,
                                            uint64_t hash)
{
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].url_hash == hash
            && image_resource_available(&images->items[i])) {
            return &images->items[i];
        }
    }
    return NULL;
}

static bool image_node_already_tracked(const ImageResources *images,
                                       const lxb_dom_node_t *node,
                                       uint64_t source_hash,
                                       bool is_mask, bool is_background,
                                       PseudoElement pseudo)
{
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->node == node && item->source_hash == source_hash
            && item->is_mask == is_mask
            && item->is_background == is_background
            && item->pseudo == pseudo) return true;
    }
    return false;
}

#include "image_inline_svg.inc"

static bool image_node_in_refresh_set(
    const lxb_dom_node_t *node, lxb_dom_node_t *const *nodes,
    size_t node_count);

/* An SVG raster that keeps its markup (IMAGE_RETARGET_VECTOR), whatever
   its paint role: display retargeting may have resized or released it, and
   the borrower takes over that state with the markup. Without markup only a
   plain inline-SVG raster qualifies: budget-owned pixels at their source
   size with no encoded body, session lease, canvas or paint role. */
static bool image_refresh_svg_lendable(const ImageResource *item)
{
    if (item->node == NULL || item->is_canvas || item->canvas_native_surface != NULL
        || item->width <= 0 || item->height <= 0) return false;
    if ((item->retarget_flags & IMAGE_RETARGET_VECTOR) != 0
        && item->encoded != NULL) return true;
    return item->pixels != NULL && item->pixel_body == NULL
        && item->encoded == NULL && item->encoded_body == NULL
        && !item->is_mask && !item->is_background
        && item->pseudo == PSEUDO_NONE && item->source_hash == 0
        && item->width == item->source_width
        && item->height == item->source_height;
}

/* Whether the entry owns the surface it shows: its pixels, or the markup of
   a vector raster released while unpainted. */
static bool image_owns_surface(const ImageResource *item)
{
    return item->pixels != NULL ? item->owns_pixels : item->owns_encoded;
}

/* Moves ownership of a shared surface (pixels and markup) between two
   entries that show it. */
static void image_move_surface(ImageResource *from, ImageResource *to)
{
    if (from->owns_pixels) {
        from->owns_pixels = false;
        to->owns_pixels = true;
    }
    if (from->owns_encoded && from->encoded == to->encoded) {
        from->owns_encoded = false;
        to->owns_encoded = true;
    }
}

/* The bytes of an inline-SVG raster as first rasterized, which decided
   whether it spent the image count. */
static size_t image_svg_first_raster_bytes(const ImageResource *item)
{
    int width = item->full_width > 0 ? item->full_width : item->width;
    int height = item->full_height > 0 ? item->full_height : item->height;
    return (size_t) width * (size_t) height * 4u;
}

/* A raster keeps its SVG markup only when a reduction could repay it:
   markup above half the raster's bytes costs more than the smallest
   reduction display retargeting makes saves. */
static bool image_svg_keeps_source(size_t decoded, size_t length)
{
    return length != 0 && length <= decoded / 2u;
}

/* A mutation-driven refresh rebuilds into a fresh table, so the hash lookup
   in load_inline_svg cannot see the outgoing rasters. The serialized markup
   is the rasterizer's entire input: every presentation value, currentColor
   and the default viewport (the element's resolved width and height) are
   written into it before hashing, and image_svg_decode reads nothing else.
   Equal hashes are therefore the same pixels, exactly as the in-table alias
   already assumes. Prefer a lender the refresh keeps; its surface outlives
   the commit, so the new entry stays a plain alias. */
static const ImageResource *image_refresh_previous_svg(
    const ImageLoadContext *context, uint64_t hash, bool *retained)
{
    *retained = false;
    const ImageResources *previous = context->refresh_previous;
    if (previous == NULL) return NULL;
    if (context->refresh_retires_all) {
        for (size_t i = 0; i < previous->count; i++) {
            const ImageResource *item = &previous->items[i];
            if (item->url_hash == hash && image_refresh_svg_lendable(item))
                return item;
        }
        return NULL;
    }
    const ImageResource *match = NULL;
    for (size_t i = 0; i < previous->count; i++) {
        const ImageResource *item = &previous->items[i];
        if (item->url_hash != hash || !image_refresh_svg_lendable(item)) {
            continue;
        }
        if (match == NULL) match = item;
        if (!image_node_in_refresh_set(
                item->node, context->refresh_retired_nodes,
                context->refresh_retired_count)) {
            *retained = true;
            return item;
        }
    }
    if (match == NULL) return NULL;
    for (size_t i = 0; i < previous->count; i++) {
        const ImageResource *item = &previous->items[i];
        if (image_resource_backing_identity(item)
                == image_resource_backing_identity(match)
            && !image_node_in_refresh_set(
                item->node, context->refresh_retired_nodes,
                context->refresh_retired_count)) {
            *retained = true;
            break;
        }
    }
    return match;
}

/* A refresh rebuilding the table takes over an outgoing SVG raster of the
   same markup or request, with the paint role of `role`, instead of
   rasterizing it again. Returns 0 when there is none to take over, 1 when
   it was taken over and -1 when the table
   could not grow. */
static int image_refresh_borrow(ImageLoadContext *context,
                                const ImageResource *role, uint64_t hash)
{
    ImageResources *images = context->images;
    bool lender_retained = false;
    const ImageResource *lender =
        image_refresh_previous_svg(context, hash, &lender_retained);
    if (lender == NULL) return 0;
    size_t lent_bytes = lender->pixels == NULL ? 0
        : (size_t) lender->width * (size_t) lender->height * 4u;
    /* A retiring lender's bytes were already deducted from this table's
       total, so re-charge the transferred surface. A retained lender keeps
       its charge and this is an alias. */
    size_t retained_limit = context->maximum_decoded_bytes;
    /* A deferred raster can be materialized after admission. An unchanged
       SVG refresh must not destroy existing artwork merely because that
       outgoing table now exceeds the discovery allowance. Borrowing moves
       the same surface, not a new decode; never grow beyond the old charge. */
    if (context->refresh_previous != NULL
        && context->refresh_previous->stats.decoded_bytes > retained_limit)
        retained_limit = context->refresh_previous->stats.decoded_bytes;
    if (!lender_retained
        && (images->stats.decoded_bytes > retained_limit
            || lent_bytes > retained_limit - images->stats.decoded_bytes))
        return 0;
    ImageResource borrowed = *lender;
    borrowed.node = role->node;
    borrowed.url_hash = hash;
    borrowed.source_hash = role->source_hash;
    borrowed.is_mask = role->is_mask;
    borrowed.is_background = role->is_background;
    borrowed.pseudo = role->pseudo;
    borrowed.cross_origin |= role->cross_origin;
    borrowed.owns_pixels = false;
    borrowed.owns_encoded = false;
    borrowed.borrows_previous = true;
    if (!lender_retained) borrowed.reused_raster_count_exempt = true;
    if (!image_add(images, borrowed)) return -1;
    image_trace(lender_retained ? "svg-refresh-alias" : "svg-refresh-adopt",
                "", 0);
    images->stats.inline_svg_refresh_reused++;
    if (lender_retained) {
        images->stats.duplicate++;
        return 1;
    }
    images->stats.attempted++;
    /* Reusing an admitted surface starts no network request or decode.
       Its bytes and metadata still pay their independent existing quotas. */
    images->stats.count_exempt++;
    images->stats.loaded++;
    if (role->is_mask) images->stats.masks_loaded++;
    if (role->is_background) images->stats.backgrounds_loaded++;
    images->stats.decoded_bytes += lent_bytes;
    images->stats.encoded_bytes += lender->encoded_length;
    if (lent_bytes > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes = lent_bytes;
    }
    if (lent_bytes > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes = lent_bytes;
    }
    return 1;
}

static bool load_inline_svg_source(ImageLoadContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, const FetchResult *sprite)
{
    ImageResources *images = context->images;
    size_t href_length = 0;
    const char *href = sprite == NULL ? NULL
        : inline_svg_external_use_href(node, &href_length);
    uint64_t source_hash = href == NULL ? 0 : image_hash_bytes(href, href_length);
    if (image_node_already_tracked(
            images, node, source_hash, false, false, PSEUDO_NONE)) {
        return true;
    }
    images->stats.discovered++;
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        return true;
    }
    /* See IMAGE_COUNT_EXEMPT_DECODED_BYTES: an icon-sized raster is admitted
       even when the count is spent, a larger one only while it is not. */
    bool count_exhausted = image_count_exhausted(context);
    size_t failures_before = context->budget->failure_count;
    uint64_t serialize_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    InlineSvgBuffer source = {.budget = context->budget};
    bool serialized = sprite == NULL
        ? inline_svg_serialize_expanded(context, node, style, &source)
        : external_svg_materialize_symbol(context, node, sprite, &source);
    if (!serialized) {
        budget_free(context->budget, source.data);
        if (context->budget->failure_count != failures_before) return false;
        images->stats.unsupported++;
        image_trace("inline-svg-serialize-unsupported", "", 0);
        return true;
    }
    inline_svg_resolve_current_color(&source,
                                     style == NULL ? 0 : style->color);
    image_trace("inline-svg", source.data, source.length);
    uint64_t hash = image_hash_bytes(source.data, source.length);
    if (serialize_started != 0) {
        images->stats.inline_svg_serialize_us +=
            image_profile_now_us() - serialize_started;
    }
    const ImageResource *duplicate = image_find_hash(images, hash);
    if (duplicate != NULL) {
        ImageResource alias = *duplicate;
        alias.node = node;
        alias.source_hash = source_hash;
        alias.cross_origin |= sprite != NULL && image_response_cross_origin(
            context->document_url, sprite->effective_url);
        alias.is_mask = false;
        alias.is_background = false;
        alias.pseudo = PSEUDO_NONE;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        images->stats.duplicate++;
        budget_free(context->budget, source.data);
        return image_add(images, alias);
    }
    ImageResource role = {.node = node, .source_hash = source_hash,
        .cross_origin = sprite != NULL && image_response_cross_origin(
            context->document_url, sprite->effective_url)};
    int borrowed = image_refresh_borrow(context, &role, hash);
    if (borrowed != 0) {
        budget_free(context->budget, source.data);
        return borrowed > 0;
    }
    if (images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        budget_free(context->budget, source.data);
        return true;
    }
    size_t remaining = context->maximum_decoded_bytes
                       - images->stats.decoded_bytes;
    int width = 0, height = 0;
    size_t raster_limit = remaining;
    if (count_exhausted && raster_limit > IMAGE_COUNT_EXEMPT_DECODED_BYTES) {
        raster_limit = IMAGE_COUNT_EXEMPT_DECODED_BYTES;
    }
    images->stats.attempted++;
    images->stats.inline_svg_rasterized++;
    image_trace("inline-svg-rasterize", source.data, source.length);
    uint64_t rasterize_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    unsigned char *pixels = image_svg_decode(
        source.data, source.length, context->budget, raster_limit,
        &width, &height);
    if (rasterize_started != 0) {
        images->stats.inline_svg_rasterize_us +=
            image_profile_now_us() - rasterize_started;
    }
    if (pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        budget_free(context->budget, pixels);
        budget_free(context->budget, source.data);
        if (context->budget->failure_count != failures_before) return false;
        /* Nothing is retained, so a refused raster never spends the count.
           Once the count is spent the decoder was held to icon size, and a
           larger picture is a limit skip rather than an unsupported one. */
        images->stats.count_exempt++;
        if (count_exhausted) {
            images->stats.skipped_limit++;
            image_trace("inline-svg-count-limit", "", 0);
        } else {
            images->stats.unsupported++;
            image_trace("inline-svg-decode-unsupported", "", 0);
        }
        return true;
    }
    size_t decoded = (size_t) width * (size_t) height * 4u;
    if (decoded <= IMAGE_COUNT_EXEMPT_DECODED_BYTES) {
        images->stats.count_exempt++;
    }
    ImageResource resource = {
        .node = node, .url_hash = hash, .source_hash = source_hash, .pixels = pixels,
        .cross_origin = sprite != NULL && image_response_cross_origin(
            context->document_url, sprite->effective_url),
        .source_width = width, .source_height = height,
        .width = width, .height = height, .owns_pixels = true
    };
    /* The serialized markup is the rasterizer's whole input, so display
       retargeting can rasterize it again at the painted size. */
    if (image_svg_keeps_source(decoded, source.length)
        && (sprite == NULL || (images->stats.encoded_bytes <= context->maximum_total_encoded_bytes
            && source.length <= context->maximum_total_encoded_bytes - images->stats.encoded_bytes))) {
        char *kept = budget_realloc(context->budget, source.data,
                                    source.length + 1u);
        resource.encoded = (unsigned char *) (kept != NULL
                                              ? kept : source.data);
        resource.encoded_length = source.length;
        resource.owns_encoded = true;
        resource.retarget_flags = IMAGE_RETARGET_VECTOR;
        source.data = NULL;
        images->stats.encoded_bytes += resource.encoded_length;
    }
    budget_free(context->budget, source.data);
    /* These pixels are immutable until retargeting replaces the surface.
       A shared lifetime lets render caches recognize an unchanged raster
       borrowed by a later resource table, without relying on its address. */
    if (resource.encoded != NULL)
        resource.pixel_body = browser_shared_body_take(
            context->budget, pixels, decoded);
    if (!image_add(images, resource)) {
        image_resource_release_owned_pixels(context->budget, &resource);
        budget_free(context->budget, resource.encoded);
        return false;
    }
    images->stats.loaded++;
    images->stats.decoded_bytes += decoded;
    if (decoded > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes = decoded;
    }
    if (decoded > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes = decoded;
    }
    return true;
}

static bool load_inline_svg(ImageLoadContext *context, lxb_dom_node_t *node,
                            const ComputedStyle *style)
{
    return load_inline_svg_source(context, node, style, NULL);
}

/* One already-authorized response supplies every matching symbol, rather than
   spending another network attempt per <use> or silently dropping all but the
   first. Only small extracted symbols survive; the full sprite is not cached.
   Per-image/raster/page ceilings and teardown ownership remain authoritative. */
static bool load_external_svg_siblings(ImageLoadContext *context,
    lxb_dom_node_t *primary, const FetchResult *sprite)
{
    size_t primary_length = 0;
    const char *primary_href = inline_svg_external_use_href(primary, &primary_length);
    if (primary_href == NULL) return true;
    size_t prefix = primary_length;
    while (prefix != 0 && primary_href[prefix - 1u] != '#') prefix--;
    lxb_dom_node_t *root = lxb_dom_interface_node(context->document->html);
    size_t visits = 0;
    ExternalSvgSymbolIndex index = {0};
    bool indexed = false, okay = true;
    for (lxb_dom_node_t *node = root; node != NULL;
         node = inline_svg_next_node(root, node, false)) {
        if (++visits > IMAGE_TRAVERSAL_NODE_LIMIT) {
            context->images->stats.skipped_limit++;
            break;
        }
        if ((visits & 63u) == 0
            && !tilefinch_platform_cooperate("image-svg-symbols", visits)) {
            okay = false;
            break;
        }
        if (node == primary || !document_node_is_tag(node, LXB_TAG_SVG)) continue;
        size_t length = 0;
        const char *href = inline_svg_external_use_href(node, &length);
        if (href == NULL || length <= prefix || memcmp(href, primary_href, prefix) != 0)
            continue;
        if (!indexed && sprite->data != NULL) {
            external_svg_symbol_index_build(context->budget, sprite->data,
                                            sprite->length, &index);
            indexed = true;
        }
        context->external_svg_symbols = &index;
        okay = load_inline_svg_source(context, node, NULL, sprite);
        context->external_svg_symbols = NULL;
        if (!okay) break;
    }
    budget_free(context->budget, index.tags);
    return okay;
}

static int parse_source_size_length(const Stylesheet *stylesheet,
                                    const char *text, size_t length)
{
    while (length != 0 && isspace((unsigned char) *text)) {
        text++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) text[length - 1])) length--;
    if (length == 0 || length > 128) return -1;
    /* currentSrc can be queried before a stylesheet exists. Keep viewport
       units on the same default basis as source selection rather than
       letting the ordinary length parser treat their number as pixels. */
    static const Stylesheet default_viewport = {
        .viewport_width = 480, .viewport_height = 272
    };
    if (stylesheet == NULL) stylesheet = &default_viewport;
    bool percent = false;
    int pixels = style_parse_length(
        stylesheet, text, length, INT_MIN, &percent);
    /* A source size is a non-negative CSS length. Percentages are invalid,
       while calc()/min()/max()/clamp(), font-relative, absolute, and
       viewport units share the engine's ordinary bounded length parser. */
    return pixels == INT_MIN || percent || pixels < 0 ? -1 : pixels;
}

static int image_source_size(const Stylesheet *stylesheet,
                             lxb_dom_node_t *candidate,
                             lxb_dom_node_t *image,
                             int layout_width)
{
    size_t length = 0;
    const char *sizes = document_attribute(candidate, "sizes", &length);
    if ((sizes == NULL || length == 0) && candidate != image) {
        sizes = document_attribute(image, "sizes", &length);
    }
    int viewport_width = stylesheet == NULL ? 480
                                             : stylesheet->viewport_width;
    if (sizes == NULL || length == 0) {
        return layout_width > 0 ? layout_width : viewport_width;
    }
    size_t start = 0;
    unsigned parentheses = 0;
    for (size_t at = 0; at <= length; at++) {
        char character = at < length ? sizes[at] : ',';
        if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        if (character != ',' || parentheses != 0) continue;
        const char *item = sizes + start;
        size_t item_length = at - start;
        while (item_length != 0
               && isspace((unsigned char) *item)) {
            item++;
            item_length--;
        }
        while (item_length != 0
               && isspace((unsigned char) item[item_length - 1])) {
            item_length--;
        }
        size_t length_start = item_length;
        size_t function_depth = 0;
        /* The final CSS component can be a math function whose whitespace
           belongs to the length, not a separator from the media condition.
           Walk the already bounded attribute backwards without allocating
           another tokenizer or splitting nested calc()/clamp() arguments. */
        while (length_start != 0) {
            char component = item[length_start - 1];
            if (component == ')') function_depth++;
            else if (component == '(' && function_depth != 0) function_depth--;
            if (function_depth == 0
                && isspace((unsigned char) component)) break;
            length_start--;
        }
        int pixels = parse_source_size_length(
            stylesheet, item + length_start,
            item_length - length_start);
        if (pixels >= 0) {
            size_t media_length = length_start;
            while (media_length != 0
                   && isspace((unsigned char) item[media_length - 1])) {
                media_length--;
            }
            if (media_length == 0
                || (stylesheet != NULL
                    && stylesheet_media_matches(
                        stylesheet, item, media_length))) {
                return pixels;
            }
        }
        start = at + 1;
    }
    return viewport_width;
}

static const char *select_srcset(const char *text, size_t length,
                                 int source_size, const char *fallback,
                                 size_t fallback_length,
                                 size_t *selected_length)
{
    enum { SRCSET_DESCRIPTOR_NONE, SRCSET_DESCRIPTOR_WIDTH,
           SRCSET_DESCRIPTOR_DENSITY };
    const char *above = NULL, *below = NULL;
    size_t above_length = 0, below_length = 0;
    double above_density = HUGE_VAL, below_density = -1.0;
    int descriptor_kind = SRCSET_DESCRIPTOR_NONE;
    for (size_t at = 0; at < length;) {
        while (at < length
               && (isspace((unsigned char) text[at]) || text[at] == ',')) at++;
        size_t url_start = at;
        /* URL tokens end at ASCII whitespace, not at an embedded comma:
           data: URLs in responsive-image sets routinely contain commas. A
           trailing comma is the candidate separator and is stripped below,
           matching HTML's srcset tokenizer. */
        while (at < length && !isspace((unsigned char) text[at])) at++;
        size_t url_end = at;
        bool trailing_comma = false;
        while (url_end > url_start && text[url_end - 1] == ',') {
            url_end--;
            trailing_comma = true;
        }
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t descriptor_start = at;
        if (!trailing_comma) {
            while (at < length && text[at] != ',') at++;
        }
        size_t descriptor_end = at;
        while (descriptor_end > descriptor_start
               && isspace((unsigned char) text[descriptor_end - 1])) {
            descriptor_end--;
        }
        if (url_end == url_start) continue;
        char descriptor[32] = {0};
        size_t descriptor_length = descriptor_end - descriptor_start;
        if (descriptor_length >= sizeof(descriptor)) {
            descriptor_length = sizeof(descriptor) - 1;
        }
        memcpy(descriptor, text + descriptor_start, descriptor_length);
        char *end = NULL;
        double number = strtod(descriptor, &end);
        double density = -1.0;
        int candidate_kind = SRCSET_DESCRIPTOR_NONE;
        if (end != descriptor && (*end == 'w' || *end == 'W')
            && number > 0.0 && source_size > 0) {
            char *tail = end + 1;
            while (isspace((unsigned char) *tail)) tail++;
            bool valid_height = *tail == '\0';
            if (!valid_height) {
                char *height_end = NULL;
                double height = strtod(tail, &height_end);
                valid_height = height_end != tail && height > 0.0
                    && (*height_end == 'h' || *height_end == 'H')
                    && height_end[1] == '\0';
            }
            if (valid_height) {
                density = number / source_size;
                candidate_kind = SRCSET_DESCRIPTOR_WIDTH;
            }
        } else if (end != descriptor && (*end == 'x' || *end == 'X')
                   && number > 0.0 && end[1] == '\0') {
            density = number;
            candidate_kind = SRCSET_DESCRIPTOR_DENSITY;
        } else if (descriptor_length == 0) {
            density = 1.0;
            candidate_kind = SRCSET_DESCRIPTOR_DENSITY;
        }
        if (density > 0.0) {
            /* A source set has one descriptor family.  Keeping the family
               established by its first valid candidate makes mixed w/x
               authoring deterministic and matches HTML's candidate-set
               processing rather than comparing unlike units. */
            if (descriptor_kind == SRCSET_DESCRIPTOR_NONE) {
                descriptor_kind = candidate_kind;
            }
            if (candidate_kind != descriptor_kind) continue;
            if (density >= 1.0 && density < above_density) {
                above = text + url_start;
                above_length = url_end - url_start;
                above_density = density;
            } else if (density < 1.0 && density > below_density) {
                below = text + url_start;
                below_length = url_end - url_start;
                below_density = density;
            }
        }
    }
    /* In a density-descriptor srcset, HTML treats src as an implicit 1x
       candidate. Omitting it made ordinary 250px/500px article images pick
       the 2x source at device scale 1 and waste decode budget. Width-
       descriptor lists deliberately ignore src. */
    if (descriptor_kind != SRCSET_DESCRIPTOR_WIDTH
        && fallback != NULL && fallback_length != 0
        && (above == NULL || above_density > 1.0)) {
        above = fallback;
        above_length = fallback_length;
        above_density = 1.0;
    }
    const char *selected = above != NULL ? above : below;
    *selected_length = above != NULL ? above_length
                       : (below != NULL ? below_length : 0);
    return selected;
}

static bool supported_picture_type(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    return type == NULL || length == 0
        || (length == 9 && memcmp(type, "image/png", 9) == 0)
        || (length == 10 && memcmp(type, "image/webp", 10) == 0)
        || (length == 10 && memcmp(type, "image/jpeg", 10) == 0)
        || (length == 13 && memcmp(type, "image/svg+xml", 13) == 0);
}

bool image_source_is_placeholder(const char *source, size_t length)
{
    if (source == NULL || length == 0) return true;
    while (length != 0 && isspace((unsigned char) *source)) {
        source++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) source[length - 1u])) length--;
    if (length == 0) return true;
    if ((length == 11u && strncasecmp(source, "about:blank", 11u) == 0)
        || (length == 1u && source[0] == '#')) return true;
    /* Recognize the conventional one-pixel GIF/PNG sentinels by their encoded
       dimensions. A byte-size threshold also catches small authored icons,
       which must remain the image source even when data-* metadata is present. */
    static const char gif_1x1[] =
        "data:image/gif;base64,R0lGODlhAQABA";
    static const char png_1x1[] =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB";
    return (length >= sizeof(gif_1x1) - 1u
            && strncasecmp(source, gif_1x1, 22u) == 0
            && memcmp(source + 22u, gif_1x1 + 22u,
                      sizeof(gif_1x1) - 1u - 22u) == 0)
        || (length >= sizeof(png_1x1) - 1u
            && strncasecmp(source, png_1x1, 22u) == 0
            && memcmp(source + 22u, png_1x1 + 22u,
                      sizeof(png_1x1) - 1u - 22u) == 0);
}

static bool image_lazy_source_is_url(const char *source, size_t length)
{
    if (source == NULL || length == 0) return false;
    while (length != 0 && isspace((unsigned char) *source)) {
        source++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) source[length - 1u])) length--;
    if (length == 0 || source[0] == '#') return false;
    static const char *const rejected[] = {
        "data:", "javascript:", "vbscript:", "about:"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        size_t prefix = strlen(rejected[i]);
        if (length >= prefix
            && strncasecmp(source, rejected[i], prefix) == 0) return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) source[i];
        if (byte < 0x20u || byte == 0x7fu || byte == '<' || byte == '>')
            return false;
    }
    return true;
}

static const char *image_lazy_source(lxb_dom_node_t *node, size_t *length)
{
    static const char *const names[] = {
        "data-src", "data-original", "data-thumb", "data-lazy-src"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t candidate_length = 0;
        const char *candidate = document_attribute(
            node, names[i], &candidate_length);
        while (candidate_length != 0
               && isspace((unsigned char) *candidate)) {
            candidate++;
            candidate_length--;
        }
        while (candidate_length != 0
               && isspace((unsigned char) candidate[candidate_length - 1u]))
            candidate_length--;
        if (image_lazy_source_is_url(candidate, candidate_length)) {
            *length = candidate_length;
            return candidate;
        }
    }
    return NULL;
}

static const char *image_select_source_for_width(
    const Stylesheet *stylesheet, lxb_dom_node_t *node, int layout_width,
    size_t *length)
{
    if (length == NULL) return NULL;
    *length = 0;
    if (node == NULL) return NULL;
    if (node->parent != NULL && document_node_is_tag(node->parent, LXB_TAG_PICTURE)) {
        for (lxb_dom_node_t *child = node->parent->first_child;
             child != NULL && child != node; child = child->next) {
            if (!document_node_is_tag(child, LXB_TAG_SOURCE)
                || !supported_picture_type(child)) continue;
            size_t media_length = 0;
            const char *media = document_attribute(child, "media",
                                                    &media_length);
            if (media != NULL && media_length != 0
                && (stylesheet == NULL
                    || !stylesheet_media_matches(stylesheet, media,
                                                  media_length))) continue;
            size_t srcset_length = 0;
            const char *srcset = document_attribute(child, "srcset",
                                                     &srcset_length);
            if (srcset == NULL || srcset_length == 0) {
                srcset = document_attribute(child, "data-srcset",
                                            &srcset_length);
            }
            const char *selected = select_srcset(
                srcset, srcset == NULL ? 0 : srcset_length,
                image_source_size(stylesheet, child, node, layout_width),
                NULL, 0, length);
            if (selected != NULL) return selected;
        }
    }
    size_t srcset_length = 0;
    const char *srcset = document_attribute(node, "srcset", &srcset_length);
    if (srcset == NULL || srcset_length == 0) {
        srcset = document_attribute(node, "data-srcset", &srcset_length);
    }
    size_t source_length = 0;
    const char *source = document_attribute(node, "src", &source_length);
    const char *selected = select_srcset(
        srcset, srcset == NULL ? 0 : srcset_length,
        image_source_size(stylesheet, node, node, layout_width),
        source, source_length, length);
    if (selected != NULL
        && !image_source_is_placeholder(selected, *length)) return selected;
    size_t lazy_length = 0;
    const char *lazy = image_lazy_source(node, &lazy_length);
    if (lazy != NULL) {
        *length = lazy_length;
        return lazy;
    }
    if (selected != NULL) return selected;
    *length = source_length;
    return source;
}

uint64_t image_source_hash(const char *source, size_t length)
{
    return source == NULL ? 0 : image_hash_bytes(source, length);
}

uint64_t image_current_source_hash(const Stylesheet *stylesheet,
                                   lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *source = image_select_source(stylesheet, node, &length);
    return source == NULL ? 0 : image_hash_bytes(source, length);
}

const char *image_select_source(const Stylesheet *stylesheet,
                                lxb_dom_node_t *node, size_t *length)
{
    return image_select_source_for_width(stylesheet, node, 0, length);
}

static bool svg_markup(const char *content_type, const void *body,
                       size_t length)
{
    if (strstr(content_type, "image/svg+xml") != NULL) return true;
    const unsigned char *data = body;
    size_t maximum = length < 512 ? length : 512;
    for (size_t i = 0; i + 4 <= maximum; i++) {
        if (data[i] == '<'
            && tolower(data[i + 1]) == 's'
            && tolower(data[i + 2]) == 'v'
            && tolower(data[i + 3]) == 'g') return true;
    }
    return false;
}

static bool svg_response(const FetchResult *fetched)
{
    return svg_markup(fetched->content_type, fetched->data, fetched->length);
}

static bool pending_add_target(ImageLoadContext *context,
                               PendingImageFetch *pending,
                               lxb_dom_node_t *node, uint64_t source_hash,
                               bool is_mask,
                               bool is_background, PseudoElement pseudo)
{
    if (pending->target_count >= MAX_TRACKED_IMAGE_NODES) {
        context->images->stats.skipped_limit++;
        return true;
    }
    if (pending->target_count == pending->target_capacity) {
        size_t capacity = pending->target_capacity == 0
                          ? 4 : pending->target_capacity * 2;
        if (capacity > MAX_TRACKED_IMAGE_NODES) {
            capacity = MAX_TRACKED_IMAGE_NODES;
        }
        PendingImageTarget *targets = budget_realloc(
            context->budget, pending->targets, capacity * sizeof(*targets));
        if (targets == NULL) return false;
        pending->targets = targets;
        pending->target_capacity = capacity;
    }
    pending->targets[pending->target_count++] = (PendingImageTarget) {
        .node = node,
        .source_hash = source_hash,
        .display_width = context->current_display_width,
        .display_height = context->current_display_height,
        .is_mask = is_mask,
        .is_background = is_background,
        .pseudo = pseudo
    };
    return true;
}

static bool image_layout_decode_target(
    const PendingImageFetch *pending, int source_width, int source_height,
    int *target_width, int *target_height)
{
    if (pending == NULL || pending->target_count == 0
        || source_width <= 0 || source_height <= 0
        || target_width == NULL || target_height == NULL) return false;
    int width = 0, height = 0;
    for (size_t i = 0; i < pending->target_count; i++) {
        const PendingImageTarget *target = &pending->targets[i];
        if (target->is_mask || target->is_background
            || target->display_width == 0 || target->display_height == 0) {
            return false;
        }
        int requested_width = target->display_width;
        int requested_height = target->display_height;
        if (requested_width >= source_width
            || requested_height >= source_height) return false;
        int scaled_width = requested_width;
        int scaled_height = requested_height;
        /* Preserve the source aspect and cover the proven box. Products are
           bounded by the admitted source pixel count, so this stays on the
           PSP's native 32-bit integer path. */
        size_t width_scale = (size_t) requested_width
                             * (size_t) source_height;
        size_t height_scale = (size_t) requested_height
                              * (size_t) source_width;
        if (width_scale >= height_scale) {
            size_t product = (size_t) source_height
                             * (size_t) requested_width;
            scaled_height = (int) (product / (size_t) source_width
                + (product % (size_t) source_width != 0));
        } else {
            size_t product = (size_t) source_width
                             * (size_t) requested_height;
            scaled_width = (int) (product / (size_t) source_height
                + (product % (size_t) source_height != 0));
        }
        if (scaled_width > width) width = scaled_width;
        if (scaled_height > height) height = scaled_height;
    }
    if (width <= 0 || height <= 0
        || width >= source_width || height >= source_height) return false;
    *target_width = width;
    *target_height = height;
    return true;
}

typedef struct {
    const PendingImageFetch *pending;
    int viewport_width;
    size_t decoded_limit;
} ImageSvgOversizeContext;

static bool image_svg_oversize_target(void *opaque, int source_width,
                                      int source_height, int *width,
                                      int *height)
{
    const ImageSvgOversizeContext *context = opaque;
    /* Backgrounds and masks can address a sprite subrectangle; shrinking
       those before layout would change their coordinate system. */
    for (size_t i = 0; i < context->pending->target_count; i++) {
        const PendingImageTarget *target = &context->pending->targets[i];
        if (target->is_mask || target->is_background) return false;
    }
    if (!image_layout_decode_target(context->pending, source_width,
                                   source_height, width, height)) {
        if (context->viewport_width <= 0) return false;
        *width = source_width < context->viewport_width
            ? source_width : context->viewport_width;
        size_t product = (size_t) source_height * (size_t) *width;
        *height = (int) (product / (size_t) source_width
            + (product % (size_t) source_width != 0));
    }
    while ((size_t) *width * (size_t) *height
               > context->decoded_limit / 4u && (*width > 1 || *height > 1)) {
        *width = *width > 1 ? (*width + 1) / 2 : 1;
        *height = *height > 1 ? (*height + 1) / 2 : 1;
    }
    return *width > 0 && *height > 0;
}

typedef enum {
    IMAGE_DECODED_CACHE_MISS = 0,
    IMAGE_DECODED_CACHE_USED,
    IMAGE_DECODED_CACHE_FAILED
} ImageDecodedCacheResult;

static ImageDecodedCacheResult image_adopt_decoded_cache(
    ImageLoadContext *context, PendingImageFetch *pending,
    const BrowserCacheEntry *cached,
    const TilefinchRequestContext *request_context)
{
    if (context == NULL || pending == NULL || cached == NULL
        || request_context == NULL || context->session == NULL
        || pending->url == NULL || pending->target_count == 0
        || cached->decoded_image_source_width <= 0
        || cached->decoded_image_source_height <= 0) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    int target_width = 0, target_height = 0;
    /* Rasters are published only when downscaled for a layout target, so
       they must match this use's target. An entry at its own intrinsic size
       is an SVG rasterization, which every use of that source shares. */
    bool intrinsic = cached->decoded_image_width
                         == cached->decoded_image_source_width
                     && cached->decoded_image_height
                         == cached->decoded_image_source_height;
    if (intrinsic) {
        target_width = cached->decoded_image_width;
        target_height = cached->decoded_image_height;
    } else if (!image_layout_decode_target(
            pending, cached->decoded_image_source_width,
            cached->decoded_image_source_height,
            &target_width, &target_height)
        || target_width != cached->decoded_image_width
        || target_height != cached->decoded_image_height) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    BrowserDecodedImage decoded = {0};
    if (!browser_session_decoded_image_acquire(
            context->session, pending->url, request_context,
            cached->data, cached->length, &decoded)) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    if (decoded.width != target_width || decoded.height != target_height
        || decoded.source_width != cached->decoded_image_source_width
        || decoded.source_height != cached->decoded_image_source_height
        || decoded.pixels == NULL
        || context->images->stats.decoded_bytes
               > context->maximum_decoded_bytes
        || decoded.pixels->length
               > context->maximum_decoded_bytes
                    - context->images->stats.decoded_bytes) {
        browser_shared_body_release(decoded.pixels);
        return IMAGE_DECODED_CACHE_MISS;
    }
    PendingImageTarget primary = pending->targets[0];
    ImageResource resource = {
        .node = primary.node,
        .url_hash = pending->url_hash,
        .source_hash = primary.source_hash,
        .pixels = decoded.pixels->data,
        .pixel_body = decoded.pixels,
        .source_width = decoded.source_width,
        .source_height = decoded.source_height,
        .width = decoded.width,
        .height = decoded.height,
        .cross_origin = image_response_cross_origin(
            context->document_url,
            browser_cache_entry_response_url(cached)),
        .is_mask = primary.is_mask,
        .is_background = primary.is_background,
        .pseudo = primary.pseudo,
        .owns_pixels = true
    };
    /* An SVG raster also leases the cached markup, so display retargeting
       can rasterize it at the size this page paints it. */
    if (intrinsic && cached->body != NULL
        && inline_svg_external_use_href(primary.node, NULL) == NULL
        && image_svg_keeps_source(decoded.pixels->length, cached->length)
        && svg_markup(cached->content_type, cached->data, cached->length)) {
        resource.encoded_body = browser_shared_body_retain(cached->body);
        if (resource.encoded_body != NULL) {
            resource.encoded = resource.encoded_body->data;
            resource.encoded_length = cached->length;
            resource.owns_encoded = true;
            resource.retarget_flags = IMAGE_RETARGET_VECTOR;
        }
    }
    size_t before = context->images->count;
    if (!image_add(context->images, resource)
        || context->images->count == before) {
        image_resource_release_owned_pixels(context->budget, &resource);
        browser_shared_body_release(resource.encoded_body);
        return IMAGE_DECODED_CACHE_FAILED;
    }
    context->images->stats.loaded++;
    context->images->stats.cache_hits++;
    context->images->stats.decoded_cache_hits++;
    context->images->stats.encoded_bytes += cached->length;
    context->images->stats.decoded_bytes += decoded.pixels->length;
    if (primary.is_mask) context->images->stats.masks_loaded++;
    if (primary.is_background) context->images->stats.backgrounds_loaded++;
    if (decoded.width != decoded.source_width
        || decoded.height != decoded.source_height) {
        context->images->stats.downsampled++;
    }
    size_t source_bytes = 0;
    if ((size_t) decoded.source_width
            <= SIZE_MAX / (size_t) decoded.source_height
        && (size_t) decoded.source_width * (size_t) decoded.source_height
               <= SIZE_MAX / 4u) {
        source_bytes = (size_t) decoded.source_width
                       * (size_t) decoded.source_height * 4u;
    }
    if (source_bytes
            > context->images->stats.largest_source_decode_bytes) {
        context->images->stats.largest_source_decode_bytes = source_bytes;
    }
    if (decoded.pixels->length
            > context->images->stats.largest_target_decode_bytes) {
        context->images->stats.largest_target_decode_bytes =
            decoded.pixels->length;
    }
    for (size_t i = 1; i < pending->target_count; i++) {
        PendingImageTarget target = pending->targets[i];
        ImageResource alias = resource;
        alias.node = target.node;
        alias.source_hash = target.source_hash;
        alias.is_mask = target.is_mask;
        alias.is_background = target.is_background;
        alias.pseudo = target.pseudo;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        context->images->stats.duplicate++;
        if (!image_add(context->images, alias)) {
            return IMAGE_DECODED_CACHE_FAILED;
        }
    }
    return IMAGE_DECODED_CACHE_USED;
}

static PendingImageFetch *pending_find_hash(ImageLoadContext *context,
                                            uint64_t hash)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id != 0
            && context->pending[i].url_hash == hash) {
            return &context->pending[i];
        }
    }
    return NULL;
}

static PendingImageFetch *pending_find_free(ImageLoadContext *context)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id == 0) {
            return &context->pending[i];
        }
    }
    return NULL;
}

static void image_pending_raster_release(ImageLoadContext *context)
{
    if (context == NULL || !context->raster_decode.active) return;
    ImagePendingRasterDecode *pending = &context->raster_decode;
    image_decode_worker_abandon(pending->token);
    image_resource_release_owned_pixels(context->budget, &pending->resource);
    if (pending->resource.owns_encoded) {
        if (pending->resource.encoded_body != NULL) {
            browser_shared_body_release(pending->resource.encoded_body);
        } else {
            budget_free(context->budget, pending->resource.encoded);
        }
    }
    budget_free(context->budget, pending->targets);
    budget_free(context->budget, pending->url);
    memset(pending, 0, sizeof(*pending));
}

static bool image_pending_raster_publish(
    ImageLoadContext *context, ImageDecodeWorkerResult *decoded)
{
    ImagePendingRasterDecode *pending = &context->raster_decode;
    ImageResource *resource = &pending->resource;
    ImageResources *images = context->images;
    if (decoded->status != IMAGE_DECODE_SUCCEEDED
        || decoded->pixels == NULL
        || decoded->pixel_bytes != pending->decoded_bytes
        || decoded->source_width != resource->source_width
        || decoded->source_height != resource->source_height) {
        budget_free(context->budget, decoded->pixels);
        /* Bytes whose header probed fine but which the decoder refuses
           failed for good, as undecodable bytes do on the synchronous
           path: the image gets its error. A transient refusal (the
           worker's arena or gate) stays a limit skip. */
        if (decoded->status == IMAGE_DECODE_DETERMINISTIC_FAILURE) {
            images->stats.unsupported++;
            image_note_targets_failure(context, pending->targets,
                                       pending->target_count);
        } else {
            images->stats.skipped_limit++;
        }
        image_pending_raster_release(context);
        return true;
    }
    resource->pixels = decoded->pixels;
    resource->owns_pixels = true;
    BrowserSharedBody *pixel_body = browser_shared_body_take(
        context->budget, decoded->pixels, decoded->pixel_bytes);
    if (pixel_body != NULL) {
        resource->pixel_body = pixel_body;
        if (context->session != NULL && pending->url != NULL) {
            TilefinchRequestContext request_context =
                image_request_context(context, pending->url);
            (void) browser_session_decoded_image_put(
                context->session, pending->url, &request_context,
                resource->encoded, resource->encoded_length, pixel_body,
                resource->source_width, resource->source_height,
                resource->width, resource->height);
        }
    }
    /* The encoded bytes stay with the resource (usually a lease on the HTTP
       cache's copy): display retargeting decodes from them again if a
       reduced surface is later painted larger. */
    if (!image_add(images, *resource)) {
        image_resource_release_owned_pixels(context->budget, resource);
        image_pending_raster_release(context);
        return false;
    }
    /* The primary entry now owns the surface and the encoded bytes. The
       pending transaction keeps only metadata so rollback can release
       through ImageResources once. */
    resource->owns_pixels = false;
    resource->owns_encoded = false;
    images->stats.loaded++;
    if (resource->is_mask) images->stats.masks_loaded++;
    if (resource->is_background) images->stats.backgrounds_loaded++;
    images->stats.encoded_bytes += pending->encoded_length;
    images->stats.decoded_bytes += pending->decoded_bytes;
    if (resource->width != resource->source_width
        || resource->height != resource->source_height) {
        images->stats.downsampled++;
    }
    if (decoded->working_bytes
            > images->stats.largest_decode_working_bytes) {
        images->stats.largest_decode_working_bytes = decoded->working_bytes;
    }
    if (pending->source_decoded_bytes
            > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes =
            pending->source_decoded_bytes;
    }
    if (pending->decoded_bytes
            > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes =
            pending->decoded_bytes;
    }
    for (size_t i = 1; i < pending->target_count; i++) {
        PendingImageTarget target = pending->targets[i];
        ImageResource alias = *resource;
        alias.node = target.node;
        alias.source_hash = target.source_hash;
        alias.is_mask = target.is_mask;
        alias.is_background = target.is_background;
        alias.pseudo = target.pseudo;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        images->stats.duplicate++;
        if (!image_add(images, alias)) {
            image_pending_raster_release(context);
            return false;
        }
    }
    budget_free(context->budget, pending->targets);
    budget_free(context->budget, pending->url);
    memset(pending, 0, sizeof(*pending));
    return true;
}

static ImagePriorityLoadStatus image_pending_raster_pump(
    ImageLoadContext *context)
{
    ImagePendingRasterDecode *pending = &context->raster_decode;
    if (!pending->active) return IMAGE_PRIORITY_LOAD_COMPLETE;
    if (pending->token == 0) {
        ImageDecodeSubmitResult submit = image_decode_worker_submit(
            &pending->resource, context->budget, &pending->token);
        if (submit == IMAGE_DECODE_SUBMIT_ACCEPTED) {
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        if (submit == IMAGE_DECODE_SUBMIT_BUSY
            && image_now_ms() - pending->prepared_ms
                < (double) context->timeout_ms) {
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        context->images->stats.skipped_limit++;
        image_pending_raster_release(context);
        return IMAGE_PRIORITY_LOAD_COMPLETE;
    }
    ImageDecodeWorkerResult decoded = {0};
    if (!image_decode_worker_collect(pending->token, &decoded))
        return IMAGE_PRIORITY_LOAD_PENDING;
    return image_pending_raster_publish(context, &decoded)
        ? IMAGE_PRIORITY_LOAD_COMPLETE : IMAGE_PRIORITY_LOAD_FAILED;
}

static bool finish_image_fetch(ImageLoadContext *context,
                               PendingImageFetch *pending,
                               FetchResult *fetched, bool success,
                               bool *probe_busy)
{
    if (probe_busy != NULL) *probe_busy = false;
    ImageResources *images = context->images;
    if (!success) {
        images->stats.failed++;
        image_note_pending_failure(context, pending);
        image_trace("fetch-failed", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
        fetch_result_destroy(fetched);
        return true;
    }
    if (!pending->response_side_effects_applied)
        image_accept_response_cookies(context, pending->url, fetched);
    TilefinchRequestContext request_context = image_request_context(
        context, pending->url);
    TilefinchResourceGrant resource_grant = pending->resource_grant;
    if (pending->url != NULL && !pending->resource_grant_valid
        && !fetch_resource_grant_create(
               fetched, &request_context, false, true, false,
               &resource_grant, NULL)) {
        images->stats.failed++;
        image_note_pending_failure(context, pending);
        image_trace("resource-denied", pending->url, strlen(pending->url));
        fetch_result_destroy(fetched);
        return true;
    }
    if (!pending->resource_grant_valid) {
        pending->resource_grant = resource_grant;
        pending->resource_grant_valid = true;
    }
    (void) fetch_result_share_body(fetched);
    bool transient_large_sprite = pending->target_count != 0
        && fetched->length > context->maximum_single_encoded_bytes
        && inline_svg_external_use_href(
               pending->targets[0].node, NULL) != NULL;
    if (!pending->response_side_effects_applied
        && context->session != NULL && pending->url != NULL
        && !pending->from_resource_cache && !transient_large_sprite) {
        char cache_control[256] = {0};
        char vary[128] = {0};
        (void) fetch_response_header_value(
            fetched, "cache-control", cache_control, sizeof(cache_control));
        (void) fetch_response_header_value(fetched, "vary", vary,
                                           sizeof(vary));
        bool stored = false;
        if (fetched->shared_body != NULL) {
            stored = browser_session_cache_put_http_shared_resource(
                context->session, pending->url, fetched->shared_body,
                fetched->etag, fetched->last_modified, fetched->content_type,
                cache_control, vary, tilefinch_platform_monotonic_time_ns(),
                &request_context, &resource_grant);
            if (stored) {
                stored = browser_session_cache_set_resource_response_provenance(
                    context->session, pending->url, &request_context,
                    fetched->effective_url, "");
            }
        }
        image_trace(stored ? "cache-stored" : "cache-store-rejected",
                    pending->url, strlen(pending->url));
    } else if (transient_large_sprite) {
        image_trace("cache-bypass-large-sprite", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
    }
    pending->response_side_effects_applied = true;
    if (pending->target_count == 0
        || fetched->length > context->maximum_total_encoded_bytes
                              - images->stats.encoded_bytes) {
        images->stats.skipped_limit++;
        fetch_result_destroy(fetched);
        return true;
    }
    /* A primary resource owns decoded pixels or the detached response body.
       Never perform that ownership handoff when the bounded node table has no
       slot. Alias additions may still be dropped nonfatally because aliases
       own neither allocation. */
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        fetch_result_destroy(fetched);
        return true;
    }
    PendingImageTarget primary = pending->targets[0];
    int width = 0, height = 0, components = 0;
    int svg_source_width = 0, svg_source_height = 0;
    bool is_svg = svg_response(fetched);
    bool supported = is_svg;
    InlineSvgBuffer external_symbol = {0};
    const void *decode_data = fetched->data;
    size_t decode_length = fetched->length;
    bool external_use = is_svg
        && inline_svg_external_use_href(primary.node, NULL) != NULL;
    if (external_use) {
        if (external_svg_materialize_symbol(
                context, primary.node, fetched, &external_symbol)) {
            decode_data = external_symbol.data;
            decode_length = external_symbol.length;
            image_trace("external-svg-use", pending->url,
                        pending->url == NULL ? 0 : strlen(pending->url));
        } else {
            is_svg = false;
            supported = false;
        }
    }
    const unsigned char *encoded = (const unsigned char *) fetched->data;
    bool is_webp = false;
    if (!is_svg) {
        uint64_t probe_checkpoint = budget_checkpoint(context->budget);
        size_t probe_baseline = context->budget->current;
        ImageDecodeProbeResult probe = image_decode_probe_info(
            context->budget, encoded, fetched->length,
            &width, &height, &components, &is_webp);
        supported = probe == IMAGE_DECODE_PROBE_SUPPORTED;
        /* stb_image's metadata API has no returned allocation ownership.
           Malformed inputs in some format probes can strand decoder scratch;
           reclaim only allocations made by this synchronous probe. */
        if (context->budget->current != probe_baseline) {
            budget_rollback(context->budget, probe_checkpoint);
        }
        if (probe == IMAGE_DECODE_PROBE_BUSY) {
            if (probe_busy != NULL) *probe_busy = true;
            budget_free(context->budget, external_symbol.data);
            return true;
        }
    }
    /* Full discovery can rasterize many provisional SVG targets before
       required layout runs. Preserve its existing workspace reserve by
       releasing only surfaces we can reconstruct from retained source. */
    if (is_svg && !context->eager_decode_rasters) {
        size_t reserve = context->budget->limit / 8u;
        if (reserve > 4u * 1024u * 1024u) reserve = 4u * 1024u * 1024u;
        size_t available = budget_remaining(context->budget);
        if (available < reserve)
            (void) images_reclaim_rebuildable_rasters(images,
                context->session, reserve - available);
    }
    size_t decoded_remaining = context->maximum_decoded_bytes
                               - images->stats.decoded_bytes;
    unsigned char *pixels = NULL;
    bool svg_limited = false;
    if (is_svg && (!external_use || external_symbol.data != NULL)) {
        uint64_t svg_checkpoint = budget_checkpoint(context->budget);
        size_t svg_baseline = context->budget->current;
        ImageSvgOversizeContext oversize = {
            pending, context->viewport_width, decoded_remaining
        };
        pixels = image_svg_decode_bounded(
            decode_data, decode_length, context->budget, decoded_remaining,
            &width, &height,
            external_use ? NULL : image_svg_oversize_target, &oversize,
            &svg_source_width, &svg_source_height, &svg_limited);
        supported = pixels != NULL;
        /* NanoSVG does not expose a partial parse object when parsing fails.
           Its scratch is transaction-local, so reclaim that generation when
           no pixel ownership was returned. */
        if (pixels == NULL && context->budget->current != svg_baseline) {
            budget_rollback(context->budget, svg_checkpoint);
        }
    }
    /* A kept raster keeps its markup: the response (usually a lease on the
       HTTP cache's copy), the data: body or the referenced symbol. */
    bool keep_svg = pixels != NULL
        && ((width != svg_source_width || height != svg_source_height)
            || image_svg_keeps_source(
                (size_t) width * (size_t) height * 4u, decode_length));
    if (!keep_svg || !external_use)
        budget_free(context->budget, external_symbol.data);
    /* Memory, the decoded-bytes quota, a declined oversize target, nesting
       past the parser's depth limits and re-entry are limits, like a
       raster's: no failure record and no error event. Only markup no
       decoder takes failed for good. */
    if (pixels == NULL && svg_limited) {
        images->stats.skipped_limit++;
        image_trace("svg-decode-limit", fetched->effective_url,
                    strlen(fetched->effective_url));
        fetch_result_destroy(fetched);
        return true;
    }
    if (!supported || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        images->stats.unsupported++;
        image_note_pending_failure(context, pending);
        image_trace("decode-unsupported", fetched->effective_url,
                    strlen(fetched->effective_url));
        budget_free(context->budget, pixels);
        if (keep_svg && external_use)
            budget_free(context->budget, external_symbol.data);
        fetch_result_destroy(fetched);
        return true;
    }
    int source_width = is_svg ? svg_source_width : width;
    int source_height = is_svg ? svg_source_height : height;
    size_t source_decoded = (size_t) source_width * (size_t) source_height * 4u;
    size_t decoded = (size_t) width * (size_t) height * 4u;
    size_t decoded_limit = is_svg ? decoded_remaining
                                  : context->maximum_decoded_bytes;
    if (!is_svg) {
        size_t working_limit = context->maximum_decoded_bytes;
        if (working_limit <= SIZE_MAX / 4u) working_limit *= 4u;
        else working_limit = SIZE_MAX;
        const size_t working_cap = context->eager_decode_rasters
            ? MAX_PRIORITY_RASTER_DECODE_WORKING_BYTES
            : MAX_RASTER_DECODE_WORKING_BYTES;
        if (context->maximum_decoded_bytes < working_cap
            && working_limit > working_cap) {
            working_limit = working_cap;
        }
        if (source_decoded > working_limit) {
            images->stats.skipped_limit++;
            fetch_result_destroy(fetched);
            return true;
        }
        /* A raster wider than the physical viewport cannot contribute more
           source samples to one presented row.  Retain its intrinsic source
           dimensions for layout, but make the deferred decoded target match
           the actual display.  Besides avoiding invisible excess work, this
           lets large responsive hero JPEGs decode within the PSP budget: the
           full source exists only while producing a viewport-sized target. */
        int layout_width = 0, layout_height = 0;
        bool layout_sized = context->eager_decode_rasters
            && image_layout_decode_target(
                pending, source_width, source_height,
                &layout_width, &layout_height);
        if (layout_sized) {
            width = layout_width;
            height = layout_height;
            decoded = (size_t) width * (size_t) height * 4u;
        } else if (context->eager_decode_rasters
            && context->viewport_width > 0
            && width > context->viewport_width) {
            height = tilefinch_mul_div_int(
                height, context->viewport_width, width);
            if (height < 1) height = 1;
            width = context->viewport_width;
            decoded = (size_t) width * (size_t) height * 4u;
        }
        while (decoded > decoded_limit && (width > 1 || height > 1)) {
            width = width > 1 ? (width + 1) / 2 : 1;
            height = height > 1 ? (height + 1) / 2 : 1;
            decoded = (size_t) width * (size_t) height * 4u;
        }
    }
    if (decoded > decoded_limit) {
        images->stats.skipped_limit++;
        if (keep_svg && external_use)
            budget_free(context->budget, external_symbol.data);
        budget_free(context->budget, pixels);
        fetch_result_destroy(fetched);
        return true;
    }
    size_t encoded_length = fetched->length;
    ImageResource resource = {.node = primary.node,
                              .url_hash = pending->url_hash,
                              .source_hash = primary.source_hash,
                              .pixels = pixels,
                              .source_width = source_width,
                              .source_height = source_height,
                              .width = width,
                              .height = height,
                              .cross_origin = image_response_cross_origin(
                                  context->document_url,
                                  fetched->effective_url),
                              .is_mask = primary.is_mask,
                              .is_background = primary.is_background,
                              .pseudo = primary.pseudo,
                              .owns_pixels = is_svg};
    if (is_svg && !external_use && pixels != NULL
        && decoded <= IMAGE_SVG_DECODED_CACHE_BYTES) {
        /* Rasterizing an SVG is one uninterruptible parse+fill (a 10 KB
           wordmark costs ~0.4 s on the PSP), and site chrome repeats the
           same file on every page; share the pixels through the session's
           decoded-image cache like a downscaled raster. */
        BrowserSharedBody *pixel_body = browser_shared_body_take(
            context->budget, pixels, decoded);
        if (pixel_body != NULL) {
            resource.pixel_body = pixel_body;
            if (context->session != NULL && pending->url != NULL) {
                TilefinchRequestContext request_context =
                    image_request_context(context, pending->url);
                (void) browser_session_decoded_image_put(
                    context->session, pending->url, &request_context,
                    (const unsigned char *) fetched->data, fetched->length,
                    pixel_body, source_width, source_height, width, height);
            }
        }
    }
    if (keep_svg) resource.retarget_flags = IMAGE_RETARGET_VECTOR;
    if (keep_svg && external_use) {
        resource.encoded = (unsigned char *) external_symbol.data;
        resource.encoded_length = decode_length;
        resource.owns_encoded = true;
    }
    if (!is_svg || (keep_svg && !external_use)) {
        resource.encoded = (unsigned char *) fetched->data;
        resource.encoded_body = fetched->shared_body;
        resource.encoded_length = fetched->length;
        resource.owns_encoded = true;
        fetched->shared_body = NULL;
        fetched->data = NULL;
        fetched->length = 0;
        fetched->capacity = 0;
    }
    if (!is_svg) {
        bool scaled_jpeg = resource.encoded_length >= 2u
            && resource.encoded[0] == 0xffu
            && resource.encoded[1] == 0xd8u
            && resource.source_width > resource.width
            && resource.source_height > resource.height;
        if (context->externally_pumped && context->eager_decode_rasters
            && scaled_jpeg && resource.encoded_body != NULL
            && !context->raster_decode.active) {
            context->raster_decode = (ImagePendingRasterDecode) {
                .resource = resource,
                .targets = pending->targets,
                .url = pending->url,
                .target_count = pending->target_count,
                .encoded_length = encoded_length,
                .source_decoded_bytes = source_decoded,
                .decoded_bytes = decoded,
                .prepared_ms = image_now_ms(),
                .active = true
            };
            pending->targets = NULL;
            pending->target_count = 0;
            pending->target_capacity = 0;
            pending->url = NULL;
            fetch_result_destroy(fetched);
            return true;
        }
        /* Priority resources are discovered before page script and the full
           resource walk inflate the shared realm.  Decode their bounded,
           viewport-sized target now while that temporary working window is
           available.  Keeping only the pixels also prevents a visible hero
           from repeatedly attempting an impossible late decode after the
           rest of the page has consumed the budget. */
        if (context->eager_decode_rasters || is_webp) {
            unsigned char *decoded_pixels = NULL;
            ImageDecodeStatus decode_status = image_resource_decode_checked(
                &resource, context->budget, &decoded_pixels);
            if (image_decode_last_peak_bytes()
                    > images->stats.largest_decode_working_bytes) {
                images->stats.largest_decode_working_bytes =
                    image_decode_last_peak_bytes();
            }
            if (decode_status == IMAGE_DECODE_SUCCEEDED) {
                resource.pixels = decoded_pixels;
                resource.owns_pixels = true;
                BrowserSharedBody *pixel_body = browser_shared_body_take(
                    context->budget, decoded_pixels, decoded);
                if (pixel_body != NULL) {
                    resource.pixel_body = pixel_body;
                    if (context->session != NULL && pending->url != NULL) {
                        (void) browser_session_decoded_image_put(
                            context->session, pending->url,
                            &request_context, resource.encoded,
                            resource.encoded_length, pixel_body,
                            source_width, source_height, width, height);
                    }
                }
                /* Keep the encoded bytes (usually a lease on the HTTP
                   cache's copy) for display retargeting. */
            } else if (is_webp) {
                /* WebP is always attempted in the cooperative resource
                   continuation. If that bounded attempt cannot be admitted,
                   keep the resource identity but release the body instead of
                   retrying the same expensive decode from every paint-cache
                   miss. A cancelled continuation rolls this suffix back. */
                if (resource.encoded_body != NULL) {
                    browser_shared_body_release(resource.encoded_body);
                } else {
                    budget_free(context->budget, resource.encoded);
                }
                resource.encoded = NULL;
                resource.encoded_body = NULL;
                resource.encoded_length = 0;
                resource.owns_encoded = false;
                images->stats.skipped_limit++;
            }
        }
    }
    if (!image_add(images, resource)) {
        image_resource_release_owned_pixels(context->budget, &resource);
        if (resource.encoded_body != NULL) {
            browser_shared_body_release(resource.encoded_body);
        } else {
            budget_free(context->budget, resource.encoded);
        }
        fetch_result_destroy(fetched);
        return false;
    }
    images->stats.loaded++;
    if (primary.is_mask) images->stats.masks_loaded++;
    if (primary.is_background) images->stats.backgrounds_loaded++;
    images->stats.encoded_bytes += encoded_length;
    if (is_svg) images->stats.decoded_bytes += decoded;
    if (!is_svg && resource.pixels != NULL) {
        images->stats.decoded_bytes += decoded;
    }
    if (!is_svg && (width != source_width || height != source_height)) {
        images->stats.downsampled++;
    }
    if (source_decoded > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes = source_decoded;
    }
    if (decoded > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes = decoded;
    }
    for (size_t i = 1; i < pending->target_count; i++) {
        PendingImageTarget target = pending->targets[i];
        ImageResource alias = resource;
        alias.node = target.node;
        alias.source_hash = target.source_hash;
        alias.is_mask = target.is_mask;
        alias.is_background = target.is_background;
        alias.pseudo = target.pseudo;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        images->stats.duplicate++;
        if (!image_add(images, alias)) {
            fetch_result_destroy(fetched);
            return false;
        }
    }
    if (external_use && !load_external_svg_siblings(context, primary.node, fetched)) {
        fetch_result_destroy(fetched);
        return false;
    }
    fetch_result_destroy(fetched);
    return true;
}

static bool finish_one_pending(ImageLoadContext *context, bool wait)
{
    uint64_t scheduler_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    FetchResult *fetched = fetch_result_create(context->budget);
    if (fetched == NULL) return false;
    size_t idle_polls = 0;
    for (;;) {
        size_t first = 0;
        size_t last = IMAGE_FETCH_CONCURRENCY;
        if (context->externally_pumped
            && context->priority_target_count > 1u) {
            /* A visible batch overlaps transport, but its pixels must become
               authoritative in priority order. Otherwise a faster second
               thumbnail relayouts and paints before the focused first card.
               Completed later responses remain safely retained by the
               scheduler until the preceding request is consumed. */
            while (first < IMAGE_FETCH_CONCURRENCY
                   && context->pending[first].request_id == 0) first++;
            last = first < IMAGE_FETCH_CONCURRENCY ? first + 1u : first;
        }
        for (size_t i = first; i < last; i++) {
            PendingImageFetch *pending = &context->pending[i];
            if (pending->request_id == 0) continue;
            /* Do not take ownership of a completed response until its format
               probe can enter the decoder. A previous page may abandon a
               lower-priority JPEG job without waiting; during that bounded
               tail, consuming this result would turn gate contention into a
               terminal unsupported-format verdict. The scheduler safely
               retains the completion for the next pump. Never spin here: the
               browser outranks the worker on PSP and must yield a frame/poll
               so the worker can release the gate. */
            FetchResult *candidate = pending->retained_result;
            bool success = pending->retained_success;
            bool newly_taken = candidate == NULL;
            if (newly_taken) {
                (void) images_decode_worker_reap_cancelled(context->budget);
                if (image_decode_busy()) continue;
                if (!fetch_scheduler_take(context->scheduler,
                                          pending->request_id,
                                          &success, fetched)) {
                    continue;
                }
                candidate = fetched;
                context->pending_reserved_bytes -= pending->reserved_bytes;
                image_note_request_finished(
                    context, pending, candidate, success);
            }
            if (scheduler_started != 0) {
                context->images->stats.scheduler_us +=
                    image_profile_now_us() - scheduler_started;
            }
            uint64_t finish_started = image_profile_enabled()
                ? image_profile_now_us() : 0;
            bool probe_busy = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            bool finish_svg = svg_response(candidate);
            size_t finish_bytes = candidate->length;
#endif
            bool finished = finish_image_fetch(
                context, pending, candidate, success, &probe_busy);
            if (finish_started != 0) {
                uint64_t finish_us = image_profile_now_us() - finish_started;
                context->images->stats.finish_us += finish_us;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                /* One completion runs between two cooperate checkpoints;
                   name the resource when it alone is a visible stall. */
                if (finish_us >= 50000u) {
                    char message[256];
                    snprintf(message, sizeof(message),
                           "tilefinch-image-slow-finish: us=%llu svg=%d "
                           "bytes=%zu url=%.160s",
                           (unsigned long long) finish_us,
                           finish_svg ? 1 : 0, finish_bytes,
                           pending->url != NULL ? pending->url : "");
                    tilefinch_platform_log_message(message);
                }
#endif
            }
            if (probe_busy) {
                if (newly_taken) {
                    pending->retained_result = candidate;
                    pending->retained_success = success;
                    return true;
                }
                fetch_result_free(fetched);
                return true;
            }
            pending->retained_result = NULL;
            context->pending_count--;
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            fetch_result_free(candidate);
            if (!newly_taken) fetch_result_free(fetched);
            /*
             * Decoding one response is the largest single unit this stage
             * owns, and the completion path above reaches it without passing
             * the poll's checkpoint. The final drain loop calls straight back
             * in, so with four requests in flight four decodes could run
             * back to back with nothing between them: measured under PPSSPP
             * that was a 454 ms stretch in which cancel could not be sampled.
             * Take the checkpoint per decode instead of per burst.
             */
            return finished && image_work(
                context, 1, true, "image-decode");
        }
        if (!wait) {
            if (scheduler_started != 0) {
                context->images->stats.scheduler_us +=
                    image_profile_now_us() - scheduler_started;
            }
            fetch_result_free(fetched);
            return true;
        }
        cancel_expired_pending(context);
        /*
         * This function runs inside the UI-facing resource continuation.
         * curl's poll timeout is a hard lower bound on cancellation and
         * presentation latency when no socket is ready.  A ten-millisecond
         * wait made an otherwise empty image unit five times larger than the
         * PSP's two-millisecond advisory budget.  Wait at most one millisecond,
         * admit one bounded body callback, and publish at most one completion
         * per continuation; the scheduler retains all in-flight transfers
         * between pumps, so this does not restart network work.
         */
        const FetchPumpQuota quota = {
            .maximum_body_callbacks = IMAGE_FETCH_PUMP_CALLBACKS,
            .maximum_body_bytes = IMAGE_FETCH_PUMP_BYTES,
            .maximum_time_us = IMAGE_FETCH_PUMP_TIME_US
        };
        size_t completed = fetch_scheduler_pump_bounded(
            context->scheduler, IMAGE_FETCH_PUMP_COMPLETIONS,
            IMAGE_FETCH_POLL_WAIT_MS, &quota, NULL);
        bool progressed = image_note_pending_progress(context);
        image_cancel_no_progress_pending(context);
        if (completed != 0 || progressed) {
            idle_polls = 0;
        } else if (fetch_scheduler_uses_virtual_replay(context->scheduler)
                   && ++idle_polls
                        >= IMAGE_FETCH_MAX_REPLAY_IDLE_POLLS) {
            /* A live pump may wait on platform work, while deterministic
               replay advances immediately. Bound the virtual case so a
               request that only an external actor could cancel cannot spin
               until a wall-clock deadline; live timeout behavior remains
               unchanged. Completed requests retain their normal result path
               on the next pass. */
            context->deadline_cancelled = true;
            for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
                PendingImageFetch *pending = &context->pending[i];
                if (pending->request_id != 0
                    && pending->retained_result == NULL) {
                    (void) fetch_scheduler_cancel(
                        context->scheduler, pending->request_id,
                        "image pipeline made no progress");
                }
            }
            idle_polls = 0;
        }
        if (!image_work(context, 1, true, "image-transport")) {
            fetch_result_free(fetched);
            return false;
        }
    }
}

static void cancel_pending(ImageLoadContext *context)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->retained_result != NULL) {
            fetch_result_destroy(pending->retained_result);
            fetch_result_free(pending->retained_result);
        } else if (pending->request_id != 0) {
            (void) fetch_scheduler_cancel(context->scheduler,
                                          pending->request_id,
                                          "image pipeline aborted");
            FetchResult *fetched = fetch_result_create(context->budget);
            bool success = false;
            if (fetched != NULL) {
                (void) fetch_scheduler_take(context->scheduler,
                                            pending->request_id,
                                            &success, fetched);
            }
            fetch_result_free(fetched);
        }
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
    }
    context->pending_count = 0;
    context->pending_reserved_bytes = 0;
    image_pending_raster_release(context);
}

static const ImageResource *image_find_document_hash(
    const ImageResources *images, uint64_t hash)
{
    if (images == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->url_hash == hash && !item->is_mask
            && !item->is_background && !item->is_canvas
            && image_resource_available(item)) return item;
    }
    return NULL;
}

static lxb_dom_node_t *image_next_within_subtree(
    lxb_dom_node_t *node, lxb_dom_node_t *root)
{
    if (node == NULL || root == NULL) return NULL;
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static bool image_alias_add(ImageResources *images, ImageResource resource,
                            ImageAliasResult *result)
{
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        return true;
    }
    if (images->count == images->capacity) {
        size_t capacity = images->capacity == 0 ? 16u
            : images->capacity * 2u;
        if (capacity > MAX_TRACKED_IMAGE_NODES)
            capacity = MAX_TRACKED_IMAGE_NODES;
        ImageResource *items = NULL;
        if (result->previous_items == NULL) {
            items = budget_malloc(
                images->budget, capacity * sizeof(*items));
            if (items != NULL && images->count != 0) {
                memcpy(items, images->items,
                       images->count * sizeof(*items));
            }
            if (items != NULL) {
                result->previous_items = images->items;
            }
        } else {
            /* No retained layout can point into this intermediate table: its
               pointers still target previous_items until the caller rebinds
               them after aliasing completes. */
            items = budget_realloc(
                images->budget, images->items,
                capacity * sizeof(*items));
        }
        if (items == NULL) return false;
        images->items = items;
        images->capacity = capacity;
    }
    resource.recovered_failure = image_failure_recover(images, &resource);
    images->items[images->count++] = resource;
    return true;
}

ImageAliasResult images_alias_existing_document_subtree(
    const Stylesheet *stylesheet, ImageResources *images,
    lxb_dom_node_t *root, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_aliases)
{
    ImageAliasResult result = {0};
    if (images == NULL || images->budget == NULL || root == NULL
        || base_url == NULL || document_url == NULL
        || referrer_policy == NULL || maximum_aliases == 0) return result;
    if (maximum_aliases > MAX_TRACKED_IMAGE_NODES)
        maximum_aliases = MAX_TRACKED_IMAGE_NODES;
    result.previous_count = images->count;

    ImageRequestScratch *scratch = NULL;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0;
         node != NULL && visited < IMAGE_EXISTING_ALIAS_VISIT_LIMIT;
         visited++, node = image_next_within_subtree(node, root)) {
        if (result.aliased == maximum_aliases
            || images->count == MAX_TRACKED_IMAGE_NODES) break;
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !document_node_is_tag(node, LXB_TAG_IMG)) {
            continue;
        }

        size_t source_length = 0;
        const char *source = image_select_source(
            stylesheet, node, &source_length);
        bool data_source = source != NULL && source_length > 5u
            && strncasecmp(source, "data:", 5u) == 0;
        if (source == NULL || source_length == 0
            || (!data_source && source_length >= 2048u)
            || (data_source
                && source_length > IMAGE_DATA_URL_SOURCE_LIMIT)) {
            continue;
        }
        uint64_t source_hash = image_hash_bytes(source, source_length);
        if (image_node_already_tracked(
                images, node, source_hash, false, false, PSEUDO_NONE)) {
            continue;
        }

        uint64_t url_hash = 0;
        if (data_source) {
            url_hash = source_hash;
        } else {
            if (scratch == NULL) {
                scratch = budget_malloc(images->budget, sizeof(*scratch));
                if (scratch == NULL) break;
            }
            memcpy(scratch->reference, source, source_length);
            scratch->reference[source_length] = '\0';
            if (!fetch_resolve_url(base_url, scratch->reference,
                                   scratch->resolved,
                                   sizeof(scratch->resolved))) {
                continue;
            }
            url_hash = image_hash_request(
                scratch->resolved, document_url, referrer_policy);
        }
        const ImageResource *existing = image_find_document_hash(
            images, url_hash);
        if (existing == NULL) continue;

        /* A successful image_add() may move the bounded resource table, so
           snapshot the owner metadata before growing it. The alias never
           retains `existing` or its DOM node. */
        ImageResource alias = *existing;
        alias.node = node;
        alias.source_hash = source_hash;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        size_t count_before = images->count;
        if (!image_alias_add(images, alias, &result)) break;
        if (images->count == count_before) break;
        images->stats.duplicate++;
        result.aliased++;
    }
    budget_free(images->budget, scratch);
    return result;
}

void images_alias_result_release(ImageResources *images,
                                 ImageAliasResult *result)
{
    if (images == NULL || result == NULL) return;
    if (images->budget != NULL)
        budget_free(images->budget, result->previous_items);
    memset(result, 0, sizeof(*result));
}

static bool load_image_node_with_provenance_impl(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length,
    const char *source_base_url, const char *referrer_source,
    const char *referrer_policy, bool is_mask, bool is_background,
    PseudoElement pseudo)
{
    ImageResources *images = context->images;
    uint64_t source_hash = source == NULL ? 0
        : image_hash_bytes(source, source_length);
    if (image_node_already_tracked(images, node, source_hash,
                                   is_mask, is_background,
                                   pseudo)) return true;
    images->stats.discovered++;
    bool data_source = source != NULL && source_length > 5
        && strncasecmp(source, "data:", 5) == 0;
    if (source == NULL || source_length == 0
        || source_base_url == NULL || referrer_source == NULL) {
        images->stats.failed++;
        if (source_base_url != NULL && referrer_source != NULL)
            image_note_failure(images, context->budget, node, source_hash,
                               is_mask, is_background, pseudo);
        image_trace("invalid-source", source, source_length);
        return true;
    }
    /* Network references are copied into a fixed-size resolver buffer below.
       Data URLs are decoded directly from the DOM string and have their own
       encoded-byte admission gate.  Keep a separate source-text ceiling so
       ordinary percent-encoded SVG icons larger than 2 KiB do not get
       rejected while hostile attributes remain bounded. Both ceilings are
       this engine's limits, not the source's fault: a limit skip with no
       failure record, like a data: body too large to decode. */
    if ((!data_source && source_length >= 2048)
        || (data_source && source_length > IMAGE_DATA_URL_SOURCE_LIMIT)) {
        images->stats.skipped_limit++;
        image_trace("source-limit", source, source_length);
        return true;
    }
    if (data_source) {
        if (!tilefinch_csp_allows_request(
                &context->document->content_security_policy,
                TILEFINCH_DESTINATION_IMAGE, "data:")) {
            images->stats.failed++;
            image_note_failure(images, context->budget, node, source_hash,
                               is_mask, is_background, pseudo);
            return true;
        }
        bool count_exempt = source_length <= IMAGE_COUNT_EXEMPT_DATA_URL_BYTES;
        if ((!count_exempt && image_count_exhausted(context))
            || images->stats.encoded_bytes
                   >= context->maximum_total_encoded_bytes
            || images->stats.decoded_bytes
                   >= context->maximum_decoded_bytes) {
            images->stats.skipped_limit++;
            return true;
        }
        uint64_t hash = image_hash_bytes(source, source_length);
        const ImageResource *duplicate = image_find_hash(images, hash);
        if (duplicate != NULL) {
            ImageResource alias = {
                .node = node,
                .url_hash = hash,
                .source_hash = source_hash,
                .pixels = duplicate->pixels,
                .pixel_body = duplicate->pixel_body,
                .encoded = duplicate->encoded,
                .encoded_body = duplicate->encoded_body,
                .encoded_length = duplicate->encoded_length,
                .source_width = duplicate->source_width,
                .source_height = duplicate->source_height,
                .width = duplicate->width,
                .height = duplicate->height,
                .retarget_flags = duplicate->retarget_flags,
                .full_width = duplicate->full_width,
                .full_height = duplicate->full_height,
                .is_mask = is_mask,
                .is_background = is_background,
                .pseudo = pseudo,
                .owns_pixels = false,
                .owns_encoded = false
            };
            images->stats.duplicate++;
            return image_add(images, alias);
        }
        ImageResource role = {
            .node = node, .source_hash = source_hash, .is_mask = is_mask,
            .is_background = is_background, .pseudo = pseudo
        };
        int borrowed = image_refresh_borrow(context, &role, hash);
        if (borrowed != 0) return borrowed > 0;
        size_t remaining = context->maximum_total_encoded_bytes
                           - images->stats.encoded_bytes;
        size_t maximum = context->maximum_single_encoded_bytes < remaining
                         ? context->maximum_single_encoded_bytes : remaining;
        unsigned char *body = NULL;
        size_t body_length = 0;
        char media_type[128] = {0};
        DataUrlDecodeResult decoded = data_url_decode(
            context->budget, source, source_length, maximum, &body,
            &body_length, media_type, sizeof(media_type));
        images->stats.attempted++;
        if (count_exempt) images->stats.count_exempt++;
        if (decoded != DATA_URL_DECODED) {
            if (decoded == DATA_URL_TOO_LARGE) {
                images->stats.skipped_limit++;
            } else if (decoded == DATA_URL_NO_MEMORY) {
                return false;
            } else {
                images->stats.unsupported++;
                image_note_failure(images, context->budget, node,
                                   source_hash, is_mask, is_background,
                                   pseudo);
            }
            budget_free(context->budget, body);
            image_trace("data-uri-invalid", source, 40);
            return true;
        }
        PendingImageFetch pending = {
            .url_hash = hash
        };
        /* finish_image_fetch may transfer this target into the asynchronous
           JPEG worker.  Use the same budget-owned target storage as network
           responses; a borrowed stack target would outlive this call and be
           dereferenced/freed when the worker publishes. */
        if (!pending_add_target(
                context, &pending, node, source_hash,
                is_mask, is_background, pseudo)) {
            budget_free(context->budget, body);
            return false;
        }
        FetchResult *fetched = fetch_result_create(context->budget);
        if (fetched == NULL) {
            budget_free(context->budget, pending.targets);
            budget_free(context->budget, body);
            return false;
        }
        fetched->data = (char *) body;
        fetched->length = body_length;
        fetched->capacity = body_length + 1;
        fetched->status_code = 200;
        snprintf(fetched->effective_url, sizeof(fetched->effective_url),
                 "data:");
        snprintf(fetched->content_type, sizeof(fetched->content_type), "%s",
                 media_type);
        image_trace("data-uri", source, 40);
        bool probe_busy = false;
        bool finished = finish_image_fetch(
            context, &pending, fetched, true, &probe_busy);
        if (probe_busy) {
            /* Inline bodies have no asynchronous scheduler slot. Defer
               without misclassifying the format; a later page resource pass
               may retry the authored source once the worker yields. */
            fetch_result_destroy(fetched);
            finished = true;
        }
        fetch_result_free(fetched);
        /* The asynchronous path nulls the pointer when it adopts ownership;
           synchronous publication leaves it here for the caller to retire. */
        budget_free(context->budget, pending.targets);
        return finished;
    }
    uint64_t resolve_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    if (context->request_scratch == NULL) {
        context->request_scratch = budget_calloc_category(
            context->budget, BUDGET_CATEGORY_RESOURCE,
            1u, sizeof(*context->request_scratch));
        if (context->request_scratch == NULL) return false;
    }
    char *reference = context->request_scratch->reference;
    memcpy(reference, source, source_length);
    reference[source_length] = '\0';
    char *resolved = context->request_scratch->resolved;
    if (!fetch_resolve_url(source_base_url, reference, resolved, 4096u)
        || !tilefinch_csp_allows_request(
               &context->document->content_security_policy,
               TILEFINCH_DESTINATION_IMAGE, resolved)) {
        images->stats.failed++;
        image_note_failure(images, context->budget, node, source_hash,
                           is_mask, is_background, pseudo);
        return true;
    }
    uint64_t hash = image_hash_request(
        resolved, referrer_source, referrer_policy);
    if (resolve_started != 0) {
        images->stats.admission_resolve_us +=
            image_profile_now_us() - resolve_started;
    }
    const ImageResource *duplicate = image_find_hash(images, hash);
    if (duplicate != NULL) {
        ImageResource alias = {.node = node, .url_hash = hash,
                               .source_hash = source_hash,
                               .pixels = duplicate->pixels,
                               .pixel_body = duplicate->pixel_body,
                               .encoded = duplicate->encoded,
                               .encoded_body = duplicate->encoded_body,
                               .encoded_length = duplicate->encoded_length,
                               .source_width = duplicate->source_width,
                               .source_height = duplicate->source_height,
                               .width = duplicate->width,
                               .height = duplicate->height,
                               .retarget_flags = duplicate->retarget_flags,
                               .full_width = duplicate->full_width,
                               .full_height = duplicate->full_height,
                               .is_mask = is_mask,
                               .is_background = is_background,
                               .pseudo = pseudo,
                               .owns_pixels = false,
                               .owns_encoded = false};
        images->stats.duplicate++;
        return image_add(images, alias);
    }
    ImageResource role = {
        .node = node, .source_hash = source_hash, .is_mask = is_mask,
        .is_background = is_background, .pseudo = pseudo
    };
    int borrowed = image_refresh_borrow(context, &role, hash);
    if (borrowed != 0) return borrowed > 0;
    PendingImageFetch *same = pending_find_hash(context, hash);
    if (same != NULL) {
        return pending_add_target(context, same, node, source_hash, is_mask,
                                  is_background, pseudo);
    }
    if (image_origin_cooled(context, resolved)) {
        images->stats.skipped_limit++;
        images->stats.no_progress_origin_skipped++;
        image_trace("origin-cooled", resolved, strlen(resolved));
        return true;
    }
    if (image_stage_expired(context)) {
        images->stats.skipped_limit++;
        image_trace("stage-expired", resolved, strlen(resolved));
        return true;
    }
    if (image_count_exhausted(context)
        || images->stats.encoded_bytes >= context->maximum_total_encoded_bytes
        || images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        image_trace("pre-limit", resolved, strlen(resolved));
        return true;
    }
    while (context->pending_count == IMAGE_FETCH_CONCURRENCY) {
        if (!finish_one_pending(context, true)) return false;
    }
    if (image_stage_expired(context)
        || images->stats.encoded_bytes >= context->maximum_total_encoded_bytes
        || images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        return true;
    }
    size_t remaining = context->maximum_total_encoded_bytes
                       - images->stats.encoded_bytes;
    /* Do not admit a new response with only the crumbs left behind other
       requests' worst-case reservations. A small preceding image can release
       enough room for the next whole response; prematurely shrinking its
       cap instead guarantees a needless refusal on otherwise affordable
       bodies. Preserve concurrency whenever one full per-file allowance fits. */
    while (context->pending_count != 0
           && context->pending_reserved_bytes > remaining
               - (context->maximum_single_encoded_bytes < remaining
                   ? context->maximum_single_encoded_bytes : remaining)) {
        if (!finish_one_pending(context, true)) return false;
        if (image_stage_expired(context)
            || images->stats.encoded_bytes
               >= context->maximum_total_encoded_bytes) {
            images->stats.skipped_limit++;
            return true;
        }
        remaining = context->maximum_total_encoded_bytes
                    - images->stats.encoded_bytes;
    }
    if (context->pending_reserved_bytes > remaining) return false;
    remaining -= context->pending_reserved_bytes;
    size_t maximum = context->maximum_single_encoded_bytes < remaining
                     ? context->maximum_single_encoded_bytes : remaining;
    /* A single shared sprite commonly carries mobile navigation chrome and
       can be moderately larger than an ordinary raster. In the realistic
       PSP profile, admit one such transient body up to a fixed 768 KiB cap;
       the traversal separately prevents later symbols from repeating it.
       Strict/low-memory profiles retain the caller's ordinary image cap. */
    if (inline_svg_external_use_href(node, NULL) != NULL
        && context->budget->limit >= 24u * 1024u * 1024u
        && maximum < EXTERNAL_SVG_SPRITE_FETCH_LIMIT) {
        maximum = remaining < EXTERNAL_SVG_SPRITE_FETCH_LIMIT
                  ? remaining : EXTERNAL_SVG_SPRITE_FETCH_LIMIT;
    }
    if (maximum == 0) {
        images->stats.skipped_limit++;
        return true;
    }
    PendingImageFetch *pending = pending_find_free(context);
    if (pending == NULL) return false;
    if (!pending_add_target(context, pending, node, source_hash, is_mask,
                            is_background, pseudo)) return false;
    size_t resolved_length = strlen(resolved);
    pending->url = budget_malloc(context->budget, resolved_length + 1);
    if (pending->url == NULL) {
        budget_free(context->budget, pending->targets);
        memset(pending, 0, sizeof(*pending));
        return false;
    }
    memcpy(pending->url, resolved, resolved_length + 1);
    pending->url_hash = hash;
    pending->reserved_bytes = maximum;
    images->stats.attempted++;
    uint64_t cache_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    const BrowserCacheEntry *cached = NULL;
    TilefinchRequestContext request_context = image_request_context(
        context, resolved);
    BrowserCacheStatus cache_status = context->session == NULL
        || content_blocker_would_block(
               context->session->content_blocker, resolved,
               context->document_url, "image", "no-cors")
        ? BROWSER_CACHE_MISS : browser_session_cache_match_resource(
            context->session, resolved, &request_context,
            tilefinch_platform_monotonic_time_ns(), &cached);
    /* The image pipeline has no conditional-request machinery: a stale
       entry is refetched from scratch.  Under hermetic replay that
       refetch can only fail once the original record is claimed (a
       resource-driven style rebuild would silently lose the image), and
       replay time is logical anyway, so a stale body is authoritative
       there.  Live fetches keep ordinary freshness semantics. */
    bool cache_usable = cached != NULL
        && (cache_status == BROWSER_CACHE_FRESH
            || (cache_status == BROWSER_CACHE_STALE
                && fetch_trace_replay_active()));
    if (cache_started != 0) {
        images->stats.admission_cache_us +=
            image_profile_now_us() - cache_started;
    }
    if (cache_usable && cached->length <= maximum) {
        ImageDecodedCacheResult decoded_cache = image_adopt_decoded_cache(
            context, pending, cached, &request_context);
        if (decoded_cache != IMAGE_DECODED_CACHE_MISS) {
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            return decoded_cache == IMAGE_DECODED_CACHE_USED;
        }
        pending->from_resource_cache = true;
        pending->resource_grant_valid = cached->resource_grant_valid;
        pending->resource_grant = cached->resource_grant;
        FetchResult *cached_result = fetch_result_create(context->budget);
        if (cached_result == NULL) {
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            return false;
        }
        cached_result->status_code = 200;
        if (cached->body != NULL) {
            cached_result->shared_body = browser_shared_body_retain(
                cached->body);
            if (cached_result->shared_body == NULL) {
                fetch_result_free(cached_result);
                budget_free(context->budget, pending->targets);
                budget_free(context->budget, pending->url);
                memset(pending, 0, sizeof(*pending));
                return false;
            }
            cached_result->data = (char *) cached->body->data;
            cached_result->capacity = 0;
        } else {
            cached_result->data = budget_malloc(context->budget,
                                                cached->length + 1);
            if (cached_result->data == NULL) {
                fetch_result_free(cached_result);
                budget_free(context->budget, pending->targets);
                budget_free(context->budget, pending->url);
                memset(pending, 0, sizeof(*pending));
                return false;
            }
            memcpy(cached_result->data, cached->data, cached->length);
            cached_result->data[cached->length] = '\0';
            cached_result->capacity = cached->length + 1;
        }
        cached_result->length = cached->length;
        snprintf(cached_result->effective_url,
                 sizeof(cached_result->effective_url), "%s",
                 browser_cache_entry_response_url(cached));
        snprintf(cached_result->content_type,
                 sizeof(cached_result->content_type), "%s",
                 cached->content_type);
        images->stats.cache_hits++;
        char *cached_url = pending->url;
        pending->url = NULL;
        bool probe_busy = false;
        bool finished = finish_image_fetch(
            context, pending, cached_result, true, &probe_busy);
        if (probe_busy) {
            fetch_result_destroy(cached_result);
            finished = true;
        }
        fetch_result_free(cached_result);
        pending->url = cached_url;
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return finished;
    }
    uint64_t context_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    FetchRequest transport = {
        .send_low_client_hints = true,
        /* Advertise only formats the decoders actually handle. */
#if defined(TILEFINCH_DISABLE_GIF)
        .accept = "image/png,image/jpeg,image/webp,image/svg+xml,"
                  "image/*;q=0.8,*/*;q=0.5",
#else
        .accept = "image/png,image/jpeg,image/gif,image/webp,image/svg+xml,"
                  "image/*;q=0.8,*/*;q=0.5",
#endif
    };
    if (!fetch_prepare_page_request_context(
            &request_context, referrer_source, referrer_policy,
            context->session, &context->document->content_security_policy,
            NULL, &transport, &context->request_scratch->prepared, NULL)) {
        images->stats.failed++;
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    const FetchRequest *request = fetch_prepared_page_request(
        &context->request_scratch->prepared);
    if (request == NULL) return true;
    if (context_started != 0) {
        images->stats.admission_context_us +=
            image_profile_now_us() - context_started;
    }
    image_trace("enqueue", resolved, strlen(resolved));
    uint64_t enqueue_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    pending->request_id = fetch_scheduler_enqueue(
        context->scheduler, resolved, request, maximum, context->timeout_ms);
    if (enqueue_started != 0) {
        images->stats.admission_enqueue_us +=
            image_profile_now_us() - enqueue_started;
    }
    if (pending->request_id == 0) {
        images->stats.failed++;
        image_trace("enqueue-failed", resolved, strlen(resolved));
        if (tilefinch_trace_images()) {
            fprintf(stderr, "tilefinch: image enqueue error=%s\n",
                    fetch_scheduler_last_error(context->scheduler));
        }
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    pending->started_ms = image_now_ms();
    pending->last_progress_ms = pending->started_ms;
    context->pending_count++;
    context->pending_reserved_bytes += maximum;
    return finish_one_pending(context, false);
}

static bool load_image_node_with_provenance(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length,
    const char *source_base_url, const char *referrer_source,
    const char *referrer_policy, bool is_mask, bool is_background,
    PseudoElement pseudo)
{
    uint64_t started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    bool loaded = load_image_node_with_provenance_impl(
        context, node, source, source_length, source_base_url,
        referrer_source, referrer_policy, is_mask, is_background, pseudo);
    if (started != 0) {
        context->images->stats.admission_us +=
            image_profile_now_us() - started;
    }
    return loaded;
}

/* Decoder-owned video frames have no encoded poster to reload. Keep the
   exact node's already-admitted backing through a metadata/style refresh,
   including hiding the native overlay. The ordinary lender transaction
   verifies identity and transfers ownership only after the rebuild succeeds. */
static int image_refresh_borrow_video(ImageLoadContext *context,
                                      lxb_dom_node_t *node)
{
    if (context->refresh_previous == NULL) return 0;
    const ImageResource *lender = images_find_node(
        context->refresh_previous, node);
    if (lender == NULL || lender->encoded != NULL
        || !image_refresh_svg_lendable(lender)) return 0;
    if (images_find_node(context->images, node) != NULL) return 1;
    if ((size_t) lender->width > SIZE_MAX / (size_t) lender->height
        || (size_t) lender->width * (size_t) lender->height > SIZE_MAX / 4u)
        return 0;
    size_t bytes = (size_t) lender->width * (size_t) lender->height * 4u;
    if (context->images->stats.decoded_bytes > context->maximum_decoded_bytes
        || bytes > context->maximum_decoded_bytes
                   - context->images->stats.decoded_bytes) return 0;
    ImageResource borrowed = *lender;
    borrowed.owns_pixels = false;
    borrowed.owns_encoded = false;
    borrowed.borrows_previous = true;
    if (!image_add(context->images, borrowed)) return -1;
    context->images->stats.discovered++;
    context->images->stats.attempted++;
    context->images->stats.loaded++;
    context->images->stats.decoded_bytes += bytes;
    return 1;
}

static bool load_document_image_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length, bool is_mask,
    bool is_background, PseudoElement pseudo)
{
    return load_image_node_with_provenance(
        context, node, source, source_length,
        context->base_url, context->document_url, context->referrer_policy,
        is_mask, is_background, pseudo);
}

static bool load_stylesheet_image_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length, bool is_mask,
    bool is_background, PseudoElement pseudo)
{
    const char *sheet_base = NULL;
    const char *sheet_policy = NULL;
    if (!stylesheet_image_url_source(
            context->stylesheet, source, &sheet_base, &sheet_policy)) {
        /* A CSS pointer without retained provenance is an internal or quota
           failure. Drop it rather than silently reinterpret it relative to
           the document and potentially leak a Referer. */
        context->images->stats.discovered++;
        context->images->stats.failed++;
        return true;
    }
    const char *base = sheet_base == NULL ? context->base_url : sheet_base;
    const char *referrer_source = sheet_base == NULL
        ? context->document_url : sheet_base;
    const char *policy = sheet_base == NULL
        ? context->referrer_policy : sheet_policy;
    return load_image_node_with_provenance(
        context, node, source, source_length, base, referrer_source, policy,
        is_mask, is_background, pseudo);
}

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle parent;
    bool has_parent;
} ImageTraversalEntry;

static bool image_traversal_push(
    ImageLoadContext *context, ImageTraversalEntry **entries,
    size_t *count, size_t *capacity, lxb_dom_node_t *node,
    const ComputedStyle *parent)
{
    if (node == NULL) return true;
    if (*count == IMAGE_TRAVERSAL_PENDING_LIMIT) return false;
    if (*count == *capacity) {
        size_t next = *capacity == 0 ? 8u : *capacity * 2u;
        if (next > IMAGE_TRAVERSAL_PENDING_LIMIT) {
            next = IMAGE_TRAVERSAL_PENDING_LIMIT;
        }
        if (next <= *capacity
            || next > SIZE_MAX / sizeof(**entries)) return false;
        ImageTraversalEntry *grown = budget_realloc(
            context->budget, *entries, next * sizeof(**entries));
        if (grown == NULL) return false;
        *entries = grown;
        *capacity = next;
    }
    ImageTraversalEntry *entry = &(*entries)[(*count)++];
    entry->node = node;
    entry->has_parent = parent != NULL;
    if (parent != NULL) entry->parent = *parent;
    return true;
}

static bool image_discovery_prefilter_disabled(void)
{
#ifndef TILEFINCH_NO_TRACE
    return getenv("TILEFINCH_DISABLE_IMAGE_PREFILTER") != NULL;
#else
    return false;
#endif
}

static bool image_process_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *style, bool *traverse)
{
    *traverse = true;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    uint64_t style_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    bool style_cache_hit = false;
    if (context->style_cache != NULL) {
        style_cache_hit = layout_reuse_cache_resolve_style(
            context->style_cache, context->stylesheet, context->fonts,
            node, parent, style);
        if (style_cache_hit) context->images->stats.node_style_cache_hits++;
        else context->images->stats.node_style_cache_misses++;
    } else {
        *style = style_for_node(context->stylesheet, node, parent);
    }
    if (style_started != 0) {
        uint64_t elapsed = image_profile_now_us() - style_started;
        context->images->stats.style_resolve_us += elapsed;
        context->images->stats.node_style_resolve_us += elapsed;
    }
    *traverse = style->display != DISPLAY_NONE && !style->hidden;
    bool retained_video = false;
    if (document_node_is_tag(node, LXB_TAG_VIDEO)) {
        int borrowed = image_refresh_borrow_video(context, node);
        if (borrowed < 0) return false;
        retained_video = borrowed != 0;
    }
    bool replacement = *traverse && style->generated_image;
    if (replacement && !load_stylesheet_image_node(context, node,
            style->generated_text, strlen(style->generated_text),
            false, true, PSEUDO_NONE)) return false;
    bool atomic_inline_svg = *traverse && !replacement
        && document_node_is_tag(node, LXB_TAG_SVG);
    if (atomic_inline_svg && image_inline_svg_is_sprite_sheet(node)) {
        /* Its symbols render through <use> in other SVGs, which serialize
           the referenced symbol themselves; nothing below it is an HTML
           resource either. */
        *traverse = false;
        return true;
    }
    bool deferred = context->defer_document_images;
    if (document_node_is_tag(node, LXB_TAG_IMG)) {
        for (size_t i = 0; !deferred && i < context->deferred_node_count; i++)
            deferred = context->deferred_nodes[i] == node;
    }
    if (*traverse && !replacement && !deferred && document_node_is_tag(node, LXB_TAG_IMG)
        && (!context->preserve_staged_document_images
            || images_find_node(context->images, node) == NULL)) {
        size_t source_length = 0;
        const char *source = image_select_source(
            context->stylesheet, node, &source_length);
        if (!load_document_image_node(context, node, source, source_length,
                                      false, false, PSEUDO_NONE)) return false;
    }
    if (*traverse && !replacement && !retained_video
        && document_node_is_tag(node, LXB_TAG_VIDEO)) {
        size_t poster_length = 0;
        const char *poster = document_attribute(
            node, "poster", &poster_length);
        if ((poster == NULL || poster_length == 0u)) {
            const MediaDeclaredVideo *declared =
                media_declared_video_cached(context->document);
            poster = declared == NULL ? NULL : declared->thumbnail_url;
            poster_length = poster == NULL ? 0u : strlen(poster);
        }
        if (poster != NULL && poster_length != 0
            && !load_document_image_node(
                context, node, poster, poster_length,
                false, false, PSEUDO_NONE)) return false;
    }
    if (atomic_inline_svg) {
        size_t external_length = 0;
        const char *external = inline_svg_external_use_href(
            node, &external_length);
        if (external != NULL) {
            size_t prefix = external_length;
            while (prefix != 0 && external[prefix - 1u] != '#') prefix--;
            uint64_t source = image_hash_bytes(external, prefix);
            bool attempted = false;
            for (size_t i = 0; i < context->external_svg_sprite_attempts; i++)
                if (context->external_svg_sprite_sources[i] == source) attempted = true;
            if (attempted || context->external_svg_sprite_attempts == 4u) {
                context->images->stats.skipped_limit++;
            } else {
                context->external_svg_sprite_sources[context->external_svg_sprite_attempts++] = source;
                if (!load_document_image_node(
                        context, node, external, external_length,
                        false, false, PSEUDO_NONE)) return false;
            }
        } else if (!load_inline_svg(context, node, style)) {
            return false;
        }
    }
    /* The scalar fields retain the common one-layer fast path. Multi-layer
       values live in the bounded paint stack and must keep distinct resource
       identities even though they belong to the same DOM node. */
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->stylesheet, computed_style_paint_stack_id(style));
    bool layered_background = paint != NULL
        && (paint->components & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0;
    bool layered_mask = paint != NULL
        && (paint->components & STYLE_PAINT_COMPONENT_MASK_IMAGE) != 0;
    if (*traverse && layered_background) {
        for (size_t i = 0; i < paint->background_count; i++) {
            const StylePaintLayer *layer = &paint->backgrounds[i];
            if (layer->kind == STYLE_PAINT_IMAGE_URL
                && layer->image != NULL && layer->image[0] != '\0'
                && !load_stylesheet_image_node(
                       context, node, layer->image, strlen(layer->image),
                       false, true, PSEUDO_NONE)) return false;
        }
    } else if (*traverse
               && style->background_image_kind
                  == STYLE_BACKGROUND_IMAGE_URL
               && style->background_image != NULL
               && style->background_image[0] != '\0'
               && !load_stylesheet_image_node(
                      context, node, style->background_image,
                      strlen(style->background_image), false, true,
                      PSEUDO_NONE)) return false;
    if (*traverse && layered_mask) {
        for (size_t i = 0; i < paint->mask_count; i++) {
            const StylePaintLayer *layer = &paint->masks[i];
            if (layer->kind == STYLE_PAINT_IMAGE_URL
                && layer->image != NULL && layer->image[0] != '\0'
                && !load_stylesheet_image_node(
                       context, node, layer->image, strlen(layer->image),
                       true, false, PSEUDO_NONE)) return false;
        }
    } else if (*traverse && style->mask_image != NULL
               && style->mask_image[0] != '\0'
               && !load_stylesheet_image_node(
                      context, node, style->mask_image,
                      strlen(style->mask_image), true, false,
                      PSEUDO_NONE)) return false;
    /* A replacement suppresses markup descendants and generated pseudos,
       but its own background and mask still belong to the element's box. */
    if (replacement) *traverse = false;
    if (!*traverse) return true;
    /* Inline SVG is decoded as one atomic replaced image. Its serializer
       already resolved the descendant presentation cascade. Set this before
       the pseudo-image prefilter can return, or that common fast path walks
       the descendants again despite having no independent HTML resources. */
    if (atomic_inline_svg) *traverse = false;
    /* Remaining generated-content images come from ::before/::after rules.
       When no such rule that can supply one matches this element, both
       pseudo resolutions are skipped; the exact test on the few flagged
       candidates is far cheaper than two resolutions. */
    if (!image_discovery_prefilter_disabled()) {
        context->images->stats.discovery_prefilter_checks++;
        if (!style_node_pseudo_rules_may_affect_discovery(
                context->stylesheet, node)) {
            context->images->stats.discovery_prefilter_skips++;
            return true;
        }
    }
    for (PseudoElement pseudo = PSEUDO_BEFORE;
         pseudo <= PSEUDO_AFTER; pseudo++) {
        style_started = image_profile_enabled()
            ? image_profile_now_us() : 0;
        void *previous_matches = layout_reuse_cache_attach_matches(
            context->style_cache, context->stylesheet);
        ComputedStyle generated = style_for_pseudo(
            context->stylesheet, node, pseudo, style);
        layout_reuse_cache_detach_matches(
            context->stylesheet, previous_matches);
        if (style_started != 0) {
            uint64_t elapsed = image_profile_now_us() - style_started;
            context->images->stats.style_resolve_us += elapsed;
            context->images->stats.pseudo_style_resolve_us += elapsed;
        }
        const StylePaintStack *generated_paint = stylesheet_paint_stack(
            context->stylesheet,
            computed_style_paint_stack_id(&generated));
        bool generated_layered_background = generated_paint != NULL
            && (generated_paint->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0;
        bool generated_layered_mask = generated_paint != NULL
            && (generated_paint->components
                & STYLE_PAINT_COMPONENT_MASK_IMAGE) != 0;
        if (generated.generated_content && generated_layered_background) {
            for (size_t i = 0; i < generated_paint->background_count; i++) {
                const StylePaintLayer *layer =
                    &generated_paint->backgrounds[i];
                if (layer->kind == STYLE_PAINT_IMAGE_URL
                    && layer->image != NULL && layer->image[0] != '\0'
                    && !load_stylesheet_image_node(
                           context, node, layer->image,
                           strlen(layer->image), false, true, pseudo)) {
                    return false;
                }
            }
        } else if (generated.generated_content
                   && generated.background_image_kind
                      == STYLE_BACKGROUND_IMAGE_URL
                   && generated.background_image != NULL
                   && generated.background_image[0] != '\0'
                   && !load_stylesheet_image_node(
                          context, node, generated.background_image,
                          strlen(generated.background_image), false,
                          true, pseudo)) return false;
        if (generated.generated_content && generated_layered_mask) {
            for (size_t i = 0; i < generated_paint->mask_count; i++) {
                const StylePaintLayer *layer = &generated_paint->masks[i];
                if (layer->kind == STYLE_PAINT_IMAGE_URL
                    && layer->image != NULL && layer->image[0] != '\0'
                    && !load_stylesheet_image_node(
                           context, node, layer->image,
                           strlen(layer->image), true, false, pseudo)) {
                    return false;
                }
            }
        } else if (generated.generated_content
                   && generated.mask_image != NULL
                   && generated.mask_image[0] != '\0'
                   && !load_stylesheet_image_node(
                          context, node, generated.mask_image,
                          strlen(generated.mask_image), true, false,
                          pseudo)) return false;
    }
    return true;
}

static bool image_process_refresh_node(
    ImageLoadContext *context, lxb_dom_node_t *node);

static bool image_process_priority_node(
    ImageLoadContext *context, lxb_dom_node_t *node)
{
    /* A provisional layout proves the element is visible, but it does not
       retain the exact inherited ComputedStyle chain needed to resolve CSS
       backgrounds and generated content authoritatively. Prioritize only
       HTML image sources here; the complete traversal below retains normal
       document order and quota semantics for every style-derived resource. */
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    if (document_node_is_tag(node, LXB_TAG_VIDEO)) {
        int borrowed = image_refresh_borrow_video(context, node);
        if (borrowed != 0) return borrowed > 0;
    }
    if (document_node_is_tag(node, LXB_TAG_SVG)) {
        /* A provisional layout has already proved that this exact SVG is
           visible. Resolve its bounded ancestor chain so compact responsive
           icons are rasterized before large markup images consume the PSP's
           decoded-image budget. */
        return image_process_refresh_node(context, node);
    }
    if (!document_node_is_tag(node, LXB_TAG_IMG) && !document_node_is_tag(node, LXB_TAG_VIDEO)) {
        return true;
    }
    size_t source_length = 0;
    const char *source = document_node_is_tag(node, LXB_TAG_VIDEO)
        ? document_attribute(node, "poster", &source_length)
        : image_select_source(context->stylesheet, node, &source_length);
    if (document_node_is_tag(node, LXB_TAG_VIDEO)
        && (source == NULL || source_length == 0u)) {
        const MediaDeclaredVideo *declared =
            media_declared_video_cached(context->document);
        source = declared == NULL ? NULL : declared->thumbnail_url;
        source_length = source == NULL ? 0u : strlen(source);
    }
    return load_document_image_node(
        context, node, source, source_length, false, false, PSEUDO_NONE);
}

static bool image_process_refresh_node(
    ImageLoadContext *context, lxb_dom_node_t *node)
{
    enum { MAXIMUM_ANCESTORS = 64 };
    if (context == NULL || node == NULL) return false;
    lxb_dom_node_t *ancestors[MAXIMUM_ANCESTORS];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == MAXIMUM_ANCESTORS) return false;
        ancestors[count++] = at;
    }
    /* A retained layout entry is keyed by the exact inherited style. Use
       the same html inheritance root as complete image discovery/layout,
       not the uncached embedder entry point's NULL parent. */
    ComputedStyle parent = context->style_cache == NULL
        ? (ComputedStyle) {0} : layout_initial_root_style();
    bool has_parent = context->style_cache != NULL;
    if (context->refresh_document_generation
            != context->document->content_generation
        || context->refresh_stylesheet_generation
            != context->stylesheet->build_generation
        || context->refresh_container_generation
            != context->stylesheet->container_state_generation) {
        context->refresh_ancestor_count = 0;
        context->refresh_document_generation = context->document->content_generation;
        context->refresh_stylesheet_generation = context->stylesheet->build_generation;
        context->refresh_container_generation = context->stylesheet->container_state_generation;
    }
    size_t depth = 0;
    while (count != 0) {
        lxb_dom_node_t *ancestor = ancestors[--count];
        if (depth < context->refresh_ancestor_count
            && context->refresh_ancestors[depth].node == ancestor) {
            parent = context->refresh_ancestors[depth].style;
            context->images->stats.node_style_cache_hits++;
        } else {
            /* Once the paths diverge, even a later equal pointer must not
               reuse a result inherited from the old path. */
            if (depth < context->refresh_ancestor_count)
                context->refresh_ancestor_count = depth;
            if (context->style_cache != NULL) {
                ComputedStyle resolved;
                bool hit = layout_reuse_cache_resolve_style(
                    context->style_cache, context->stylesheet, context->fonts,
                    ancestor, has_parent ? &parent : NULL, &resolved);
                if (hit) context->images->stats.node_style_cache_hits++;
                else context->images->stats.node_style_cache_misses++;
                parent = resolved;
            } else {
                parent = style_for_node(context->stylesheet, ancestor,
                                        has_parent ? &parent : NULL);
            }
            if (depth < context->refresh_ancestor_capacity) {
                context->refresh_ancestors[depth] = (ImageRefreshAncestorStyle) {
                    .node = ancestor, .style = parent
                };
                context->refresh_ancestor_count = depth + 1u;
            }
        }
        depth++;
        has_parent = true;
        /* A complete traversal would never reach a descendant of a hidden
           element. Refreshing one exact resource must retain that rule. */
        if (parent.display == DISPLAY_NONE || parent.hidden) return true;
    }
    ComputedStyle style = {0};
    bool traverse = true;
    return image_process_node(
        context, node, has_parent ? &parent : NULL, &style, &traverse);
}

static bool image_process_priority_target(
    ImageLoadContext *context, const ImagePriorityTarget *target)
{
    if (context == NULL || target == NULL || target->node == NULL) {
        return false;
    }
    if (target->kind == IMAGE_PRIORITY_KIND_LINKED_VIDEO) {
        context->current_display_width = target->display_width;
        context->current_display_height = target->display_height;
        bool loaded = load_document_image_node(context, target->node,
            target->source, target->source == NULL ? 0 : strlen(target->source),
            false, false, PSEUDO_NONE);
        context->current_display_width = 0;
        context->current_display_height = 0;
        return loaded;
    }
    if (target->kind == IMAGE_PRIORITY_KIND_DOCUMENT
        || target->kind == IMAGE_PRIORITY_KIND_PRELOAD) {
        uint16_t display_width = target->display_width;
        uint16_t display_height = target->display_height;
        size_t source_length = 0;
        const char *source = document_node_is_tag(target->node, LXB_TAG_IMG)
            ? image_select_source_for_width(
                context->stylesheet, target->node,
                target->kind == IMAGE_PRIORITY_KIND_PRELOAD ? 0 : display_width,
                &source_length)
            : NULL;
        /* A shared response must satisfy every alias. Resolve the complete
           bounded priority set before the first request so traversal order
           cannot leave a later, larger box backed by an undersized bitmap. */
        if (source != NULL && source_length != 0) {
            for (size_t i = 0; i < context->priority_target_count; i++) {
                const ImagePriorityTarget *other =
                    &context->priority_targets[i];
                if ((other->kind != IMAGE_PRIORITY_KIND_DOCUMENT
                        && other->kind != IMAGE_PRIORITY_KIND_PRELOAD)
                    || !document_node_is_tag(other->node, LXB_TAG_IMG)) continue;
                size_t other_length = 0;
                const char *other_source = image_select_source_for_width(
                    context->stylesheet, other->node,
                    other->kind == IMAGE_PRIORITY_KIND_PRELOAD ? 0 : other->display_width,
                    &other_length);
                if (other_source == NULL || other_length != source_length
                    || memcmp(other_source, source, source_length) != 0) {
                    continue;
                }
                if (other->display_width == 0
                    || other->display_height == 0) {
                    display_width = 0;
                    display_height = 0;
                    break;
                }
                if (other->display_width > display_width) {
                    display_width = other->display_width;
                }
                if (other->display_height > display_height) {
                    display_height = other->display_height;
                }
            }
        }
        context->current_display_width = display_width;
        context->current_display_height = display_height;
        bool loaded;
        if (source != NULL) {
            loaded = load_document_image_node(
                context, target->node, source, source_length,
                false, false, PSEUDO_NONE);
        } else {
            loaded = image_process_priority_node(context, target->node);
        }
        context->current_display_width = 0;
        context->current_display_height = 0;
        return loaded;
    }
    bool is_mask = target->kind == IMAGE_PRIORITY_KIND_MASK;
    bool is_background =
        target->kind == IMAGE_PRIORITY_KIND_BACKGROUND;
    if ((!is_mask && !is_background) || target->source == NULL
        || target->source[0] == '\0'
        || target->pseudo > (uint8_t) PSEUDO_AFTER) return false;
    return load_stylesheet_image_node(
        context, target->node, target->source, strlen(target->source),
        is_mask, is_background, (PseudoElement) target->pseudo);
}

static bool walk_images(ImageLoadContext *context, lxb_dom_node_t *node,
                        const ComputedStyle *parent)
{
    ImageTraversalEntry *entries = NULL;
    size_t count = 0, capacity = 0, visited = 0;
    bool ok = image_traversal_push(
        context, &entries, &count, &capacity, node, parent);
    while (ok && count != 0) {
        ImageTraversalEntry entry = entries[--count];
        node = entry.node;
        if (++visited > IMAGE_TRAVERSAL_NODE_LIMIT) {
            ok = false;
            break;
        }
        const ComputedStyle *node_parent = entry.has_parent
            ? &entry.parent : NULL;
        ComputedStyle style;
        bool traverse = true;
        if (!image_work(context, 1, false, "resource")
            || !image_process_node(
                context, node, node_parent, &style, &traverse)) {
            ok = false;
            break;
        }
        const ComputedStyle *child_parent =
            node->type == LXB_DOM_NODE_TYPE_ELEMENT ? &style : node_parent;
        /* Push the sibling first so the child remains the next item in
           document order. Each pending branch owns the exact inherited style
           it needs, so no C recursion or fixed stack-sized style array is
           required. */
        ok = image_traversal_push(
            context, &entries, &count, &capacity, node->next, node_parent);
        if (ok && traverse) {
            ok = image_traversal_push(
                context, &entries, &count, &capacity, node->first_child,
                child_parent);
        }
    }
    budget_free(context->budget, entries);
    return ok;
}

static void images_rollback_optional_suffix(
    ImageResources *images, size_t retained_count,
    const ExternalImageStats *retained_stats, bool retained_priority_staged,
    uint32_t retained_failure_serial)
{
    if (images == NULL || images->budget == NULL || retained_stats == NULL
        || retained_count > images->count) return;
    image_failures_rollback(images, retained_count, retained_failure_serial);
    for (size_t i = retained_count; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        image_canvas_native_forget(item);
        image_resource_release_owned_pixels(images->budget, item);
        if (item->owns_encoded) {
            if (item->encoded_body != NULL) {
                browser_shared_body_release(item->encoded_body);
            } else {
                budget_free(images->budget, item->encoded);
            }
        }
    }
    images->count = retained_count;
    images->stats = *retained_stats;
    images->stats.priority_retained_on_failure++;
    images->priority_staged = retained_priority_staged;
}

static bool images_load_external_impl(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *priority_nodes,
    size_t priority_node_count, const ImagePriorityTarget *priority_targets,
    size_t priority_target_count, bool full_traversal,
    bool refresh_complete_nodes, Budget *budget,
    const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    bool defer_document_images, lxb_dom_node_t *const *deferred_nodes,
    size_t deferred_node_count, const ImageResources *refresh_previous)
{
    if (images != NULL) images->stats.load_cancelled = false;
    if (deferred_node_count > 128u
        || (deferred_node_count != 0 && deferred_nodes == NULL)) return false;
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || images == NULL
        || budget == NULL || base_url == NULL || document_url == NULL
        || maximum_count == 0
        || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0 || maximum_decoded_bytes == 0
        || timeout_ms <= 0
        || !images_decode_worker_wait_cancelled(budget)) return false;
#ifndef __PSP__
    if (tilefinch_test_faults()->refuse_next_image_load_setup) {
        tilefinch_test_faults()->refuse_next_image_load_setup = false;
        return false;
    }
#endif
    /* A caller may request more images than the engine can track (a
       reference-scoring profile can raise the count to fit a busy 106-image
       page).  Exceeding a limit must soft-skip, never fail the
       navigation: the per-image budgets already skip past their ceilings, so
       clamp network attempts rather than returning false. The independently
       bounded node table also admits aliases that share decoded pixels. */
    if (maximum_count > MAX_IMAGE_FETCH_ATTEMPTS) {
        maximum_count = MAX_IMAGE_FETCH_ATTEMPTS;
    }
    /* Selected refresh prepared this cache against the stable outgoing table
       before creating its transactional replacement. Never retain a pointer
       to that stack-local temporary in the page's cache. */
    if (!refresh_complete_nodes) {
        if (full_traversal)
            layout_reuse_cache_expect_elements(style_cache,
                                               document->element_count);
        layout_reuse_cache_prepare(
            style_cache, stylesheet, fonts, images, viewport_width);
    }
    bool owns_scheduler = scheduler == NULL;
    if (owns_scheduler) {
        size_t reserved = maximum_single_encoded_bytes;
        if (reserved <= SIZE_MAX / IMAGE_FETCH_CONCURRENCY) {
            reserved *= IMAGE_FETCH_CONCURRENCY;
        } else {
            reserved = SIZE_MAX;
        }
        scheduler = fetch_scheduler_create(
            budget, IMAGE_FETCH_CONCURRENCY, reserved);
        if (scheduler == NULL) return false;
    }
    if (images->budget == NULL) {
        memset(images, 0, sizeof(*images));
        images->budget = budget;
    } else if (images->budget != budget) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    size_t retained_count = images->count;
    ExternalImageStats retained_stats = images->stats;
    bool retained_priority_staged = images->priority_staged;
    uint32_t retained_failure_serial = images->failure_serial;
    if (!images->priority_staged) {
        images->stats.max_slice_us = 0;
        images->stats.work_units = 0;
        images->stats.max_slice_work_units = 0;
        images->stats.cooperative_yields = 0;
    }
    double started_ms = image_now_ms();
    ImageLoadContext context = {
        .document = document,
        .images = images,
        .stylesheet = stylesheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .referrer_policy = referrer_policy,
        .maximum_count = maximum_count,
        .maximum_total_encoded_bytes = maximum_total_encoded_bytes,
        .maximum_single_encoded_bytes = maximum_single_encoded_bytes,
        .maximum_decoded_bytes = maximum_decoded_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .style_cache = style_cache,
        .fonts = fonts,
        .viewport_width = viewport_width > 0
            ? viewport_width : stylesheet->viewport_width,
        .priority_targets = priority_targets,
        .priority_target_count = priority_target_count,
        .preserve_staged_document_images =
            full_traversal && images->priority_staged,
        .defer_document_images = defer_document_images,
        .deferred_nodes = deferred_nodes,
        .deferred_node_count = deferred_node_count,
        .refresh_previous = refresh_complete_nodes || full_traversal
            ? refresh_previous : NULL,
        .refresh_retires_all = full_traversal && refresh_previous != NULL,
        .refresh_retired_nodes = priority_nodes,
        .refresh_retired_count = priority_node_count,
        .deadline_ms = image_now_ms() + (double) timeout_ms,
        .slice_started_us = tilefinch_platform_monotonic_time_us(),
        .eager_decode_rasters = !full_traversal
            && (priority_node_count != 0 || priority_target_count != 0)
    };
    uint64_t traversal_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    /* Large immutable resource walks and multi-node refresh batches share
       layout's bounded variable/selector memos. Refreshes otherwise repeat
       inherited variable resolution through the same ancestors for each SVG.
       Small sheets and one-node priority loads keep the allocation-free path.
       Refusal preserves uncached matching; nested owners keep their cache,
       and cleanup covers cancellation and every early return. */
    bool cache_styles = (full_traversal
        || (refresh_complete_nodes && priority_node_count > 1u))
        && stylesheet->count >= 64u;
    ImageStyleCacheScope style_scope
        __attribute__((cleanup(image_style_cache_scope_end))) = {
            .sheet = stylesheet, .budget = budget,
            .owned = cache_styles && style_variable_cache_begin(stylesheet, budget)
        };
    if (cache_styles) {
        style_scope.ancestors = budget_calloc_category(budget,
            BUDGET_CATEGORY_RESOURCE, 1, sizeof(*style_scope.ancestors));
        style_scope.selector_owned = style_selector_cooperation_begin(
            stylesheet, image_selector_cooperate, &context, style_scope.ancestors);
    }
    if (cache_styles && style_cache == NULL && refresh_complete_nodes
        && priority_node_count > 1u
        && priority_nodes[0] != NULL) {
        size_t capacity = 0;
        for (lxb_dom_node_t *at = priority_nodes[0]->parent;
             at != NULL && capacity < IMAGE_REFRESH_ANCESTOR_LIMIT;
             at = at->parent) {
            if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) capacity++;
        }
        if (capacity != 0) {
            style_scope.refresh_ancestors = budget_malloc_category(
                budget, BUDGET_CATEGORY_RESOURCE,
                capacity * sizeof(*style_scope.refresh_ancestors));
            if (style_scope.refresh_ancestors != NULL) {
                context.refresh_ancestors = style_scope.refresh_ancestors;
                context.refresh_ancestor_capacity = capacity;
            }
        }
    }
    bool traversed = true;
    if (full_traversal) {
        /* Match layout's exact html/body inheritance roots. Starting at the
           document node gives every descendant a different parent hash and
           makes otherwise identical computed styles unsafe to hand off. */
        lxb_dom_node_t *body = document_body_node(document);
        lxb_dom_node_t *html = body == NULL ? NULL : body->parent;
        ComputedStyle root = layout_initial_root_style();
        ComputedStyle html_style = root;
        bool html_traverse = true;
        if (html != NULL && html->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_node_is_tag(html, LXB_TAG_HTML)) {
            traversed = image_process_node(
                &context, html, &root, &html_style, &html_traverse);
        }
        if (traversed && html_traverse) {
            traversed = walk_images(&context, body, &html_style);
        }
    } else {
        for (size_t i = 0; traversed && i < priority_node_count; i++) {
            lxb_dom_node_t *node = priority_nodes[i];
            if (node == NULL
                || !image_work(&context, 1, false, "resource")) {
                traversed = node == NULL;
                continue;
            }
            traversed = refresh_complete_nodes
                ? image_process_refresh_node(&context, node)
                : image_process_priority_node(&context, node);
        }
        for (size_t i = 0; traversed && i < priority_target_count; i++) {
            if (!image_work(&context, 1, false, "resource")) {
                traversed = false;
                break;
            }
            traversed = image_process_priority_target(
                &context, &priority_targets[i]);
        }
    }
    if (style_selector_cooperation_cancelled(stylesheet)) traversed = false;
    if (!traversed) {
        cancel_pending(&context);
        if (retained_count != 0) {
            images_rollback_optional_suffix(
                images, retained_count, &retained_stats,
                retained_priority_staged, retained_failure_serial);
        } else {
            images_destroy(images);
        }
        images->stats.load_cancelled = context.cooperate_cancelled;
        budget_free(budget, context.request_scratch);
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    if (traversal_started != 0) {
        images->stats.traversal_us +=
            image_profile_now_us() - traversal_started;
    }
    uint64_t drain_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    while (context.pending_count != 0) {
        if (!finish_one_pending(&context, true)) {
            cancel_pending(&context);
            if (retained_count != 0) {
                images_rollback_optional_suffix(
                    images, retained_count, &retained_stats,
                    retained_priority_staged, retained_failure_serial);
            } else {
                images_destroy(images);
            }
            images->stats.load_cancelled = context.cooperate_cancelled;
            if (owns_scheduler) fetch_scheduler_destroy(scheduler);
            budget_free(budget, context.request_scratch);
            return false;
        }
    }
    if (drain_started != 0) {
        images->stats.drain_us += image_profile_now_us() - drain_started;
    }
    image_finish_slice(&context);
    images->stats.deadline_cancelled = context.deadline_cancelled ? 1u : 0u;
    images->stats.deadline_exceeded = image_now_ms() >= context.deadline_ms;
    double elapsed = image_now_ms() - started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    images->stats.elapsed_ms += (uint64_t) elapsed;
    budget_free(budget, context.request_scratch);
    if (owns_scheduler) fetch_scheduler_destroy(scheduler);
    return true;
}

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
                          BrowserSession *session)
{
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0, false, NULL, 0, NULL);
    if (images != NULL) images->priority_staged = false;
    return loaded;
}

bool images_load_external_reusing_layout_styles(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width)
{
    return images_load_external_reusing_layout_styles_deferred(
        document, stylesheet, images, budget, base_url, document_url,
        referrer_policy, maximum_count, maximum_total_encoded_bytes,
        maximum_single_encoded_bytes, maximum_decoded_bytes, timeout_ms,
        scheduler, session, style_cache, fonts, viewport_width, false);
}

bool images_load_external_reusing_layout_styles_deferred(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    bool defer_document_images)
{
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        style_cache, fonts, viewport_width, defer_document_images, NULL, 0, NULL);
    if (images != NULL) images->priority_staged = false;
    return loaded;
}

bool images_load_external_reusing_layout_styles_excluding(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    lxb_dom_node_t *const *deferred_nodes, size_t deferred_node_count)
{
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        style_cache, fonts, viewport_width, false, deferred_nodes, deferred_node_count, NULL);
    if (images != NULL) images->priority_staged = false;
    return loaded;
}

static bool image_refresh_lenders_present(
    const ImageResources *images, const ImageResources *replacement);

/* Before the rebuild loads its replacement, the outgoing table keeps only
   what the load can lend: plain inline-SVG rasters. Every other entry (an
   external image, a background, a mask, a canvas) is decoded or rebuilt
   again by the load, exactly as after the destroy-first rebuild, so its
   resources are released now rather than held beside their replacements;
   the rebuild's peak then exceeds the destroy-first one by at most the
   rasters that were not taken over. A released entry's surface that a kept
   raster shares passes to the kept entry first. */
static bool image_rebuild_lendable(const ImageResource *item, bool video_only)
{
    return image_refresh_svg_lendable(item)
        && (!video_only || document_node_is_tag(item->node, LXB_TAG_VIDEO));
}

static void image_rebuild_release_unlendable(ImageResources *images,
                                             bool video_only)
{
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (image_rebuild_lendable(item, video_only) || !image_owns_surface(item))
            continue;
        for (size_t at = 0; at < images->count; at++) {
            ImageResource *kept = &images->items[at];
            if (image_resource_backing_identity(kept)
                    == image_resource_backing_identity(item)
                && image_rebuild_lendable(kept, video_only)) {
                image_move_surface(item, kept);
                break;
            }
        }
    }
    size_t write = 0;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource item = images->items[i];
        if (image_rebuild_lendable(&item, video_only)) {
            images->items[write++] = item;
            continue;
        }
        image_canvas_native_forget(&item);
        image_resource_release_owned_pixels(images->budget, &item);
        if (item.owns_encoded) {
            if (item.encoded_body != NULL)
                browser_shared_body_release(item.encoded_body);
            else
                budget_free(images->budget, item.encoded);
        }
    }
    images->count = write;
    for (size_t i = 0; i < 2u; i++) {
        budget_free(images->budget, images->canvas_depth[i].values);
        memset(&images->canvas_depth[i], 0, sizeof(images->canvas_depth[i]));
    }
}

void images_prepare_external_rebuild(ImageResources *images)
{
    if (images == NULL) return;
    bool video = false;
    for (size_t i = 0; i < images->count && !video; i++)
        video = image_rebuild_lendable(&images->items[i], true);
    if (video) image_rebuild_release_unlendable(images, true);
    else images_destroy(images);
}

void images_retire_subtree(ImageResources *images,
                           const lxb_dom_node_t *root)
{
    if (images == NULL || root == NULL) return;
    for (size_t i = 0; i < images->count; i++) {
        size_t depth = 0;
        for (const lxb_dom_node_t *at = images->items[i].node;
             at != NULL; at = at->parent) {
            /* A bounded-out walk is conservatively retired, never retained
               as a potentially dangling pointer. Template fragments are
               announced separately by the ownership-tree destructor. */
            if (at == root || ++depth >= 4096u) {
                images->items[i].node = NULL;
                break;
            }
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < images->failure_count; i++) {
        size_t depth = 0;
        const lxb_dom_node_t *at = images->failures[i].node;
        while (at != NULL && at != root && ++depth < 4096u) at = at->parent;
        if (at == NULL) images->failures[kept++] = images->failures[i];
    }
    images->failure_count = kept;
    for (size_t i = 0; i < 2u; i++) {
        size_t depth = 0;
        for (const lxb_dom_node_t *at = images->canvas_depth[i].node;
             at != NULL; at = at->parent) {
            if (at == root || ++depth >= 4096u) {
                budget_free(images->budget, images->canvas_depth[i].values);
                memset(&images->canvas_depth[i], 0,
                       sizeof(images->canvas_depth[i]));
                break;
            }
        }
    }
}

bool images_rebuild_external_reusing_rasters(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width,
    lxb_dom_node_t *const *deferred_nodes, size_t deferred_node_count)
{
    if (images == NULL || budget == NULL
        || (images->budget != NULL && images->budget != budget)) return false;
    ImageResources replacement = {.budget = budget};
    const ImageResources *lenders = images->count == 0 ? NULL : images;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_SVG_REFRESH_REUSE") != NULL) lenders = NULL;
#endif
    if (lenders == NULL) images_destroy(images);
    else image_rebuild_release_unlendable(images, false);
    bool loaded = images_load_external_impl(
        document, stylesheet, &replacement, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        style_cache, fonts, viewport_width, false, deferred_nodes,
        deferred_node_count, lenders);
    replacement.priority_staged = false;
    if (loaded && !image_refresh_lenders_present(images, &replacement))
        loaded = false;
    if (!loaded) {
        /* No borrowed surface may outlive this call: clear the borrowers'
           claims before releasing the replacement's own resources. */
        for (size_t i = 0; i < replacement.count; i++) {
            if (replacement.items[i].borrows_previous)
                replacement.items[i].owns_pixels = false;
        }
        images_destroy(&replacement);
        images_destroy(images);
        return false;
    }
    /* The first borrower of each outgoing surface becomes its owner; the
       outgoing table then releases only what nobody borrowed. */
    for (size_t i = 0; i < replacement.count; i++) {
        ImageResource *borrowed = &replacement.items[i];
        if (!borrowed->borrows_previous) continue;
        borrowed->borrows_previous = false;
        for (size_t at = 0; at < images->count; at++) {
            ImageResource *lender = &images->items[at];
            if (image_owns_surface(lender)
                && image_resource_backing_identity(lender)
                       == image_resource_backing_identity(borrowed)) {
                image_move_surface(lender, borrowed);
                break;
            }
        }
    }
    replacement.retarget = images->retarget;
    images_destroy(images);
    *images = replacement;
    return true;
}

bool images_load_external_priority_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes, size_t maximum_single_encoded_bytes,
    size_t maximum_decoded_bytes, long timeout_ms, FetchScheduler *scheduler,
    BrowserSession *session)
{
    if (node_count == 0) return true;
    if (nodes == NULL) return false;
    bool loaded = images_load_external_impl(
        document, stylesheet, images, nodes, node_count, NULL, 0,
        false, false, budget, base_url, document_url, referrer_policy,
        maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0, false, NULL, 0, NULL);
    if (loaded && images != NULL) images->priority_staged = true;
    return loaded;
}

static bool image_node_in_refresh_set(
    const lxb_dom_node_t *node, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i] == node) return true;
    }
    return false;
}

static size_t image_refresh_owned_encoded_bytes(
    const ImageResources *images, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->owns_encoded
            || !image_node_in_refresh_set(
                item->node, nodes, node_count)) continue;
        bool retained_alias = false;
        for (size_t alias = 0; alias < images->count; alias++) {
            const ImageResource *candidate = &images->items[alias];
            if (alias != i
                && !image_node_in_refresh_set(
                    candidate->node, nodes, node_count)
                && candidate->encoded == item->encoded
                && candidate->encoded_body == item->encoded_body) {
                retained_alias = true;
                break;
            }
        }
        if (!retained_alias && item->encoded_length <= SIZE_MAX - bytes) {
            bytes += item->encoded_length;
        }
    }
    return bytes;
}

static size_t image_refresh_owned_decoded_bytes(
    const ImageResources *images, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->owns_pixels
            || !image_node_in_refresh_set(item->node, nodes, node_count)
            || item->width <= 0 || item->height <= 0
            || (size_t) item->width > SIZE_MAX / (size_t) item->height
            || (size_t) item->width * (size_t) item->height
                   > SIZE_MAX / 4u) continue;
        bool retained_alias = false;
        for (size_t alias = 0; alias < images->count; alias++) {
            const ImageResource *candidate = &images->items[alias];
            if (alias != i
                && !image_node_in_refresh_set(
                    candidate->node, nodes, node_count)
                && candidate->pixels == item->pixels) {
                retained_alias = true;
                break;
            }
        }
        if (retained_alias) continue;
        size_t item_bytes =
            (size_t) item->width * (size_t) item->height * 4u;
        if (item_bytes <= SIZE_MAX - bytes) bytes += item_bytes;
    }
    return bytes;
}

/* Each inline-SVG raster owner consumed one attempt from the page's image
   count when it was rasterized (or adopted by a refresh). Retiring it
   returns that attempt as its decoded bytes are returned; otherwise every
   refresh of an unchanged icon would ratchet the count until later
   refreshes skip the icons outright. */
static size_t image_refresh_owned_svg_attempts(
    const ImageResources *images, lxb_dom_node_t *const *nodes,
    size_t node_count, size_t *exempt)
{
    size_t attempts = 0;
    *exempt = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!image_owns_surface(item) || !image_refresh_svg_lendable(item)
            || !image_node_in_refresh_set(item->node, nodes, node_count)) {
            continue;
        }
        bool retained_alias = false;
        for (size_t alias = 0; alias < images->count; alias++) {
            const ImageResource *candidate = &images->items[alias];
            if (alias != i
                && !image_node_in_refresh_set(
                    candidate->node, nodes, node_count)
                && image_resource_backing_identity(candidate)
                       == image_resource_backing_identity(item)) {
                retained_alias = true;
                break;
            }
        }
        if (retained_alias) continue;
        attempts++;
        if (item->reused_raster_count_exempt || image_svg_first_raster_bytes(item)
                <= IMAGE_COUNT_EXEMPT_DECODED_BYTES) {
            (*exempt)++;
        }
    }
    return attempts;
}

static void image_refresh_transfer_old_ownership(
    ImageResources *images, size_t owner_index,
    lxb_dom_node_t *const *nodes, size_t node_count)
{
    ImageResource *owner = &images->items[owner_index];
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *candidate = &images->items[i];
        if (i == owner_index
            || image_node_in_refresh_set(
                   candidate->node, nodes, node_count)) continue;
        if (owner->owns_pixels && candidate->pixels == owner->pixels) {
            candidate->owns_pixels = true;
            owner->owns_pixels = false;
        }
        if (owner->owns_encoded
            && candidate->encoded == owner->encoded
            && candidate->encoded_body == owner->encoded_body) {
            candidate->owns_encoded = true;
            owner->owns_encoded = false;
        }
        if (!owner->owns_pixels && !owner->owns_encoded) break;
    }
}

/* Cooperation inside the refresh walk may run other owners of the outgoing
   table. Before the replacement is published, every borrowed surface must
   still be held there under the same identity; otherwise refuse the refresh
   (the caller falls back to a full rebuild) rather than publish a pointer
   that may have been released. */
static bool image_refresh_lenders_present(
    const ImageResources *images, const ImageResources *replacement)
{
    for (size_t i = 0; i < replacement->count; i++) {
        const ImageResource *borrowed = &replacement->items[i];
        if (!borrowed->borrows_previous) continue;
        bool present = false;
        for (size_t at = 0; !present && at < images->count; at++) {
            const ImageResource *lender = &images->items[at];
            present = image_resource_backing_identity(lender)
                    == image_resource_backing_identity(borrowed)
                && lender->url_hash == borrowed->url_hash
                && lender->width == borrowed->width
                && lender->height == borrowed->height
                && image_refresh_svg_lendable(lender);
        }
        if (!present) return false;
    }
    return true;
}

/* Runs after retiring entries have handed their surfaces to retained
   aliases. A surface a retiring entry still owns would be freed with it, so
   the first borrower becomes its owner; later borrowers of the same surface
   stay aliases of that one. Surfaces owned elsewhere remain plain aliases. */
static void image_refresh_adopt_lent_surfaces(
    ImageResources *images, ImageResources *replacement,
    lxb_dom_node_t *const *nodes, size_t node_count)
{
    for (size_t i = 0; i < replacement->count; i++) {
        ImageResource *borrowed = &replacement->items[i];
        if (!borrowed->borrows_previous) continue;
        borrowed->borrows_previous = false;
        for (size_t at = 0; at < images->count; at++) {
            ImageResource *lender = &images->items[at];
            if (image_owns_surface(lender)
                && image_resource_backing_identity(lender)
                       == image_resource_backing_identity(borrowed)
                && image_node_in_refresh_set(
                    lender->node, nodes, node_count)) {
                image_move_surface(lender, borrowed);
                break;
            }
        }
    }
}

/* Drops the failures of `nodes` (ImageResources.failures). */
static void image_failures_forget_nodes(
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count)
{
    size_t kept = 0;
    for (size_t i = 0; i < images->failure_count; i++)
        if (!image_node_in_refresh_set(images->failures[i].node, nodes,
                                       node_count))
            images->failures[kept++] = images->failures[i];
    images->failure_count = kept;
}

void images_discard_nodes(
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count)
{
    if (images == NULL || images->budget == NULL || nodes == NULL
        || node_count == 0u) return;
    size_t retired_encoded = image_refresh_owned_encoded_bytes(
        images, nodes, node_count);
    size_t retired_decoded = image_refresh_owned_decoded_bytes(
        images, nodes, node_count);
    for (size_t i = 0; i < images->count; i++) {
        if (image_node_in_refresh_set(
                images->items[i].node, nodes, node_count)) {
            image_refresh_transfer_old_ownership(
                images, i, nodes, node_count);
        }
    }
    size_t write = 0u;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource item = images->items[i];
        if (!image_node_in_refresh_set(item.node, nodes, node_count)) {
            images->items[write++] = item;
            continue;
        }
        image_canvas_native_forget(&item);
        image_resource_release_owned_pixels(images->budget, &item);
        if (item.owns_encoded) {
            if (item.encoded_body != NULL)
                browser_shared_body_release(item.encoded_body);
            else
                budget_free(images->budget, item.encoded);
        }
    }
    images->count = write;
    image_failures_forget_nodes(images, nodes, node_count);
    images->stats.encoded_bytes = retired_encoded
            <= images->stats.encoded_bytes
        ? images->stats.encoded_bytes - retired_encoded : 0u;
    images->stats.decoded_bytes = retired_decoded
            <= images->stats.decoded_bytes
        ? images->stats.decoded_bytes - retired_decoded : 0u;
}

bool images_refresh_external_nodes_reusing_layout_styles(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width)
{
    if (node_count == 0) return true;
    if (document == NULL || stylesheet == NULL || images == NULL
        || nodes == NULL || budget == NULL || images->budget != budget) {
        return false;
    }
    layout_reuse_cache_prepare(
        style_cache, stylesheet, fonts, images, viewport_width);
    size_t retired_encoded = image_refresh_owned_encoded_bytes(
        images, nodes, node_count);
    size_t retired_decoded = image_refresh_owned_decoded_bytes(
        images, nodes, node_count);
    size_t retired_exempt = 0;
    size_t retired_attempts = image_refresh_owned_svg_attempts(
        images, nodes, node_count, &retired_exempt);
    ImageResources replacement = {
        .budget = budget,
        .stats = images->stats,
        .priority_staged = true
    };
    replacement.stats.encoded_bytes =
        retired_encoded <= replacement.stats.encoded_bytes
        ? replacement.stats.encoded_bytes - retired_encoded : 0;
    replacement.stats.decoded_bytes =
        retired_decoded <= replacement.stats.decoded_bytes
        ? replacement.stats.decoded_bytes - retired_decoded : 0;
    replacement.stats.attempted =
        retired_attempts <= replacement.stats.attempted
        ? replacement.stats.attempted - retired_attempts : 0;
    replacement.stats.count_exempt =
        retired_exempt <= replacement.stats.count_exempt
        ? replacement.stats.count_exempt - retired_exempt : 0;
    const ImageResources *lenders = images;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_SVG_REFRESH_REUSE") != NULL) lenders = NULL;
#endif
    if (!images_load_external_impl(
            document, stylesheet, &replacement, nodes, node_count, NULL, 0,
            false, true, budget, base_url, document_url, referrer_policy,
            maximum_count, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_decoded_bytes, timeout_ms,
            scheduler, session, style_cache, fonts, viewport_width,
            false, NULL, 0, lenders)) {
        /* A cooperative cancellation can interrupt selector resolution. No
           partially resolved entry may survive into a later retry. */
        layout_reuse_cache_reset(style_cache);
        images_destroy(&replacement);
        return false;
    }
    if (!image_refresh_lenders_present(images, &replacement)) {
        images_destroy(&replacement);
        return false;
    }

    size_t retained_count = 0;
    for (size_t i = 0; i < images->count; i++) {
        if (!image_node_in_refresh_set(
                images->items[i].node, nodes, node_count)) retained_count++;
    }
    if (replacement.count > MAX_TRACKED_IMAGE_NODES - retained_count) {
        images_destroy(&replacement);
        return false;
    }
    size_t wanted = retained_count + replacement.count;
    if (wanted > images->capacity) {
        size_t capacity = images->capacity == 0 ? 16 : images->capacity;
        while (capacity < wanted && capacity < MAX_TRACKED_IMAGE_NODES) {
            size_t grown = capacity <= MAX_TRACKED_IMAGE_NODES / 2u
                ? capacity * 2u : MAX_TRACKED_IMAGE_NODES;
            if (grown <= capacity) break;
            capacity = grown;
        }
        ImageResource *grown = budget_realloc(
            budget, images->items, capacity * sizeof(*grown));
        if (grown == NULL) {
            images_destroy(&replacement);
            return false;
        }
        images->items = grown;
        images->capacity = capacity;
    }

    for (size_t i = 0; i < images->count; i++) {
        if (image_node_in_refresh_set(
                images->items[i].node, nodes, node_count)) {
            image_refresh_transfer_old_ownership(
                images, i, nodes, node_count);
        }
    }
    image_refresh_adopt_lent_surfaces(images, &replacement, nodes, node_count);
    size_t write = 0;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource item = images->items[i];
        if (image_node_in_refresh_set(item.node, nodes, node_count)) {
            image_canvas_native_forget(&item);
            image_resource_release_owned_pixels(budget, &item);
            if (item.owns_encoded) {
                if (item.encoded_body != NULL) {
                    browser_shared_body_release(item.encoded_body);
                } else {
                    budget_free(budget, item.encoded);
                }
            }
            continue;
        }
        images->items[write++] = item;
    }
    if (replacement.count != 0) {
        memcpy(images->items + write, replacement.items,
               replacement.count * sizeof(*replacement.items));
        write += replacement.count;
    }
    images->count = write;
    images->stats = replacement.stats;
    images->priority_staged = false;
    /* The refreshed nodes' failures are the replacement's now. */
    image_failures_forget_nodes(images, nodes, node_count);
    for (size_t i = 0; i < replacement.failure_count; i++)
        image_note_failure(images, budget, replacement.failures[i].node,
                           replacement.failures[i].source_hash, false, false,
                           PSEUDO_NONE);
    budget_free(budget, replacement.failures);
    replacement.failures = NULL;
    replacement.failure_count = replacement.failure_capacity = 0;
    budget_free(budget, replacement.items);
    replacement.items = NULL;
    replacement.count = replacement.capacity = 0;
    return true;
}

bool images_refresh_external_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    return images_refresh_external_nodes_reusing_layout_styles(
        document, stylesheet, images, nodes, node_count, budget, base_url,
        document_url, referrer_policy, maximum_count, maximum_total_encoded_bytes,
        maximum_single_encoded_bytes, maximum_decoded_bytes, timeout_ms,
        scheduler, session, NULL, NULL, 0);
}

bool images_load_external_priority_targets(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *targets,
    size_t target_count, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    if (target_count == 0) return true;
    if (targets == NULL) return false;
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, targets, target_count,
        false, false, budget, base_url, document_url, referrer_policy,
        maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0, false, NULL, 0, NULL);
    if (loaded && images != NULL) images->priority_staged = true;
    return loaded;
}

static ImagePriorityLoadStatus image_priority_load_fail(
    ImagePriorityLoadJob *job)
{
    if (job == NULL) return IMAGE_PRIORITY_LOAD_FAILED;
    cancel_pending(&job->context);
    if (!job->rolled_back) {
        images_rollback_optional_suffix(
            job->context.images, job->retained_count,
            &job->retained_stats, job->retained_priority_staged,
            job->retained_failure_serial);
        job->rolled_back = true;
    }
    job->terminal = true;
    job->failed = true;
    return IMAGE_PRIORITY_LOAD_FAILED;
}

ImagePriorityLoadJob *images_priority_load_begin(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *target,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    return images_priority_load_begin_batch(
        document, stylesheet, images, target, 1u, budget, base_url,
        document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session);
}

ImagePriorityLoadJob *images_priority_load_begin_batch(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *targets,
    size_t target_count, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || images == NULL || targets == NULL || target_count == 0
        || target_count > IMAGE_PRIORITY_LOAD_BATCH_LIMIT
        || budget == NULL || base_url == NULL || document_url == NULL
        || maximum_count == 0 || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0 || maximum_decoded_bytes == 0
        || timeout_ms <= 0 || scheduler == NULL || image_decode_busy()) {
        return NULL;
    }
    for (size_t at = 0; at < target_count; at++) {
        if (targets[at].node == NULL
            || (targets[at].kind != IMAGE_PRIORITY_KIND_DOCUMENT
                && targets[at].kind != IMAGE_PRIORITY_KIND_LINKED_VIDEO
                && targets[at].kind != IMAGE_PRIORITY_KIND_PRELOAD)) return NULL;
    }
    if (maximum_count > MAX_IMAGE_FETCH_ATTEMPTS) {
        maximum_count = MAX_IMAGE_FETCH_ATTEMPTS;
    }
    if (images->budget == NULL) {
        memset(images, 0, sizeof(*images));
        images->budget = budget;
    } else if (images->budget != budget) {
        return NULL;
    }
    ImagePriorityLoadJob *job = budget_calloc(budget, 1, sizeof(*job));
    if (job == NULL) return NULL;
    memcpy(job->targets, targets, target_count * sizeof(*targets));
    job->document_generation = document->content_generation;
    for (size_t at = 0; at < target_count; at++) {
        if (targets[at].weak_handle == 0) continue;
        size_t length = 0;
        const char *source = image_select_source_for_width(
            stylesheet, targets[at].node,
            targets[at].kind == IMAGE_PRIORITY_KIND_PRELOAD ? 0 : targets[at].display_width,
            &length);
        job->source_hashes[at] = source == NULL ? 0 : image_hash_bytes(source, length);
    }
    job->target_count = target_count;
    job->retained_count = images->count;
    job->retained_stats = images->stats;
    job->retained_failure_serial = images->failure_serial;
    job->retained_priority_staged = images->priority_staged;
    job->started_ms = image_now_ms();
    job->context = (ImageLoadContext) {
        .document = document,
        .images = images,
        .stylesheet = stylesheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .referrer_policy = referrer_policy,
        .maximum_count = maximum_count,
        .maximum_total_encoded_bytes = maximum_total_encoded_bytes,
        .maximum_single_encoded_bytes = maximum_single_encoded_bytes,
        .maximum_decoded_bytes = maximum_decoded_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .viewport_width = stylesheet->viewport_width,
        .priority_targets = job->targets,
        .priority_target_count = target_count,
        .deadline_ms = 0.0,
        .slice_started_us = tilefinch_platform_monotonic_time_us(),
        .eager_decode_rasters = true,
        .externally_pumped = true
    };
    return job;
}

bool images_priority_load_sources_current(ImagePriorityLoadJob *job)
{
    if (job == NULL || job->context.document == NULL) return false;
    uint64_t generation = job->context.document->content_generation;
    if (generation == job->document_generation) return true;
    for (size_t at = 0; at < job->target_count; at++) {
        const ImagePriorityTarget *target = &job->targets[at];
        if (target->weak_handle == 0) continue;
        size_t length = 0;
        const char *source = image_select_source_for_width(
            job->context.stylesheet, target->node,
            target->kind == IMAGE_PRIORITY_KIND_PRELOAD ? 0 : target->display_width,
            &length);
        uint64_t hash = source == NULL ? 0 : image_hash_bytes(source, length);
        if (hash != job->source_hashes[at]) return false;
    }
    job->document_generation = generation;
    return true;
}

ImagePriorityLoadStatus images_priority_load_pump(
    ImagePriorityLoadJob *job)
{
    if (job == NULL || job->failed) return IMAGE_PRIORITY_LOAD_FAILED;
    if (job->terminal) return IMAGE_PRIORITY_LOAD_COMPLETE;
    ImageLoadContext *context = &job->context;
    if (!job->admitted) {
        /* A resumable boundary: queued input defers admission, not the
           transaction itself. Unlike a decoder checkpoint, no DOM/resource
           state is borrowed here and no request has begun. */
        if (!tilefinch_platform_cooperate_at(
                "image-admission-boundary", 0,
                TILEFINCH_COOPERATE_IMAGE_ADMISSION)) {
            image_finish_slice(context);
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        if (fetch_scheduler_enqueue_would_block(
                context->scheduler,
                context->maximum_single_encoded_bytes)) {
            if (image_now_ms() - job->started_ms
                >= (double) context->timeout_ms) {
                return image_priority_load_fail(job);
            }
            image_finish_slice(context);
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        job->admitted = true;
        job->started_ms = image_now_ms();
        context->deadline_ms =
            job->started_ms + (double) context->timeout_ms;
        for (size_t at = 0; at < job->target_count; at++) {
            if (at != 0
                && (!tilefinch_platform_cooperate_at(
                        "image-admission-boundary", at,
                        TILEFINCH_COOPERATE_IMAGE_ADMISSION)
                    || fetch_scheduler_enqueue_would_block(
                        context->scheduler,
                        context->maximum_single_encoded_bytes))) {
                /* The second visible target is optional overlap, never an
                   admission failure. Leave it in the page queue for the next
                   job when input is waiting or only one descriptor is
                   presently available. Keep the first request intact. */
                job->target_count = at;
                context->priority_target_count = at;
                break;
            }
            if (!image_work(context, 1, false,
                            "deferred-image-admission")
                || !image_process_priority_target(
                       context, &job->targets[at])) {
                return image_priority_load_fail(job);
            }
        }
        if (context->pending_count == 0
            && !context->raster_decode.active) {
            image_finish_slice(context);
            context->images->priority_staged = true;
            job->terminal = true;
            return IMAGE_PRIORITY_LOAD_COMPLETE;
        }
        /* Admission is one idle unit. Even an immediately available replay
           or cache-backed transport result is consumed on the next call. */
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }

    if (context->raster_decode.active) {
        ImagePriorityLoadStatus decode_status =
            image_pending_raster_pump(context);
        image_finish_slice(context);
        if (decode_status == IMAGE_PRIORITY_LOAD_FAILED)
            return image_priority_load_fail(job);
        if (decode_status == IMAGE_PRIORITY_LOAD_PENDING
            || context->pending_count != 0) {
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        goto image_priority_load_complete;
    }

    cancel_expired_pending(context);
    const FetchPumpQuota quota = {
        .maximum_body_callbacks = IMAGE_FETCH_PUMP_CALLBACKS,
        .maximum_body_bytes = IMAGE_FETCH_PUMP_BYTES,
        .maximum_time_us = IMAGE_FETCH_PUMP_TIME_US
    };
    size_t completed = fetch_scheduler_pump_bounded(
        context->scheduler, IMAGE_FETCH_PUMP_COMPLETIONS, 0,
        &quota, NULL);
    image_note_pending_progress(context);
    image_cancel_no_progress_pending(context);
    if (completed == 0) {
        /* A scheduler completion retained after decode-gate contention also
           produces no new completion edge, including for a one-image job.
           Always give the ordered consumer one nonblocking retry. */
        size_t pending_before = context->pending_count;
        if (!finish_one_pending(context, false)) {
            return image_priority_load_fail(job);
        }
        if (context->pending_count == pending_before) {
            image_finish_slice(context);
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
    } else if (!finish_one_pending(context, false)) {
        return image_priority_load_fail(job);
    }
    if (context->pending_count != 0) {
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }
    if (context->raster_decode.active) {
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }
image_priority_load_complete:
    context->images->stats.deadline_cancelled =
        context->deadline_cancelled ? 1u : 0u;
    context->images->stats.deadline_exceeded =
        image_now_ms() >= context->deadline_ms;
    double elapsed = image_now_ms() - job->started_ms;
    if (elapsed > 0.0) {
        context->images->stats.elapsed_ms += (uint64_t) elapsed;
    }
    context->images->priority_staged = true;
    job->terminal = true;
    return IMAGE_PRIORITY_LOAD_COMPLETE;
}

void images_priority_load_commit_progress(ImagePriorityLoadJob *job)
{
    if (job == NULL || job->context.images == NULL || job->rolled_back)
        return;
    job->retained_count = job->context.images->count;
    job->retained_stats = job->context.images->stats;
    job->retained_failure_serial = job->context.images->failure_serial;
    job->retained_priority_staged =
        job->context.images->priority_staged;
}

size_t images_priority_load_target_count(const ImagePriorityLoadJob *job)
{
    return job == NULL ? 0 : job->target_count;
}

bool images_priority_load_contains_node(
    const ImagePriorityLoadJob *job, const lxb_dom_node_t *node)
{
    if (job == NULL || node == NULL) return false;
    for (size_t at = 0; at < job->target_count; at++) {
        if (job->targets[at].node == node) return true;
    }
    return false;
}

void images_priority_load_destroy(ImagePriorityLoadJob *job)
{
    if (job == NULL) return;
    if (!job->terminal) (void) image_priority_load_fail(job);
    budget_free(job->context.budget, job->context.request_scratch);
    budget_free(job->context.budget, job);
}

bool image_resource_available(const ImageResource *image)
{
    return image != NULL && (image->pixels != NULL
                             || (image->encoded != NULL
                                 && image->encoded_length != 0));
}

const void *image_resource_backing_identity(const ImageResource *image)
{
    if (image == NULL) return NULL;
    if (image->pixels != NULL) return image->pixels;
    if (image->encoded_body != NULL) return image->encoded_body;
    return image->encoded;
}

bool image_resource_source_current(const Stylesheet *sheet,
                                    const ImageResource *image)
{
    if (image == NULL || image->node == NULL) return false;
    size_t length = 0;
    const char *source = image_select_source(sheet, image->node, &length);
    return source != NULL && length != 0
        && image->source_hash == image_hash_bytes(source, length);
}

const ImageResource *images_find_node(const ImageResources *images,
                                      const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && !images->items[i].is_mask
            && !images->items[i].is_background) {
            return &images->items[i];
        }
    }
    return NULL;
}

const ImageResource *images_find_background_node(const ImageResources *images,
                                                 const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node
            && images->items[i].is_background
            && images->items[i].pseudo == PSEUDO_NONE) {
            return &images->items[i];
        }
    }
    return NULL;
}

static const ImageResource *images_find_paint_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo, bool mask)
{
    if (images == NULL || node == NULL || source == NULL
        || source[0] == '\0') return NULL;
    uint64_t source_hash = image_hash_bytes(source, strlen(source));
    const ImageResource *legacy = NULL;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->node != node || item->is_mask != mask
            || item->is_background != !mask
            || item->pseudo != pseudo) continue;
        if (item->source_hash == source_hash) return item;
        /* Adopted/test resources predate authored-source identity. Preserve
           their one-resource-per-role behavior without letting such an
           entry alias two real layered loads. */
        if (item->source_hash == 0 && legacy == NULL) legacy = item;
    }
    return legacy;
}

const ImageResource *images_find_background_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo)
{
    return images_find_paint_source(images, node, source, pseudo, false);
}

const ImageResource *images_find_mask_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo)
{
    return images_find_paint_source(images, node, source, pseudo, true);
}

const ImageResource *images_find_mask_node(const ImageResources *images,
                                           const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && images->items[i].is_mask
            && images->items[i].pseudo == PSEUDO_NONE) {
            return &images->items[i];
        }
    }
    return NULL;
}

const ImageResource *images_find_pseudo_mask(const ImageResources *images,
                                             const lxb_dom_node_t *node,
                                             PseudoElement pseudo)
{
    if (images == NULL || node == NULL || pseudo == PSEUDO_NONE) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && images->items[i].is_mask
            && images->items[i].pseudo == pseudo) return &images->items[i];
    }
    return NULL;
}

const ImageResource *images_find_pseudo_background(
    const ImageResources *images, const lxb_dom_node_t *node,
    PseudoElement pseudo)
{
    if (images == NULL || node == NULL || pseudo == PSEUDO_NONE) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node
            && images->items[i].is_background
            && images->items[i].pseudo == pseudo) return &images->items[i];
    }
    return NULL;
}

void images_destroy(ImageResources *images)
{
    if (images == NULL) return;
    if (images->budget != NULL) {
        images_retarget_plan_discard(images);
        for (size_t i = 0; i < images->count; i++) {
            image_canvas_native_forget(&images->items[i]);
            image_resource_release_owned_pixels(
                images->budget, &images->items[i]);
            if (images->items[i].owns_encoded) {
                if (images->items[i].encoded_body != NULL) {
                    browser_shared_body_release(
                        images->items[i].encoded_body);
                } else {
                    budget_free(images->budget, images->items[i].encoded);
                }
            }
        }
        for (size_t i = 0; i < 2u; i++)
            budget_free(images->budget, images->canvas_depth[i].values);
        budget_free(images->budget, images->items);
        budget_free(images->budget, images->failures);
    }
    memset(images, 0, sizeof(*images));
}
