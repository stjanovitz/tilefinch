#include "tilefinch/script_loader.h"

#include "tilefinch/document.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/request_context.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/script_admission.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/url.h"

#include "data_url.h"
#include "diagnostic_trace.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/element.h>
#include <lexbor/ns/const.h>

/* The tilefinch_trace_* helpers cache their own answer, and are constant
   false under TILEFINCH_NO_TRACE; a second cache here would keep the
   formatting alive in builds that cannot trace. */
static void script_trace_skip(const char *url)
{
    if (tilefinch_trace_script_failures())
        fprintf(stderr, "script-quota-skip url=%s\n",
                url == NULL ? "<null>" : url);
}

static void script_trace_attempt(const char *url)
{
    if (tilefinch_trace_script_attempts())
        fprintf(stderr, "script-attempt url=%s\n",
                url == NULL ? "<null>" : url);
}

static void script_metrics_record_source_work(
    ExternalScriptMetrics *metrics, size_t source_length)
{
    if (metrics == NULL) return;
    if (source_length > SIZE_MAX - metrics->source_work_bytes) {
        metrics->source_work_bytes = SIZE_MAX;
    } else {
        metrics->source_work_bytes += source_length;
    }
}

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (p), (s))

#define EXTERNAL_SCRIPT_HARD_LIMIT 256
#define SCRIPT_DISCOVERY_VISIT_LIMIT 65536u
/* Early parser-blocking JavaScript is optional; visible CSS and the first
   authoritative layout are not. Every admission below keeps the
   presentation reserve (script_admission.h) free so a large bundle cannot
   consume the capacity the page still needs to become readable. */
#define SCRIPT_PRESENTATION_RESERVE_BYTES \
    SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES
#define SCRIPT_CRITICAL_BOOTSTRAP_RESERVE 8u

_Static_assert(
    FETCH_REFERRER_POLICY_LIMIT
        <= BROWSER_MODULE_REFERRER_POLICY_LIMIT,
    "module cache must retain every normalized referrer policy token");

static bool script_heap_pressure_required(
    ScriptRuntime *runtime, size_t working_bytes, size_t reserve_bytes)
{
    size_t remaining = script_runtime_heap_available(runtime);
    return working_bytes > remaining
        || reserve_bytes > remaining - working_bytes;
}

/* A bounded script response is only useful if the runtime can still compile
   it and service the DOM work it may synchronously trigger.  Under evidenced
   pressure, make one ordinary GC/pool-trim attempt for the pipeline and then
   remeasure the shared page budget.  Known-size cache/compile work can be
   rejected immediately; an unknown network response is capped to affordable
   staging space so small scripts still make progress and an oversized body is
   rejected by policy before the allocator is exhausted.  GC between top-level
   scripts is standards-safe. Network staging always leaves a fixed minimum
   for the runtime; once the source length is known, compilation additionally
   reserves one source-sized working copy. Both rules scale with generic work,
   not a page or host. */
static size_t script_collect_pressure_once(
    ScriptRuntime *runtime, Budget *budget, size_t working_bytes,
    size_t budget_reserve_bytes, size_t heap_reserve_bytes,
    ExternalScriptMetrics *metrics)
{
    /* One collection per page load, then at most one a second: a page that
       settled after load leaves garbage a later dynamic import can use
       (chatgpt.com's Send imported its sentinel with the heap 12 KiB short
       of the reserve, long after the load-time collection). */
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    if (budget == NULL || metrics == NULL
        || (metrics->pressure_collections != 0
            && now_us - metrics->last_pressure_collection_us
                   < UINT64_C(1000000))
        || (!budget_pressure_required(
                budget, working_bytes, budget_reserve_bytes)
            && !script_heap_pressure_required(
                runtime, working_bytes, heap_reserve_bytes))) {
        return 0;
    }
    size_t before = budget_remaining(budget);
    (void) script_runtime_collect_and_trim(runtime);
    size_t after = budget_remaining(budget);
    size_t reclaimed = after > before ? after - before : 0;
    metrics->pressure_collections++;
    metrics->last_pressure_collection_us = now_us;
    if (reclaimed > SIZE_MAX - metrics->pressure_reclaimed_bytes) {
        metrics->pressure_reclaimed_bytes = SIZE_MAX;
    } else {
        metrics->pressure_reclaimed_bytes += reclaimed;
    }
    return reclaimed;
}

/* Compiled-script tables are optional Budget memory: they give way, through
   the Budget reclaim hook, before a page script is capped or refused for
   lack of room. Returns whether the pressure remains. */
static bool script_budget_pressure(Budget *budget, size_t working_bytes,
                                   size_t reserve_bytes)
{
    if (!budget_pressure_required(budget, working_bytes, reserve_bytes))
        return false;
    (void) budget_make_room(budget, working_bytes > SIZE_MAX - reserve_bytes
                                        ? SIZE_MAX
                                        : working_bytes + reserve_bytes);
    return budget_pressure_required(budget, working_bytes, reserve_bytes);
}

static bool script_admit_known_working_set_with_reserve(
    ScriptRuntime *runtime, Budget *budget, size_t working_bytes,
    size_t minimum_budget_reserve_bytes,
    size_t minimum_heap_reserve_bytes, ExternalScriptMetrics *metrics)
{
    if (budget == NULL || metrics == NULL) return false;
    size_t budget_reserve = working_bytes > minimum_budget_reserve_bytes
        ? working_bytes : minimum_budget_reserve_bytes;
    size_t heap_reserve = working_bytes > minimum_heap_reserve_bytes
        ? working_bytes : minimum_heap_reserve_bytes;
    if (!script_budget_pressure(budget, working_bytes, budget_reserve)
        && !script_heap_pressure_required(
            runtime, working_bytes, heap_reserve)) {
        return true;
    }
    size_t reclaimed = script_collect_pressure_once(
        runtime, budget, working_bytes, budget_reserve, heap_reserve,
        metrics);
    if (!budget_pressure_required(
            budget, working_bytes, budget_reserve)
        && !script_heap_pressure_required(
            runtime, working_bytes, heap_reserve)) {
        budget_record_pressure(
            budget, BUDGET_PRESSURE_JAVASCRIPT, 0, reclaimed);
        return true;
    }
    metrics->skipped_pressure++;
    budget_record_pressure(
        budget, BUDGET_PRESSURE_JAVASCRIPT, working_bytes, reclaimed);
    return false;
}

/* Admits one compile unit (a whole script, or the largest factory or
   statement of a bundle the browser splits) of `unit_bytes` source when its
   planned compile working set (script_admission.h: the measured median,
   twice the source plus parser state) fits the heap the realm can reach,
   and the page Budget keeps the presentation reserve beside it. After one
   collection, a refusal counts as memory pressure. A unit that needs more
   than planned is stopped by the heap limit and refused at compile time
   with the realm intact. A page realm's heap grows on demand
   (script_runtime_heap_available counts that headroom). */
static bool script_admit_known_working_set(
    ScriptRuntime *runtime, Budget *budget, size_t unit_bytes,
    ExternalScriptMetrics *metrics)
{
    if (budget == NULL || metrics == NULL) return false;
    size_t peak = script_admission_compile_peak(unit_bytes);
    if (!script_budget_pressure(
            budget, peak, SCRIPT_PRESENTATION_RESERVE_BYTES)
        && !script_heap_pressure_required(runtime, peak, 0)) {
        return true;
    }
    size_t reclaimed = script_collect_pressure_once(
        runtime, budget, peak, SCRIPT_PRESENTATION_RESERVE_BYTES, 0,
        metrics);
    if (!budget_pressure_required(
            budget, peak, SCRIPT_PRESENTATION_RESERVE_BYTES)
        && !script_heap_pressure_required(runtime, peak, 0)) {
        budget_record_pressure(
            budget, BUDGET_PRESSURE_JAVASCRIPT, 0, reclaimed);
        return true;
    }
    metrics->skipped_pressure++;
    budget_record_pressure(
        budget, BUDGET_PRESSURE_JAVASCRIPT, peak, reclaimed);
    return false;
}

static bool script_bound_network_working_set(
    ScriptRuntime *runtime, Budget *budget, size_t *response_limit,
    bool *pressure_capped, ExternalScriptMetrics *metrics)
{
    if (budget == NULL || response_limit == NULL || *response_limit == 0
        || pressure_capped == NULL || metrics == NULL) return false;
    *pressure_capped = false;
    size_t requested = *response_limit;
    if (!script_budget_pressure(
            budget, requested, SCRIPT_PRESENTATION_RESERVE_BYTES)) {
        return true;
    }
    size_t remaining = budget_remaining(budget);
    if (remaining <= SCRIPT_PRESENTATION_RESERVE_BYTES) {
        size_t reclaimed = script_collect_pressure_once(
            runtime, budget, requested,
            SCRIPT_PRESENTATION_RESERVE_BYTES,
            SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES, metrics);
        if (!budget_pressure_required(
                budget, requested, SCRIPT_PRESENTATION_RESERVE_BYTES)) {
            budget_record_pressure(
                budget, BUDGET_PRESSURE_JAVASCRIPT, 0, reclaimed);
            return true;
        }
        remaining = budget_remaining(budget);
        if (remaining > SCRIPT_PRESENTATION_RESERVE_BYTES) {
            size_t affordable =
                remaining - SCRIPT_PRESENTATION_RESERVE_BYTES;
            *response_limit = affordable;
            *pressure_capped = true;
            metrics->pressure_capped_requests++;
            budget_record_pressure(
                budget, BUDGET_PRESSURE_JAVASCRIPT,
                requested - affordable, reclaimed);
            return true;
        }
        metrics->skipped_pressure++;
        budget_record_pressure(
            budget, BUDGET_PRESSURE_JAVASCRIPT, requested, reclaimed);
        return false;
    }
    size_t affordable = remaining - SCRIPT_PRESENTATION_RESERVE_BYTES;
    if (affordable >= requested) return true;
    *response_limit = affordable;
    *pressure_capped = true;
    metrics->pressure_capped_requests++;
    budget_record_pressure(
        budget, BUDGET_PRESSURE_JAVASCRIPT, requested - affordable,
        0);
    return true;
}

static bool script_fetch_was_pressure_rejected(
    const FetchResult *fetch, bool pressure_capped)
{
    return pressure_capped && fetch != NULL
        && strcmp(fetch->error, "response quota exceeded") == 0;
}

static void script_compact_fetch_source(FetchResult *fetch)
{
    if (fetch == NULL || fetch->budget == NULL || fetch->shared_body != NULL
        || fetch->data == NULL || fetch->length == SIZE_MAX
        || fetch->capacity <= fetch->length + 1) return;
    char *trimmed = budget_realloc_category(
        fetch->budget, BUDGET_CATEGORY_RESOURCE, fetch->data,
        fetch->length + 1);
    if (trimmed == NULL) return;
    fetch->data = trimmed;
    fetch->capacity = fetch->length + 1;
    fetch->data[fetch->length] = '\0';
}

static BrowserCacheStatus script_cache_match(
    BrowserSession *session, const char *url, const char *initiator_url,
    const TilefinchRequestContext *context,
    const BrowserCacheEntry **entry)
{
    return session == NULL
            || content_blocker_would_block(
                   session->content_blocker, url, initiator_url,
                   "script", "no-cors")
        ? BROWSER_CACHE_MISS
        : browser_session_cache_match_classic_script(
            session, url, context,
            tilefinch_platform_monotonic_time_ns(), entry);
}

static BrowserCacheStatus script_module_cache_match(
    BrowserSession *session, const char *request_url,
    const char *initiator_origin, const char *top_level_url,
    bool initiator_opaque, TilefinchCredentialsMode credentials,
    const BrowserCacheEntry **entry)
{
    return session == NULL
            || content_blocker_would_block(
                   session->content_blocker, request_url, initiator_origin,
                   "script", "cors")
        ? BROWSER_CACHE_MISS
        : browser_session_cache_match_module(
            session, request_url, initiator_origin, top_level_url,
            initiator_opaque, credentials,
            tilefinch_platform_monotonic_time_ns(), entry);
}

void script_cache_response_policy(const FetchResult *fetch,
                                  char cache_control[256], char vary[128])
{
    cache_control[0] = '\0';
    vary[0] = '\0';
    (void) fetch_response_header_value(fetch, "cache-control",
                                       cache_control, 256);
    (void) fetch_response_header_value(fetch, "vary", vary, 128);
}

bool script_response_no_store(const FetchResult *fetch)
{
    char cache_control[256], vary[128];
    script_cache_response_policy(fetch, cache_control, vary);
    return browser_session_cache_control_no_store(cache_control);
}

static void script_cache_store(BrowserSession *session, const char *url,
                               FetchResult *fetch,
                               const TilefinchRequestContext *context,
                               const TilefinchResourceGrant *grant)
{
    char cache_control[256], vary[128];
    script_cache_response_policy(fetch, cache_control, vary);
    if (fetch_result_share_body(fetch)) {
        (void) browser_session_cache_put_http_shared_classic_script(
            session, url, fetch->shared_body, fetch->etag,
            fetch->last_modified, fetch->content_type,
            cache_control, vary, tilefinch_platform_monotonic_time_ns(),
            context, grant);
    }
}

static void script_cache_revalidate(BrowserSession *session, const char *url,
                                    const FetchResult *fetch,
                                    const TilefinchRequestContext *context,
                                    const TilefinchResourceGrant *grant)
{
    char cache_control[256], vary[128];
    script_cache_response_policy(fetch, cache_control, vary);
    (void) browser_session_cache_revalidate_classic_script(
        session, url, cache_control, vary,
        tilefinch_platform_monotonic_time_ns(), context, grant);
}

bool script_module_cache_record(
    BrowserSession *session, const char *request_url,
    const char *effective_url, const char *initiator_origin,
    const char *top_level_url, bool initiator_opaque,
    TilefinchCredentialsMode credentials, FetchResult *fetch,
    bool revalidate, uint64_t now_ns)
{
    if (session == NULL || request_url == NULL || effective_url == NULL
        || initiator_origin == NULL || fetch == NULL
        || (!revalidate && fetch->length == 0)) return false;
    char cache_control[256], vary[128];
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            fetch, &referrer_policy_header_present,
            response_referrer_policy)) return false;
    script_cache_response_policy(fetch, cache_control, vary);
    BrowserModuleCacheProvenance provenance = {
        .effective_url = effective_url,
        .initiator_origin = initiator_origin,
        .top_level_url = top_level_url,
        .initiator_opaque = initiator_opaque,
        .response_referrer_policy = response_referrer_policy,
        .credentials = credentials,
        .cors_validated = true,
        .cors_redirect_origin_tainted = fetch->redirect_origin_tainted,
        .javascript_mime_validated = true,
        .referrer_policy_header_present =
            referrer_policy_header_present
    };
    if (revalidate)
        return browser_session_cache_revalidate_module(
            session, request_url, cache_control, vary, now_ns, &provenance);
    if (fetch_result_share_body(fetch))
        return browser_session_cache_put_http_shared_module(
            session, request_url, fetch->shared_body, fetch->etag,
            fetch->last_modified, fetch->content_type,
            cache_control, vary, now_ns, &provenance);
    return browser_session_cache_put_http_module(
        session, request_url, (const unsigned char *) fetch->data,
        fetch->length, fetch->etag, fetch->last_modified,
        fetch->content_type, cache_control, vary, now_ns, &provenance);
}

static const char *script_module_effective_referrer_policy(
    const char *incoming_policy, const char *response_policy)
{
    return response_policy != NULL && response_policy[0] != '\0'
        ? response_policy
        : (incoming_policy == NULL ? "" : incoming_policy);
}

typedef struct ModuleBodyLease {
    char *source;
    BrowserSharedBody *body;
    struct ModuleBodyLease *next;
} ModuleBodyLease;

typedef struct {
    const char *data;
    size_t length;
    BrowserSharedBody *body;
    char *fallback_copy;
    Budget *budget;
} ScriptCacheSource;

static bool script_source_has_terminator(const unsigned char *data,
                                         size_t length)
{
    return data != NULL && length != SIZE_MAX
        && budget_usable_size(data) > length && data[length] == 0;
}

/* Cache lookup returns a borrowed entry which later script/module activity can
   evict.  Retaining its body pins only the immutable payload, not the cache
   slot, across compilation and recursive module loading. */
static bool script_cache_source_acquire(Budget *budget,
                                        const BrowserCacheEntry *entry,
                                        ScriptCacheSource *source)
{
    if (source == NULL) return false;
    memset(source, 0, sizeof(*source));
    source->budget = budget;
    if (budget == NULL || entry == NULL || entry->data == NULL
        || entry->length == 0) return false;
    if (entry->body != NULL && entry->body->data == entry->data
        && entry->body->length == entry->length
        && script_source_has_terminator(entry->body->data, entry->length)) {
        source->body = browser_shared_body_retain(entry->body);
        if (source->body != NULL) {
            source->data = (const char *) source->body->data;
            source->length = source->body->length;
            return true;
        }
    }
    if (entry->length == SIZE_MAX) return false;
    source->fallback_copy = budget_malloc(budget, entry->length + 1);
    if (source->fallback_copy == NULL) return false;
    memcpy(source->fallback_copy, entry->data, entry->length);
    source->fallback_copy[entry->length] = '\0';
    source->data = source->fallback_copy;
    source->length = entry->length;
    return true;
}

static void script_cache_source_release(ScriptCacheSource *source)
{
    if (source == NULL) return;
    browser_shared_body_release(source->body);
    budget_free(source->budget, source->fallback_copy);
    memset(source, 0, sizeof(*source));
}

static bool node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

typedef enum {
    SCRIPT_KIND_INERT = 0,
    SCRIPT_KIND_CLASSIC,
    SCRIPT_KIND_MODULE
} ScriptKind;

static ScriptKind script_declared_kind(lxb_dom_node_t *node)
{
    if (!node_name_is(node, "script")) return SCRIPT_KIND_INERT;
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    bool module = false;
    if (!script_type_attribute_classify(type, length, &module))
        return SCRIPT_KIND_INERT;
    return module ? SCRIPT_KIND_MODULE : SCRIPT_KIND_CLASSIC;
}

static bool script_has_nomodule(lxb_dom_node_t *node)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "nomodule", 8u);
}

static ScriptKind script_kind(lxb_dom_node_t *node)
{
    ScriptKind kind = script_declared_kind(node);
    /* Module support is a browser capability. A nomodule fallback remains
       inert even if the paired module later fails to fetch or execute. */
    return kind == SCRIPT_KIND_CLASSIC && script_has_nomodule(node)
        ? SCRIPT_KIND_INERT : kind;
}

/* SVG 2 uses `href` for an external script, while HTMLScriptElement uses
   `src`. Keep that namespace distinction here so every discovery, blocking,
   cache, and execution path agrees on whether the script is external. */
static const char *script_source_attribute(lxb_dom_node_t *node,
                                           size_t *length)
{
    const char *source = document_attribute(node, "src", length);
    if ((source == NULL || *length == 0) && node != NULL
        && node->ns == LXB_NS_SVG) {
        source = document_attribute(node, "href", length);
    }
    return source;
}

static lxb_dom_node_t *script_discovery_next(lxb_dom_node_t *root,
                                             lxb_dom_node_t *node)
{
    if (root == NULL || node == NULL) return NULL;
    if (node->first_child != NULL) return node->first_child;
    while (node != root && node->next == NULL) node = node->parent;
    return node == root ? NULL : node->next;
}

static void collect_scripts(ScriptRuntime *runtime, lxb_dom_node_t *node,
                            long *scripts, size_t capacity,
                            size_t *count, size_t *discovered,
                            size_t *skipped_modules,
                            size_t *skipped_nomodule)
{
    lxb_dom_node_t *root = node;
    for (size_t visited = 0;
         node != NULL && visited < SCRIPT_DISCOVERY_VISIT_LIMIT;
         visited++, node = script_discovery_next(root, node)) {
        ScriptKind declared = script_declared_kind(node);
        if (declared == SCRIPT_KIND_CLASSIC
            && script_has_nomodule(node)) {
            if (skipped_nomodule != NULL) (*skipped_nomodule)++;
            continue;
        }
        ScriptKind kind = declared;
        if (kind == SCRIPT_KIND_CLASSIC) {
            size_t source_length = 0;
            const char *source = script_source_attribute(
                node, &source_length);
            if (source != NULL && source_length != 0) {
                (*discovered)++;
                if (*count < capacity) {
                    long handle = script_runtime_node_weak_handle(runtime,
                                                                  node);
                    if (handle != 0) scripts[(*count)++] = handle;
                }
            }
        } else if (kind == SCRIPT_KIND_MODULE
                   && skipped_modules != NULL) {
            (*skipped_modules)++;
        }
    }
}

/* A page without a policy never pays for the parser-inserted lookup. */
static uint8_t script_element_csp_grant(
    const TilefinchContentSecurityPolicy *csp, ScriptRuntime *runtime,
    lxb_dom_node_t *node)
{
    return csp == NULL || !csp->header_present ? 0
        : tilefinch_csp_element_grant(
              csp, TILEFINCH_DESTINATION_SCRIPT, node,
              script_runtime_script_parser_inserted(runtime, node));
}

static TilefinchRequestContext script_request_context(
    const char *document_url, const char *top_level_url,
    const char *target_url, bool cors,
    TilefinchCredentialsMode credentials, bool initiator_opaque)
{
    if (initiator_opaque
        && credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN) {
        credentials = TILEFINCH_CREDENTIALS_OMIT;
    }
    return (TilefinchRequestContext) {
        .target_url = target_url,
        .initiator_url = document_url,
        .top_level_url = top_level_url,
        .method = "GET",
        .mode = cors ? TILEFINCH_REQUEST_MODE_CORS
                     : TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = credentials,
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .initiator_opaque = initiator_opaque
    };
}

static bool script_resource_grant(
    const FetchResult *fetch, const TilefinchRequestContext *context,
    const char *accepted_content_type, bool module,
    TilefinchResourceGrant *grant)
{
    bool javascript_mime = script_module_mime_type_allowed(
        accepted_content_type);
    return fetch_resource_grant_create(
        fetch, context, context != NULL
            && context->mode == TILEFINCH_REQUEST_MODE_CORS,
        javascript_mime, module, grant, NULL);
}

static TilefinchCredentialsMode script_cors_credentials(
    lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "crossorigin", &length);
    static const char use_credentials[] = "use-credentials";
    return value != NULL && length == sizeof(use_credentials) - 1
        && strncasecmp(value, use_credentials, length) == 0
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
}

static bool script_crossorigin_present(lxb_dom_node_t *node)
{
    size_t length = 0;
    return document_attribute(node, "crossorigin", &length) != NULL;
}

static bool script_integrity_present(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "integrity", &length);
    return value != NULL && length != 0;
}

static bool script_integrity_request_eligible(
    lxb_dom_node_t *node, const char *document_url, const char *target_url,
    bool initiator_opaque)
{
    return !script_integrity_present(node)
        || (!initiator_opaque
            && tilefinch_url_same_origin(document_url, target_url))
        || script_crossorigin_present(node);
}

static void script_accept_response_cookies(
    BrowserSession *session, const TilefinchRequestContext *request,
    const FetchResult *fetch, const char *fallback_url)
{
    if (session == NULL || request == NULL || fetch == NULL
        || !tilefinch_request_sends_credentials(request)) return;
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        TilefinchRequestContext response = *request;
        response.target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            session, &response, fetch->set_cookies[i]);
    }
}

static ScriptQuotaReserveResult script_quota_reserve_bounded(
    ScriptRuntime *runtime, ScriptQuotaCountMode count_mode,
    size_t requested_max_bytes, long timeout_ms,
    ScriptQuotaReservation *reservation, bool *progress_failed)
{
    if (progress_failed != NULL) *progress_failed = false;
    ScriptQuotaReserveResult status = script_runtime_script_quota_reserve(
        runtime, count_mode, requested_max_bytes, reservation);
    if (status != SCRIPT_QUOTA_RESERVE_DEFERRED || timeout_ms <= 0) {
        return status;
    }
    uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t wait_ns = (uint64_t) timeout_ms;
    wait_ns = wait_ns > UINT64_MAX / UINT64_C(1000000)
        ? UINT64_MAX : wait_ns * UINT64_C(1000000);
    uint64_t deadline_ns = wait_ns > UINT64_MAX - started_ns
        ? UINT64_MAX : started_ns + wait_ns;

    /* The wall clock is an external platform dependency.  Keep an explicit
       pump bound as well, so a broken or low-resolution monotonic clock can
       never turn a nominally bounded loader wait into an infinite loop. */
    uint64_t timeout_u64 = (uint64_t) timeout_ms;
    uint64_t pump_limit_u64 = (timeout_u64 + UINT64_C(3)) / UINT64_C(4)
                              + UINT64_C(2);
    size_t maximum_pumps = pump_limit_u64 > (uint64_t) SIZE_MAX
        ? SIZE_MAX : (size_t) pump_limit_u64;
    size_t pumps = 0;
    while (status == SCRIPT_QUOTA_RESERVE_DEFERRED
           && pumps++ < maximum_pumps) {
        uint64_t now_ns = tilefinch_platform_monotonic_time_ns();
        if (now_ns >= deadline_ns) break;
        uint64_t remaining_ns = deadline_ns - now_ns;
        unsigned wait_ms = remaining_ns >= UINT64_C(4000000)
            ? 4u : (unsigned) ((remaining_ns + UINT64_C(999999))
                               / UINT64_C(1000000));
        ScriptQuotaProgressResult progress =
            script_runtime_script_quota_progress(runtime, wait_ms);
        if (progress == SCRIPT_QUOTA_PROGRESS_FAILED) {
            if (tilefinch_trace_script_failures()) {
                fprintf(stderr,
                        "script-quota-progress-failure requested=%zu "
                        "pump=%zu/%zu\n",
                        requested_max_bytes, pumps, maximum_pumps);
            }
            if (progress_failed != NULL) *progress_failed = true;
            return SCRIPT_QUOTA_RESERVE_REJECTED;
        }
        if (progress == SCRIPT_QUOTA_PROGRESS_EXHAUSTED) break;
        status = script_runtime_script_quota_reserve(
            runtime, count_mode, requested_max_bytes, reservation);
    }
    return status;
}

bool external_scripts_load(NavigationSession *navigation,
                           const char *document_url,
                           size_t maximum_scripts,
                           size_t maximum_total_bytes,
                           size_t maximum_file_bytes,
                           long timeout_ms,
                           ExternalScriptMetrics *metrics)
{
    if (navigation == NULL || !navigation->page.loaded
        || navigation->page.runtime == NULL || document_url == NULL
        || metrics == NULL || maximum_scripts == 0
        || maximum_total_bytes == 0 || maximum_file_bytes == 0) return false;
    memset(metrics, 0, sizeof(*metrics));
    long scripts[EXTERNAL_SCRIPT_HARD_LIMIT] = {0};
    size_t script_count = 0;
    size_t collection_limit = maximum_scripts < EXTERNAL_SCRIPT_HARD_LIMIT
        ? maximum_scripts : EXTERNAL_SCRIPT_HARD_LIMIT;
    collect_scripts(
        navigation->page.runtime,
        lxb_dom_interface_node(navigation->page.document.html),
        scripts, collection_limit, &script_count,
        &metrics->discovered, &metrics->skipped_module,
        &metrics->skipped_nomodule);
    if (metrics->discovered > script_count) {
        metrics->skipped_quota += metrics->discovered - script_count;
    }
    for (size_t i = 0; i < script_count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            navigation->page.runtime, scripts[i]);
        if (node == NULL) continue;
        size_t reference_length = 0;
        const char *reference = script_source_attribute(
            node, &reference_length);
        if (reference == NULL || reference_length == 0
            || reference_length >= NAVIGATION_URL_LIMIT) {
            metrics->failed++;
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        char reference_copy[NAVIGATION_URL_LIMIT];
        memcpy(reference_copy, reference, reference_length);
        reference_copy[reference_length] = '\0';
        char resolved[NAVIGATION_URL_LIMIT];
        if (!fetch_resolve_url(document_url, reference_copy, resolved,
                               sizeof(resolved))) {
            metrics->failed++;
            (void) navigation_dispatch_node_event(navigation, node, "error");
            continue;
        }
        uint8_t csp_grant = script_element_csp_grant(
            &navigation->page.document.content_security_policy,
            navigation->page.runtime, node);
        if (!tilefinch_csp_allows_request_granted(
                &navigation->page.document.content_security_policy,
                TILEFINCH_DESTINATION_SCRIPT, resolved, csp_grant)) {
            metrics->failed++;
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        bool cors = script_crossorigin_present(node);
        bool initiator_opaque = script_runtime_origin_is_opaque(
            navigation->page.runtime);
        if (!script_integrity_request_eligible(
                node, document_url, resolved, initiator_opaque)) {
            metrics->failed++;
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        TilefinchCredentialsMode credentials = cors
            ? script_cors_credentials(node)
            : initiator_opaque ? TILEFINCH_CREDENTIALS_OMIT
                               : TILEFINCH_CREDENTIALS_INCLUDE;
        char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
        if (cors && initiator_opaque) {
            memcpy(initiator_origin, "null", sizeof("null"));
        } else if (cors && !tilefinch_url_origin(
                       document_url, initiator_origin,
                       sizeof(initiator_origin))) {
            metrics->failed++;
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        ScriptQuotaReservation quota = {0};
        if (metrics->bytes >= maximum_total_bytes) {
            metrics->skipped_quota++; script_trace_skip(resolved);
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        size_t requested_bytes = maximum_total_bytes - metrics->bytes;
        if (requested_bytes > maximum_file_bytes) {
            requested_bytes = maximum_file_bytes;
        }
        bool progress_failed = false;
        ScriptQuotaReserveResult quota_status = script_quota_reserve_bounded(
            navigation->page.runtime, SCRIPT_QUOTA_NEW_EXECUTABLE,
            requested_bytes, timeout_ms, &quota, &progress_failed);
        if (progress_failed) return false;
        if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
            || quota.reserved_bytes == 0) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            metrics->skipped_quota++; script_trace_skip(resolved);
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        size_t response_limit = quota.reserved_bytes;
        TilefinchRequestContext request_context = script_request_context(
            document_url, document_url, resolved, cors, credentials,
            initiator_opaque);
        request_context.csp_grant = csp_grant;
        const BrowserCacheEntry *cached = NULL;
        BrowserCacheStatus cache_status = cors ? BROWSER_CACHE_MISS
            : script_cache_match(
                navigation->browser_session, resolved, document_url,
                &request_context, &cached);
        if (cache_status == BROWSER_CACHE_FRESH) {
            if (cached == NULL || cached->length > response_limit) {
                script_runtime_script_quota_abort(
                    navigation->page.runtime, &quota);
                metrics->skipped_quota++; script_trace_skip(resolved);
                (void) navigation_dispatch_node_event(
                    navigation, node, "error");
                continue;
            }
            metrics->attempted++; script_trace_attempt(resolved);
            metrics->cache_hits++;
            ScriptCacheSource cached_source;
            if (!script_cache_source_acquire(
                    navigation->budget, cached, &cached_source)) {
                script_runtime_script_quota_abort(
                    navigation->page.runtime, &quota);
                metrics->failed++;
                (void) navigation_dispatch_node_event(navigation, node,
                                                       "error");
                continue;
            }
            if (!script_runtime_script_quota_commit(
                    navigation->page.runtime, &quota,
                    cached_source.length)) {
                script_runtime_script_quota_abort(
                    navigation->page.runtime, &quota);
                script_cache_source_release(&cached_source);
                metrics->skipped_quota++; script_trace_skip(resolved);
                (void) navigation_dispatch_node_event(
                    navigation, node, "error");
                continue;
            }
            if (!script_admit_known_working_set(
                    navigation->page.runtime, navigation->budget,
                    cached_source.length, metrics)) {
                script_cache_source_release(&cached_source);
                (void) navigation_dispatch_node_event(
                    navigation, node, "error");
                continue;
            }
            bool loaded = navigation_evaluate_external_script(
                navigation, node, cached_source.data,
                cached_source.length,
                resolved);
            size_t cached_length = cached_source.length;
            script_cache_source_release(&cached_source);
            if (loaded) {
                metrics->loaded++;
                metrics->bytes += cached_length;
                (void) script_runtime_record_resource_timing(
                    navigation->page.runtime, resolved, "script");
            } else {
                metrics->failed++;
                (void) navigation_dispatch_node_event(
                    navigation, node, "error");
            }
            continue;
        }
        bool pressure_capped = false;
        if (!script_bound_network_working_set(
                navigation->page.runtime, navigation->budget,
                &response_limit, &pressure_capped, metrics)) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            (void) navigation_dispatch_node_event(navigation, node, "error");
            continue;
        }
        ScriptCacheSource cached_source = {0};
        bool cached_source_ready = cached != NULL
            && script_cache_source_acquire(
                navigation->budget, cached, &cached_source);
        FetchResult *fetch = fetch_result_create(navigation->budget);
        if (fetch == NULL) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            script_cache_source_release(&cached_source);
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            return false;
        }
        metrics->attempted++; script_trace_attempt(resolved);
        FetchRequest transport = {
            .send_low_client_hints = true,
            .accept = "*/*",
            .if_none_match = cached == NULL ? NULL : cached->etag,
            .if_modified_since = cached == NULL
                                 ? NULL : cached->last_modified,
        };
        FetchPreparedPageRequest prepared;
        if (!fetch_prepare_page_request_context(
                &request_context, document_url,
                navigation->page.referrer_policy,
                navigation->browser_session,
                &navigation->page.document.content_security_policy,
                NULL, &transport, &prepared, NULL)) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            metrics->failed++;
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
            continue;
        }
        const FetchRequest *request = fetch_prepared_page_request(&prepared);
        if (request == NULL || !fetch_request_cancelable(
                                      navigation->budget, resolved, request,
                                      response_limit, timeout_ms, NULL, NULL,
                                      fetch)) {
            if (script_fetch_was_pressure_rejected(fetch, pressure_capped)) {
                metrics->skipped_pressure++;
            } else {
                metrics->failed++;
            }
            (void) navigation_dispatch_node_event(navigation, node, "error");
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            continue;
        }
        script_accept_response_cookies(
            navigation->browser_session, &request_context, fetch,
            fetch->effective_url[0] == '\0' ? resolved
                                             : fetch->effective_url);
        const char *source = fetch->data;
        size_t source_length = fetch->length;
        bool revalidated = false;
        if (fetch->status_code == 304 && cached != NULL) {
            if (cached_source_ready) {
                source = cached_source.data;
                source_length = cached_source.length;
                metrics->cache_hits++;
                revalidated = true;
            }
        }
        if ((fetch->status_code != 304 || cached_source_ready)
            && !script_runtime_script_quota_commit(
                   navigation->page.runtime, &quota, source_length)) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
            metrics->skipped_quota++; script_trace_skip(resolved);
            (void) navigation_dispatch_node_event(navigation, node, "error");
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            continue;
        }
        bool loaded = fetch->status_code != 304 || cached_source_ready;
        TilefinchResourceGrant resource_grant = {0};
        if (loaded && fetch->status_code == 304) {
            loaded = cached != NULL && cached->resource_grant_valid;
            if (loaded) resource_grant = cached->resource_grant;
        } else if (loaded) {
            loaded = script_resource_grant(
                fetch, &request_context, fetch->content_type, false,
                &resource_grant);
        }
        if (!loaded) {
            script_runtime_script_quota_abort(
                navigation->page.runtime, &quota);
        }
        if (loaded && !script_admit_known_working_set(
                          navigation->page.runtime, navigation->budget,
                          source_length, metrics)) {
            (void) navigation_dispatch_node_event(navigation, node, "error");
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            continue;
        }
        if (loaded && navigation->browser_session != NULL && !cors
            && fetch->status_code != 304) {
            script_cache_store(
                navigation->browser_session, resolved, fetch,
                &request_context, &resource_grant);
            /* Sharing may move the buffer even if its descriptor is refused. */
            source = fetch->data;
        }
        if (loaded) {
            loaded = navigation_evaluate_external_script_response(
                navigation, node, source, source_length,
                fetch->effective_url[0] == '\0'
                ? resolved : fetch->effective_url,
                script_response_no_store(fetch));
        }
        if (revalidated) {
            script_cache_revalidate(navigation->browser_session, resolved,
                                    fetch, &request_context,
                                    &resource_grant);
        }
        if (loaded) {
            metrics->loaded++;
            metrics->bytes += source_length;
            (void) script_runtime_record_resource_timing(
                navigation->page.runtime, resolved, "script");
        } else {
            metrics->failed++;
            (void) navigation_dispatch_node_event(
                navigation, node, "error");
        }
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
    }
    return true;
}

static void collect_executable_scripts(ScriptRuntime *runtime,
                                       lxb_dom_node_t *node,
                                       long *scripts,
                                       size_t capacity, size_t *count,
                                       size_t *discovered,
                                       const StreamingScriptState *streaming,
                                       size_t *skipped_nomodule)
{
    lxb_dom_node_t *root = node;
    for (size_t visited = 0;
         node != NULL && visited < SCRIPT_DISCOVERY_VISIT_LIMIT;
        visited++, node = script_discovery_next(root, node)) {
        ScriptKind declared = script_declared_kind(node);
        if (declared == SCRIPT_KIND_CLASSIC && script_has_nomodule(node)) {
            if (skipped_nomodule != NULL) (*skipped_nomodule)++;
            continue;
        }
        if (declared != SCRIPT_KIND_INERT) {
            long handle = script_runtime_node_weak_handle(runtime, node);
            bool parser_executed = false;
            if (handle != 0 && streaming != NULL) {
                for (size_t i = 0;
                     i < streaming->parser_executed_count; i++) {
                    if (streaming->parser_executed[i] == handle) {
                        parser_executed = true;
                        break;
                    }
                }
            }
            /* Streaming metrics already include parser-blocking scripts
               handled at their closing tag.  A post-parse rescan must not
               count a still-live member of that set a second time. */
            if (parser_executed) continue;
            (*discovered)++;
            if (*count < capacity && handle != 0) {
                scripts[(*count)++] = handle;
            }
        }
    }
}

static bool attribute_present(lxb_dom_node_t *node, const char *name)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
           && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name));
}

static bool script_referrer_policy_valid(const char *policy)
{
    return policy != NULL
        && tilefinch_referrer_policy_code(policy, strlen(policy), false)
               != TILEFINCH_REFERRER_POLICY_UNKNOWN;
}

static void script_referrer_policy_for_node(
    lxb_dom_node_t *node, const char *fallback,
    char output[FETCH_REFERRER_POLICY_LIMIT])
{
    const char *selected = fallback == NULL ? "" : fallback;
    char normalized[FETCH_REFERRER_POLICY_LIMIT] = {0};
    size_t length = 0;
    const char *attribute = document_attribute(
        node, "referrerpolicy", &length);
    if (attribute != NULL) {
        if (length != 0 && length < sizeof(normalized)) {
            for (size_t i = 0; i < length; i++) {
                normalized[i] = (char) tolower(
                    (unsigned char) attribute[i]);
            }
            normalized[length] = '\0';
            if (script_referrer_policy_valid(normalized)) {
                selected = normalized;
            }
        }
    }
    if (!script_referrer_policy_valid(selected)) selected = "";
    snprintf(output, FETCH_REFERRER_POLICY_LIMIT, "%s", selected);
}

static bool script_cost_identifier_equal(
    const char *source, size_t begin, size_t end, const char *word)
{
    size_t length = end - begin;
    return strlen(word) == length
        && memcmp(source + begin, word, length) == 0;
}

/* One allocation-free lexical pass. It deliberately ignores strings and
   comments and does not attempt to parse JavaScript; false negatives merely
   retain today's bounded evaluator, while admission rejection requires a
   large cross-origin body plus multiple independent risk signals. */
void script_static_cost_profile(
    const char *source, size_t length, ScriptStaticCostProfile *profile)
{
    if (profile == NULL) return;
    ScriptStaticCostProfile result = {.bytes = length};
    if (source == NULL) {
        *profile = result;
        return;
    }
    bool line_comment = false, block_comment = false;
    char quote = 0;
    bool escaped = false;
    bool saw_document = false;
    for (size_t i = 0; i < length;) {
        char value = source[i];
        if (line_comment) {
            if (value == '\n' || value == '\r') line_comment = false;
            i++;
            continue;
        }
        if (block_comment) {
            if (value == '*' && i + 1u < length && source[i + 1u] == '/') {
                block_comment = false;
                i += 2u;
            } else i++;
            continue;
        }
        if (quote != 0) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quote = 0;
            i++;
            continue;
        }
        if (value == '/' && i + 1u < length) {
            if (source[i + 1u] == '/') {
                line_comment = true;
                i += 2u;
                continue;
            }
            if (source[i + 1u] == '*') {
                block_comment = true;
                i += 2u;
                continue;
            }
        }
        if (value == '\'' || value == '"' || value == '`') {
            quote = value;
            i++;
            continue;
        }
        unsigned char byte = (unsigned char) value;
        if (!(isalpha(byte) || value == '_' || value == '$')) {
            if (!isspace(byte) && value != '.') saw_document = false;
            i++;
            continue;
        }
        size_t begin = i++;
        while (i < length) {
            unsigned char next = (unsigned char) source[i];
            if (!(isalnum(next) || source[i] == '_' || source[i] == '$')) {
                break;
            }
            i++;
        }
        if (script_cost_identifier_equal(source, begin, i, "for")
            || script_cost_identifier_equal(source, begin, i, "while")) {
            result.loops++;
        } else if (script_cost_identifier_equal(source, begin, i, "eval")) {
            result.flags |= SCRIPT_COST_SIGNAL_EVAL;
        } else if (script_cost_identifier_equal(
                       source, begin, i, "Function")) {
            result.flags |= SCRIPT_COST_SIGNAL_FUNCTION_CTOR;
        } else if (script_cost_identifier_equal(
                       source, begin, i, "function")) {
            result.flags |= SCRIPT_COST_SIGNAL_FUNCTION_DECL;
        } else if (saw_document
                   && script_cost_identifier_equal(
                          source, begin, i, "write")) {
            result.flags |= SCRIPT_COST_SIGNAL_DOCUMENT_WRITE;
        }
        saw_document = script_cost_identifier_equal(
            source, begin, i, "document");
    }
    *profile = result;
}

bool script_static_cost_rejects(
    const ScriptStaticCostProfile *signals, bool third_party, bool module)
{
    if (signals == NULL || module || !third_party) return false;
    const unsigned dynamic = SCRIPT_COST_SIGNAL_EVAL
                           | SCRIPT_COST_SIGNAL_FUNCTION_CTOR
                           | SCRIPT_COST_SIGNAL_DOCUMENT_WRITE;
    if (signals->bytes >= 384u * 1024u) return true;
    if (signals->bytes >= 192u * 1024u
        && (signals->flags & dynamic) != 0 && signals->loops >= 2u) {
        return true;
    }
    return signals->bytes >= 256u * 1024u && signals->loops >= 16u;
}

static void script_record_watchdog_profile(
    const ScriptStaticCostProfile *signals,
    ExternalScriptMetrics *metrics)
{
    if (signals == NULL || metrics == NULL) return;
    metrics->watchdog_classification_misses++;
    if (signals->bytes
        > SIZE_MAX - metrics->watchdog_classification_miss_bytes) {
        metrics->watchdog_classification_miss_bytes = SIZE_MAX;
    } else {
        metrics->watchdog_classification_miss_bytes += signals->bytes;
    }
    if (signals->loops
        > SIZE_MAX - metrics->watchdog_classification_miss_loops) {
        metrics->watchdog_classification_miss_loops = SIZE_MAX;
    } else {
        metrics->watchdog_classification_miss_loops += signals->loops;
    }
    metrics->watchdog_classification_miss_flags |= signals->flags;
}

static void script_record_watchdog_miss(
    ScriptRuntime *runtime, const char *source, size_t source_length,
    ExternalScriptMetrics *metrics)
{
    if (runtime == NULL || source == NULL || metrics == NULL
        || !script_runtime_last_slice_interrupted(runtime)) return;
    ScriptStaticCostProfile signals;
    script_static_cost_profile(source, source_length, &signals);
    script_record_watchdog_profile(&signals, metrics);
}

static void script_metrics_record_execution_time(
    ExternalScriptMetrics *metrics, uint64_t started_ns)
{
    if (metrics == NULL) return;
    uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed_us = finished_ns >= started_ns
        ? (finished_ns - started_ns) / UINT64_C(1000) : 0;
    metrics->execution_us = elapsed_us > UINT64_MAX - metrics->execution_us
        ? UINT64_MAX : metrics->execution_us + elapsed_us;
}

static void script_lazy_source_release(void *opaque)
{
    browser_shared_body_release((BrowserSharedBody *) opaque);
}

static bool script_evaluate_external_node(
    ScriptRuntime *runtime, Budget *budget, lxb_dom_node_t *node,
    const char *source,
    size_t source_length, const char *request_url,
    const char *response_url, bool module,
    const char *module_referrer_policy,
    TilefinchCredentialsMode module_credentials,
    BrowserSharedBody *source_body, const ScriptLazyWebpackPlan *lazy_plan,
    bool response_no_store, ExternalScriptMetrics *metrics)
{
    /* The page's script weight (heavy pages, script_admission.h). */
    script_runtime_note_script_source(
        runtime, source_length,
        lazy_plan != NULL ? lazy_plan->largest_factory_bytes : source_length);
    size_t integrity_length = 0;
    const char *integrity = document_attribute(
        node, "integrity", &integrity_length);
    /* Eligibility is response-scoped, not merely request-scoped. A
       same-origin URL may redirect across origins; without a CORS-enabled
       element that response must not become an integrity oracle or execute. */
    if (integrity != NULL && integrity_length != 0
        && (script_runtime_origin_is_opaque(runtime)
            || !tilefinch_url_same_origin(request_url, response_url))
        && !module && !script_crossorigin_present(node)) {
        metrics->policy_refusals++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return false;
    }
    TilefinchIntegrityResult integrity_result =
        tilefinch_resource_integrity_verify(
            integrity, integrity_length, (const uint8_t *) source,
            source_length);
    if (integrity_result == TILEFINCH_INTEGRITY_MISMATCH
        || integrity_result == TILEFINCH_INTEGRITY_INVALID) {
        metrics->policy_refusals++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return false;
    }
    bool trace_failure = tilefinch_trace_script_failures();
    /* ScriptResult is intentionally telemetry-rich and larger than the PSP's
       complete per-frame allowance. Evaluation already retains the canonical
       result in ScriptRuntime, so materialize a temporary copy only when the
       optional failure trace needs its error text. A diagnostic allocation
       miss must never prevent author code from running. */
    ScriptResult *result = trace_failure
        ? budget_calloc(budget, 1, sizeof(*result)) : NULL;
    if (!module && source_body != NULL && lazy_plan != NULL
        && source_body->data == (const unsigned char *) source
        && source_body->length == source_length) {
        BrowserSharedBody *lease = browser_shared_body_retain(source_body);
        if (lease != NULL) {
            metrics->lazy_webpack_candidates++;
            uint64_t evaluation_started_ns =
                tilefinch_platform_monotonic_time_ns();
            ScriptLazyBundleRecordTarget record = {
                .request_url = request_url,
                .body = source_body,
                .no_store = response_no_store
            };
            ScriptLazyEvaluation lazy =
                script_runtime_evaluate_external_lazy_webpack_recorded(
                    runtime, node, source, source_length, response_url,
                    lazy_plan, &record, lease, script_lazy_source_release,
                    result);
            script_metrics_record_execution_time(
                metrics, evaluation_started_ns);
            if (lazy != SCRIPT_LAZY_EVALUATION_FALLBACK) {
                metrics->lazy_webpack_applied++;
                metrics->lazy_webpack_factories += lazy_plan->factory_count;
                metrics->lazy_webpack_source_bytes +=
                    lazy_plan->factory_source_bytes;
                if (lazy == SCRIPT_LAZY_EVALUATION_FAILED
                    && trace_failure) {
                    fprintf(stderr,
                            "lazy-script-evaluation-failure url=\"%s\" "
                            "bytes=%zu factories=%zu error=\"%s\"\n",
                            response_url == NULL ? "" : response_url,
                            source_length, lazy_plan->factory_count,
                            result == NULL ? "<diagnostic unavailable>"
                                           : result->error);
                }
                bool succeeded = lazy == SCRIPT_LAZY_EVALUATION_SUCCEEDED;
                if (!succeeded) metrics->execution_failures++;
                budget_free(budget, result);
                return succeeded;
            }
            metrics->lazy_webpack_fallbacks++;
            browser_shared_body_release(lease);
        }
    }
    uint64_t evaluation_started_ns = tilefinch_platform_monotonic_time_ns();
    bool ok = module
        ? script_runtime_evaluate_external_module_response(
              runtime, node, source, source_length, request_url,
              response_url, module_referrer_policy, module_credentials,
              response_no_store, result)
        : script_runtime_evaluate_external_classic_cached(
              runtime, node, source, source_length, request_url,
              response_url, response_no_store, result);
    script_metrics_record_execution_time(metrics, evaluation_started_ns);
    if (!ok) {
        metrics->execution_failures++;
        script_record_watchdog_miss(
            runtime, source, source_length, metrics);
    }
    if (!ok && trace_failure) {
        fprintf(stderr,
                "script-evaluation-failure url=\"%s\" bytes=%zu "
                "module=%s error=\"%s\"\n",
                response_url == NULL ? "" : response_url, source_length,
                module ? "yes" : "no",
                result == NULL ? "<diagnostic unavailable>" : result->error);
    }
    budget_free(budget, result);
    return ok;
}

static bool script_quota_claim_executable(ScriptRuntime *runtime)
{
    ScriptQuotaReservation reservation = {0};
    ScriptQuotaReserveResult status = script_runtime_script_quota_reserve(
        runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &reservation);
    if (status != SCRIPT_QUOTA_RESERVE_GRANTED) return false;
    if (script_runtime_script_quota_commit(runtime, &reservation, 0)) {
        return true;
    }
    script_runtime_script_quota_abort(runtime, &reservation);
    return false;
}

static bool script_evaluate_inline_node(ScriptRuntime *runtime, Budget *budget,
                                        lxb_dom_node_t *node, bool module,
                                        bool try_data_fast_path,
                                        ExternalScriptMetrics *metrics)
{
    ScriptStaticCostProfile inline_cost = {0};
    if (!try_data_fast_path) {
        for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
             child != NULL; child = child->next) {
            size_t length = 0;
            const char *source = document_text_data(child, &length);
            if (source == NULL || length == 0) continue;
            ScriptStaticCostProfile part;
            script_static_cost_profile(source, length, &part);
            inline_cost.bytes = length > SIZE_MAX - inline_cost.bytes
                ? SIZE_MAX : inline_cost.bytes + length;
            inline_cost.loops = part.loops > SIZE_MAX - inline_cost.loops
                ? SIZE_MAX : inline_cost.loops + part.loops;
            inline_cost.flags |= part.flags;
        }
    }
    bool trace_failure = tilefinch_trace_script_failures();
    char id_copy[128] = {0};
    size_t id_copy_length = 0;
    if (trace_failure) {
        size_t id_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        if (id != NULL) {
            id_copy_length = id_length < sizeof(id_copy) - 1
                ? id_length : sizeof(id_copy) - 1;
            memcpy(id_copy, id, id_copy_length);
            id_copy[id_copy_length] = '\0';
        }
    }
    ScriptResult *result = trace_failure
        ? budget_calloc(budget, 1, sizeof(*result)) : NULL;
    bool ok = false;
    ScriptInlineDataEvaluation data = SCRIPT_INLINE_DATA_FALLBACK;
    size_t data_bytes = 0;
    uint64_t evaluation_started_ns = tilefinch_platform_monotonic_time_ns();
    if (!module && try_data_fast_path) {
        data = script_runtime_evaluate_inline_data(
            runtime, node, &data_bytes);
        if (tilefinch_trace_script_attempts()) {
            fprintf(stderr,
                    "inline-data-fast-path status=%d bytes=%zu\n",
                    (int) data, data_bytes);
        }
    }
    if (data == SCRIPT_INLINE_DATA_APPLIED) {
        ok = true;
        if (metrics != NULL) {
            metrics->inline_data_fast_paths++;
            if (data_bytes > SIZE_MAX - metrics->inline_data_fast_path_bytes) {
                metrics->inline_data_fast_path_bytes = SIZE_MAX;
            } else {
                metrics->inline_data_fast_path_bytes += data_bytes;
            }
        }
    } else if (data == SCRIPT_INLINE_DATA_FAILED) {
        ok = false;
    } else {
        ok = script_runtime_evaluate_inline(runtime, node, module, result);
    }
    script_metrics_record_execution_time(metrics, evaluation_started_ns);
    /* An author exception terminates this script, not the document.  In
       particular, a realm-local OOM can leave the allocator at its ceiling
       even after QuickJS has released the failed compilation's temporaries;
       collect at this safe top-level boundary so DOMContentLoaded and later
       small scripts still have a chance to run inside the unchanged limit. */
    if (!ok) (void) script_runtime_collect_and_trim(runtime);
    if (!ok) metrics->execution_failures++;
    if (!ok && script_runtime_last_slice_interrupted(runtime)) {
        script_record_watchdog_profile(&inline_cost, metrics);
    }
    /* Author code may remove this element (or replace its whole ancestor
       subtree) during evaluation.  Diagnostics therefore use the bounded
       pre-evaluation copy and never dereference `node` afterwards. */
    if (!ok && trace_failure) {
        fprintf(stderr,
                "inline-script-evaluation-failure id=\"%.*s\" module=%s "
                "error=\"%s\"\n",
                (int) id_copy_length, id_copy,
                module ? "yes" : "no",
                result == NULL ? "<diagnostic unavailable>" : result->error);
    }
    budget_free(budget, result);
    return ok;
}

static bool script_admit_inline_node(
    ScriptRuntime *runtime, Budget *budget, lxb_dom_node_t *node,
    const TilefinchContentSecurityPolicy *csp,
    ExternalScriptMetrics *metrics);

static bool script_admit_inline_data_node(
    ScriptRuntime *runtime, Budget *budget, lxb_dom_node_t *node,
    const TilefinchContentSecurityPolicy *csp,
    ExternalScriptMetrics *metrics, size_t *source_length)
{
    if (source_length != NULL) *source_length = 0;
    if (runtime == NULL || budget == NULL || node == NULL || metrics == NULL
        || source_length == NULL) return false;
    if (!tilefinch_csp_allows_inline_script(csp, node)) {
        metrics->failed++;
        metrics->policy_refusals++;
        return false;
    }
    if (!script_runtime_inline_data_candidate(node, source_length)) {
        return false;
    }
    /* The source already belongs to the DOM. The fast path needs one bounded
       copy plus the parsed JSON graph, but no compiler/bytecode expansion.
       Reserve one source-sized working set in each allocator while preserving
       the ordinary presentation floor. */
    return script_admit_known_working_set_with_reserve(
        runtime, budget, *source_length,
        SCRIPT_PRESENTATION_RESERVE_BYTES, *source_length, metrics);
}

/* Execute a classic inline script with data-shape admission ahead of the
   realm's executable quota. A false-positive candidate falls back to the
   compiler only after claiming the ordinary quota, preserving JavaScript
   semantics without charging strict JSON assignments for bytecode they do
   not create. Rejection is a clean no-op at the document level. */
static bool script_execute_inline_admitted(
    ScriptRuntime *runtime, Budget *budget, lxb_dom_node_t *node, bool module,
    bool executable_precounted, bool prefer_data,
    const TilefinchContentSecurityPolicy *csp,
    ExternalScriptMetrics *metrics)
{
    script_runtime_script_mark_started(runtime, node);
    if (!module && prefer_data) {
        size_t source_length = 0;
        if (script_admit_inline_data_node(
                runtime, budget, node, csp, metrics, &source_length)) {
            uint64_t evaluation_started_ns =
                tilefinch_platform_monotonic_time_ns();
            ScriptInlineDataEvaluation data =
                script_runtime_evaluate_inline_data(runtime, node, NULL);
            script_metrics_record_execution_time(
                metrics, evaluation_started_ns);
            if (data == SCRIPT_INLINE_DATA_APPLIED) {
                metrics->inline_data_fast_paths++;
                metrics->inline_data_quota_exemptions++;
                if (source_length
                    > SIZE_MAX - metrics->inline_data_fast_path_bytes) {
                    metrics->inline_data_fast_path_bytes = SIZE_MAX;
                } else {
                    metrics->inline_data_fast_path_bytes += source_length;
                }
                return true;
            }
            if (data == SCRIPT_INLINE_DATA_FAILED) {
                metrics->failed++;
                metrics->execution_failures++;
                (void) script_runtime_collect_and_trim(runtime);
                return !script_runtime_document_refresh_failed(runtime);
            }
            /* The allocation-free scan is conservative, while JS_ParseJSON
               remains authoritative. An ambiguity receives normal script
               treatment rather than being silently discarded. */
        } else if (script_runtime_inline_data_candidate(node, NULL)) {
            return true;
        }
    }
    if (!executable_precounted && !script_quota_claim_executable(runtime)) {
        metrics->skipped_quota++;
        return true;
    }
    if (!script_admit_inline_node(runtime, budget, node, csp, metrics)) {
        return true;
    }
    if (!script_evaluate_inline_node(
            runtime, budget, node, module, false, metrics)) {
        metrics->failed++;
    }
    return !script_runtime_document_refresh_failed(runtime);
}

static bool script_admit_inline_node(ScriptRuntime *runtime, Budget *budget,
                                     lxb_dom_node_t *node,
                                     const TilefinchContentSecurityPolicy *csp,
                                     ExternalScriptMetrics *metrics)
{
    if (runtime == NULL || budget == NULL || node == NULL || metrics == NULL) {
        return false;
    }
    if (!tilefinch_csp_allows_inline_script(csp, node)) {
        metrics->failed++;
        metrics->policy_refusals++;
        return false;
    }
    size_t source_length = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > SIZE_MAX - source_length) {
            metrics->skipped_pressure++;
            budget_record_pressure(
                budget, BUDGET_PRESSURE_JAVASCRIPT, SIZE_MAX, 0);
            return false;
        }
        source_length += length;
    }
    if (source_length == 0) return true;
    /* The QuickJS heap keeps the expansion window
       script_runtime_inline_execution_reserve() selects; the page Budget
       keeps the presentation reserve, which already covers the inline
       script's DOM work. */
    size_t execution_reserve =
        script_runtime_inline_execution_reserve(runtime, source_length);
    size_t budget_reserve = SCRIPT_PRESENTATION_RESERVE_BYTES;
    bool admitted = script_admit_known_working_set_with_reserve(
        runtime, budget, source_length,
        budget_reserve, execution_reserve, metrics);
    if (tilefinch_trace_script_failures()) {
        size_t heap_remaining = script_runtime_heap_remaining(runtime);
        fprintf(stderr,
                "inline-script-admission bytes=%zu heap-remaining=%zu "
                "budget-reserve=%zu heap-reserve=%zu admitted=%s\n",
                source_length, heap_remaining,
                budget_reserve, execution_reserve,
                admitted ? "yes" : "no");
    }
    return admitted;
}

static bool script_evaluate_data_url_node(
    ScriptRuntime *runtime, Budget *budget, lxb_dom_node_t *node,
    const char *url, size_t url_length, const char *referrer_policy,
    bool module, size_t maximum_file_bytes, long timeout_ms,
    ExternalScriptMetrics *metrics,
    DocumentScriptProcessResult *process_result)
{
    unsigned char *source = NULL;
    size_t source_length = 0;
    char media_type[128];
    DataUrlDecodeResult decoded = data_url_decode(
        budget, url, url_length, maximum_file_bytes, &source,
        &source_length, media_type, sizeof(media_type));
    if (decoded != DATA_URL_DECODED
        || (module && !script_module_mime_type_allowed(media_type))) {
        if (decoded == DATA_URL_TOO_LARGE) {
            metrics->skipped_quota++;
            if (process_result != NULL) {
                *process_result = DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT;
            }
        }
        else metrics->failed++;
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "data-script-decode-failure encoded=%zu result=%d "
                    "media-type=\"%s\" module=%s\n",
                    url_length, (int) decoded, media_type,
                    module ? "yes" : "no");
        }
        budget_free(budget, source);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }

    script_metrics_record_source_work(metrics, source_length);

    ScriptQuotaReservation quota = {0};
    bool progress_failed = false;
    ScriptQuotaReserveResult quota_status = script_quota_reserve_bounded(
        runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE, source_length,
        timeout_ms, &quota, &progress_failed);
    if (progress_failed) {
        budget_free(budget, source);
        return false;
    }
    if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
        || !script_runtime_script_quota_commit(
               runtime, &quota, source_length)) {
        script_runtime_script_quota_abort(runtime, &quota);
        metrics->skipped_quota++;
        budget_free(budget, source);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    if (!script_admit_known_working_set(
            runtime, budget, source_length, metrics)) {
        budget_free(budget, source);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }

    char effective_policy[FETCH_REFERRER_POLICY_LIMIT];
    script_referrer_policy_for_node(
        node, referrer_policy, effective_policy);
    TilefinchCredentialsMode credentials = module
        ? script_cors_credentials(node) : TILEFINCH_CREDENTIALS_INCLUDE;
    metrics->attempted++;
    bool ok = script_evaluate_external_node(
        runtime, budget, node, (const char *) source, source_length,
        url, url, module, module ? effective_policy : NULL, credentials,
        NULL, NULL, true, metrics);
    if (ok) {
        metrics->loaded++;
        metrics->bytes += source_length;
        (void) script_runtime_record_resource_timing(runtime, url, "script");
    } else {
        metrics->failed++;
    }
    budget_free(budget, source);
    return true;
}

bool document_script_is_parser_blocking(lxb_dom_node_t *element)
{
    if (element == NULL || script_kind(element) != SCRIPT_KIND_CLASSIC) {
        return false;
    }
    size_t source_length = 0;
    const char *source = script_source_attribute(element, &source_length);
    bool external = source != NULL && source_length != 0;
    return !external || (!attribute_present(element, "async")
                         && !attribute_present(element, "defer"));
}

static bool execute_external_node(
    ScriptRuntime *runtime, Budget *budget, BrowserSession *session,
    FetchScheduler *scheduler,
    lxb_dom_node_t *node, const char *base_url, const char *document_url,
    const char *referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    bool module, bool executable_precounted,
    size_t maximum_total_bytes, size_t maximum_file_bytes,
    long timeout_ms, ExternalScriptMetrics *metrics,
    DocumentScriptProcessResult *process_result)
{
    (void) maximum_total_bytes;
    if (process_result != NULL) {
        *process_result = DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    }
    script_runtime_script_mark_started(runtime, node);
    size_t reference_length = 0;
    const char *reference = script_source_attribute(node, &reference_length);
    if (reference == NULL || reference_length == 0) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "external-script-reference-failure reference=%s "
                    "length=%zu\n",
                    reference == NULL ? "null" : "set", reference_length);
        }
        return false;
    }
    uint8_t csp_grant = script_element_csp_grant(
        content_security_policy, runtime, node);
    if (reference_length >= 5
        && strncasecmp(reference, "data:", 5) == 0) {
        if (!executable_precounted
            && !script_quota_claim_executable(runtime)) {
            metrics->skipped_quota++;
            (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
            return true;
        }
        /* CSP sees only a data: URL's scheme, so check its header (media
           type and parameters) rather than requiring the whole bundle to
           fit a URL buffer: Reddit ships a 34 KB data: module. The decoder
           below bounds the payload by the per-file script limit. */
        char data_url[256];
        size_t header_length = 0;
        while (header_length < reference_length
               && reference[header_length] != ','
               && header_length + 2u < sizeof(data_url)) header_length++;
        if (header_length >= reference_length
            || reference[header_length] != ',') {
            metrics->failed++;
            (void) script_runtime_dispatch_node(
                runtime, node, "error", NULL);
            return true;
        }
        memcpy(data_url, reference, header_length + 1u);
        data_url[header_length + 1u] = '\0';
        if (!tilefinch_csp_allows_request_granted(
                content_security_policy, TILEFINCH_DESTINATION_SCRIPT,
                data_url, csp_grant)) {
            metrics->failed++;
            metrics->policy_refusals++;
            (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
            return true;
        }
        return script_evaluate_data_url_node(
            runtime, budget, node, reference, reference_length,
            referrer_policy, module, maximum_file_bytes, timeout_ms,
            metrics, process_result);
    }
    if (reference_length >= NAVIGATION_URL_LIMIT) {
        metrics->failed++;
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "external-script-url-too-long length=%zu limit=%u\n",
                    reference_length, (unsigned) NAVIGATION_URL_LIMIT);
        }
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    char copy[NAVIGATION_URL_LIMIT];
    memcpy(copy, reference, reference_length); copy[reference_length] = '\0';
    char resolved[NAVIGATION_URL_LIMIT];
    if (!fetch_resolve_url(base_url, copy, resolved, sizeof(resolved))) {
        metrics->failed++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    if (!tilefinch_csp_allows_request_granted(
            content_security_policy, TILEFINCH_DESTINATION_SCRIPT,
            resolved, csp_grant)) {
        metrics->failed++;
        metrics->policy_refusals++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    bool cors = module || script_crossorigin_present(node);
    bool initiator_opaque = script_runtime_origin_is_opaque(runtime);
    if (!script_integrity_request_eligible(
            node, document_url, resolved, initiator_opaque)) {
        metrics->failed++;
        metrics->policy_refusals++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    if (module) {
        ScriptModuleMapStatus module_status =
            script_runtime_module_map_status(runtime, resolved);
        if (module_status == SCRIPT_MODULE_MAP_EVALUATED
            || module_status == SCRIPT_MODULE_MAP_FAILED) {
            bool succeeded = module_status == SCRIPT_MODULE_MAP_EVALUATED;
            metrics->module_map_hits++;
            if (succeeded) metrics->loaded++;
            else metrics->failed++;
            (void) script_runtime_dispatch_node(
                runtime, node, succeeded ? "load" : "error", NULL);
            return true;
        }
    }
    if (!executable_precounted
        && !script_quota_claim_executable(runtime)) {
        metrics->skipped_quota++;
        script_trace_skip(resolved);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    char request_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    script_referrer_policy_for_node(
        node, referrer_policy, request_referrer_policy);
    TilefinchCredentialsMode module_credentials = cors
        ? script_cors_credentials(node)
        : initiator_opaque ? TILEFINCH_CREDENTIALS_OMIT
                           : TILEFINCH_CREDENTIALS_INCLUDE;
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
    if (cors && initiator_opaque) {
        memcpy(initiator_origin, "null", sizeof("null"));
    } else if (cors && !tilefinch_url_origin(
                   document_url, initiator_origin,
                   sizeof(initiator_origin))) {
        metrics->failed++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    ScriptQuotaReservation quota = {0};
    bool progress_failed = false;
    ScriptQuotaReserveResult quota_status = script_quota_reserve_bounded(
        runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
        maximum_file_bytes, timeout_ms, &quota, &progress_failed);
    if (progress_failed) return false;
    if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
        || quota.reserved_bytes == 0) {
        script_runtime_script_quota_abort(runtime, &quota);
        metrics->skipped_quota++; script_trace_skip(resolved);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    size_t response_limit = quota.reserved_bytes;
    char top_level_url[NAVIGATION_URL_LIMIT];
    if (!script_runtime_copy_top_level_url(
            runtime, top_level_url, sizeof(top_level_url))) {
        script_runtime_script_quota_abort(runtime, &quota);
        metrics->failed++;
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    TilefinchRequestContext request_context = script_request_context(
        document_url, top_level_url, resolved, cors, module_credentials,
        initiator_opaque);
    request_context.csp_grant = csp_grant;
    const BrowserCacheEntry *cached = NULL;
    /* An installed app's classic script restored as bytecode only runs
       without reading its source unless integrity metadata, or the
       cross-origin cost check below, needs the bytes. */
    size_t deferred_length = 0;
    bool deferred = !module && !cors && session != NULL
        && !script_integrity_present(node)
        && !content_blocker_would_block(
               session->content_blocker, resolved, document_url,
               "script", "no-cors")
        && browser_session_offline_script_match(
               session, resolved, &request_context, &deferred_length)
        && (deferred_length < 192u * 1024u
            || tilefinch_url_same_origin(document_url, resolved));
    BrowserCacheStatus cache_status = deferred ? BROWSER_CACHE_FRESH
        : module
        ? script_module_cache_match(
            session, resolved, initiator_origin, top_level_url,
            initiator_opaque, module_credentials, &cached)
        : cors ? BROWSER_CACHE_MISS
               : script_cache_match(
                   session, resolved, document_url, &request_context,
                   &cached);
    if (module && cached != NULL
        && !script_module_mime_type_allowed(cached->content_type)) {
        cached = NULL;
        cache_status = BROWSER_CACHE_MISS;
    }
    if (cache_status == BROWSER_CACHE_FRESH) {
        size_t fresh_length = deferred ? deferred_length
            : cached == NULL ? 0 : cached->length;
        if ((cached == NULL && !deferred) || fresh_length > response_limit) {
            script_runtime_script_quota_abort(runtime, &quota);
            metrics->skipped_quota++; script_trace_skip(resolved);
            if (fresh_length > maximum_file_bytes)
                script_runtime_note_oversized_script(runtime, fresh_length);
            if (fresh_length > maximum_file_bytes
                && process_result != NULL) {
                *process_result = DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT;
            }
            (void) script_runtime_dispatch_node(
                runtime, node, "error", NULL);
            return true;
        }
        ScriptCacheSource cached_source = {
            .length = deferred_length, .budget = budget
        };
        if (!deferred
            && !script_cache_source_acquire(budget, cached, &cached_source)) {
            script_runtime_script_quota_abort(runtime, &quota);
            metrics->failed++;
            (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
            return true;
        }
        size_t cached_length = cached_source.length;
        script_metrics_record_source_work(metrics, cached_length);
        const char *cached_response_url = module
            ? cached->module_effective_url : resolved;
        bool cost_rejected = false;
        if (!module && !deferred && cached_length >= 192u * 1024u
            && !tilefinch_url_same_origin(
                   document_url, cached_response_url)) {
            ScriptStaticCostProfile cost;
            script_static_cost_profile(
                cached_source.data, cached_length, &cost);
            cost_rejected = script_static_cost_rejects(
                &cost, true, false);
        }
        if (cost_rejected) {
            script_runtime_script_quota_abort(runtime, &quota);
            script_cache_source_release(&cached_source);
            metrics->cost_class_rejections++;
            (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
            return true;
        }
        if (!script_runtime_script_quota_commit(
                runtime, &quota, cached_length)) {
            script_runtime_script_quota_abort(runtime, &quota);
            script_cache_source_release(&cached_source);
            metrics->skipped_quota++; script_trace_skip(resolved);
            (void) script_runtime_dispatch_node(
                runtime, node, "error", NULL);
            return true;
        }
        ScriptLazyWebpackPlan lazy_plan;
        bool has_lazy_plan = cached_source.body != NULL
            && script_runtime_lazy_webpack_plan_eligible(
                runtime, budget, resolved, cached_source.data, cached_length,
                module, &lazy_plan);
        size_t compile_working_bytes = has_lazy_plan
            ? lazy_plan.largest_factory_bytes : cached_length;
        if (!script_admit_known_working_set(
                runtime, budget, compile_working_bytes, metrics)) {
            if (tilefinch_trace_script_failures()) {
                fprintf(stderr,
                        "script-admission-reject url=%s working=%zu\n",
                        resolved,
                        compile_working_bytes);
            }
            if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
            script_cache_source_release(&cached_source);
            (void) script_runtime_dispatch_node(
                runtime, node, "error", NULL);
            return true;
        }
        metrics->attempted++; script_trace_attempt(resolved);
        metrics->cache_hits++;
        const char *module_policy = module
            ? script_module_effective_referrer_policy(
                  request_referrer_policy,
                  cached->module_response_referrer_policy)
            : NULL;
        bool ok = script_evaluate_external_node(
            runtime, budget, node, cached_source.data, cached_length,
            resolved,
            module ? cached->module_effective_url : resolved,
            module, module_policy, module_credentials, cached_source.body,
            has_lazy_plan ? &lazy_plan : NULL, false, metrics);
        if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
        script_cache_source_release(&cached_source);
        if (ok) {
            metrics->loaded++; metrics->bytes += cached_length;
            (void) script_runtime_record_resource_timing(runtime, resolved,
                                                         "script");
        }
        else metrics->failed++;
        return true;
    }
    bool pressure_capped = false;
    if (!script_bound_network_working_set(
            runtime, budget, &response_limit, &pressure_capped, metrics)) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr, "script-network-bound-reject url=%s\n",
                    resolved);
        }
        script_runtime_script_quota_abort(runtime, &quota);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        return true;
    }
    bool caller_source_limit_enforced =
        !pressure_capped && response_limit == maximum_file_bytes;
    metrics->attempted++; script_trace_attempt(resolved);
    ScriptCacheSource cached_source = {0};
    bool cached_source_ready = cached != NULL
        && script_cache_source_acquire(budget, cached, &cached_source);
    FetchRequest transport = {
        .allow_http_errors = true,
        .send_low_client_hints = true,
        .accept = "*/*",
        .if_none_match = cached == NULL ? NULL : cached->etag,
        .if_modified_since = cached == NULL ? NULL : cached->last_modified,
        .cors_cached_response_validated = module && cached != NULL
            && !cached->module_cors_redirect_origin_tainted
            && (cached->etag[0] != '\0'
                || cached->last_modified[0] != '\0'),
    };
    FetchPreparedPageRequest prepared;
    bool request_ready = fetch_prepare_page_request_context(
        &request_context, document_url, request_referrer_policy,
        session, content_security_policy, NULL, &transport,
        &prepared, NULL);
    const FetchRequest *request = request_ready
        ? fetch_prepared_page_request(&prepared) : NULL;
    /* Preserve the transport's policy telemetry by still submitting the
       request, but snapshot its sink-adjacent decision so an enqueue refusal
       remains distinguishable from an author-code/load failure. */
    bool request_policy_refused = request != NULL
        && (!fetch_request_security_allows_target(request, resolved)
            || (session != NULL && session->content_blocker != NULL
                && content_blocker_would_block(
                       session->content_blocker, resolved,
                       request->initiator_url, request->sec_fetch_dest,
                       request->sec_fetch_mode)));
    FetchResult *fetch = fetch_result_create(budget);
    if (fetch == NULL) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "script-fetch-result-allocation-failure url=\"%s\" "
                    "remaining=%zu\n",
                    resolved, budget_remaining(budget));
        }
        script_runtime_script_quota_abort(runtime, &quota);
        script_cache_source_release(&cached_source);
        return false;
    }
    if (request == NULL || !fetch_scheduler_request(scheduler, resolved, request,
                                 response_limit, timeout_ms, fetch)) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "script-fetch-failure url=\"%s\" limit=%zu "
                    "pressure-capped=%s error=\"%s\"\n",
                    resolved, response_limit,
                    pressure_capped ? "yes" : "no", fetch->error);
        }
        if (request == NULL) {
            metrics->failed++;
        } else if (request_policy_refused) {
            metrics->failed++;
            metrics->policy_refusals++;
        } else if (fetch->response_limit_exceeded && !pressure_capped) {
            metrics->skipped_quota++;
            script_runtime_note_oversized_script(runtime, response_limit);
        } else if (script_fetch_was_pressure_rejected(fetch, pressure_capped)) {
            metrics->skipped_pressure++;
        } else {
            metrics->failed++;
        }
        if (request != NULL && fetch->response_limit_exceeded
            && caller_source_limit_enforced
            && process_result != NULL) {
            *process_result = DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT;
        }
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        script_runtime_script_quota_abort(runtime, &quota);
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return true;
    }
    /* The fetcher grows geometrically. Compilation needs the complete source
       but not its unused capacity; trimming here prevents a 2.1 MiB bundle
       from pinning a 4 MiB response allocation throughout the page realm. */
    script_compact_fetch_source(fetch);
    const char *source = fetch->data;
    size_t source_length = fetch->length;
    const char *response_url = fetch->effective_url[0] == '\0'
        ? (module && fetch->status_code == 304 && cached != NULL
            ? cached->module_effective_url : resolved)
        : fetch->effective_url;
    if (fetch->status_code == 304 && cached != NULL) {
        if (cached_source_ready) {
            source = cached_source.data;
            source_length = cached_source.length;
            metrics->cache_hits++;
        }
    }
    script_metrics_record_source_work(metrics, source_length);
    bool ok = (fetch->status_code >= 200 && fetch->status_code < 300)
        || (fetch->status_code == 304 && cached_source_ready);
    const char *accepted_content_type = fetch->status_code == 304
        && cached != NULL ? cached->content_type : fetch->content_type;
    if (ok && module) {
        ok = fetch->status_code == 304
            ? script_module_revalidated_mime_allowed(
                accepted_content_type, fetch->content_type)
            : script_module_mime_type_allowed(accepted_content_type);
    }
    TilefinchResourceGrant resource_grant = {0};
    if (ok && !module && fetch->status_code == 304) {
        if (fetch->effective_url[0] == '\0') {
            snprintf(fetch->effective_url, sizeof(fetch->effective_url),
                     "%s", response_url);
        }
        ok = cached != NULL && cached->resource_grant_valid
            && fetch_resource_grant_revalidate_304(
                fetch, &request_context, &cached->resource_grant, false,
                script_module_mime_type_allowed(accepted_content_type),
                false, &resource_grant, NULL);
    } else if (ok) {
        if (fetch->effective_url[0] == '\0') {
            snprintf(fetch->effective_url, sizeof(fetch->effective_url),
                     "%s", response_url);
        }
        ok = script_resource_grant(
            fetch, &request_context, accepted_content_type, module,
            &resource_grant);
    }
    if (ok && fetch->status_code == 304) {
        ok = module
            ? script_module_cache_record(
                session, resolved, response_url, initiator_origin,
                top_level_url, initiator_opaque, module_credentials, fetch,
                true, tilefinch_platform_monotonic_time_ns())
            : (script_cache_revalidate(
                   session, resolved, fetch, &request_context,
                   &resource_grant), true);
    }
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT] = {0};
    if (ok && module && fetch->status_code != 304) {
        bool header_present = false;
        ok = fetch_response_referrer_policy(
            fetch, &header_present, response_referrer_policy);
    }
    if (ok) {
        script_accept_response_cookies(
            session, &request_context, fetch, response_url);
    }
    BrowserSharedBody *source_body = fetch->status_code == 304
        ? cached_source.body : NULL;
    ScriptLazyWebpackPlan lazy_plan;
    /* The planner, or a bundle record of exactly these bytes. */
    bool has_lazy_plan = ok && script_runtime_lazy_webpack_plan_eligible(
        runtime, budget, resolved, source, source_length, module,
        &lazy_plan);
    if (has_lazy_plan && fetch->status_code != 304) {
        bool shared = fetch_result_share_body(fetch);
        source = fetch->data;
        if (shared) {
            source_body = fetch->shared_body;
        } else {
            script_lazy_webpack_plan_destroy(&lazy_plan);
            has_lazy_plan = false;
        }
    }
    if (has_lazy_plan && source_body == NULL) {
        script_lazy_webpack_plan_destroy(&lazy_plan);
        has_lazy_plan = false;
    }
    if (ok && !module && source_length >= 192u * 1024u
        && !tilefinch_url_same_origin(document_url, response_url)) {
        ScriptStaticCostProfile cost;
        script_static_cost_profile(source, source_length, &cost);
        if (script_static_cost_rejects(&cost, true, false)) {
            script_runtime_script_quota_abort(runtime, &quota);
            if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
            metrics->cost_class_rejections++;
            (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            return true;
        }
    }
    if (ok && !script_runtime_script_quota_commit(
                  runtime, &quota, source_length)) {
        script_runtime_script_quota_abort(runtime, &quota);
        if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
        metrics->skipped_quota++; script_trace_skip(resolved);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return true;
    }
    if (!ok) script_runtime_script_quota_abort(runtime, &quota);
    size_t compile_working_bytes = has_lazy_plan
        ? lazy_plan.largest_factory_bytes : source_length;
    if (ok && !script_admit_known_working_set(
                  runtime, budget, compile_working_bytes, metrics)) {
        if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return true;
    }
    /* HTTP caching is determined by the validated response, not by whether
       author code later compiles or executes.  Store before evaluation so
       the classic evaluator can attach serialized bytecode to this exact
       response entry on its first navigation. */
    if (ok && session != NULL && fetch->status_code != 304
        && (!cors || module)
        && fetch->length != 0) {
        if (module) {
            (void) script_module_cache_record(
                session, resolved, response_url, initiator_origin,
                top_level_url, initiator_opaque, module_credentials, fetch,
                false, tilefinch_platform_monotonic_time_ns());
        } else {
            script_cache_store(
                session, resolved, fetch, &request_context,
                &resource_grant);
        }
        source = fetch->data;
    }
    if (ok) {
        const char *module_policy = module
            ? script_module_effective_referrer_policy(
                  request_referrer_policy,
                  fetch->status_code == 304 && cached != NULL
                    ? cached->module_response_referrer_policy
                    : response_referrer_policy)
            : NULL;
        ok = script_evaluate_external_node(
            runtime, budget, node, source, source_length, resolved,
            response_url, module, module_policy, module_credentials,
            source_body,
            has_lazy_plan ? &lazy_plan : NULL,
            script_response_no_store(fetch), metrics);
    } else {
        (void) script_runtime_dispatch_node(runtime, node, "error", NULL);
    }
    if (has_lazy_plan) script_lazy_webpack_plan_destroy(&lazy_plan);
    if (ok) {
        metrics->loaded++; metrics->bytes += source_length;
        (void) script_runtime_record_resource_timing(runtime, resolved,
                                                     "script");
    }
    else metrics->failed++;
    fetch_result_free(fetch);
    script_cache_source_release(&cached_source);
    return true;
}

static bool execute_external_node_live(
    PocDocument *document, ScriptRuntime *runtime, Budget *budget,
    BrowserSession *session, FetchScheduler *scheduler,
    lxb_dom_node_t *node, const char *fallback_base_url,
    const char *document_url, const char *referrer_policy, bool module,
    bool executable_precounted,
    size_t maximum_total_bytes, size_t maximum_file_bytes,
    long timeout_ms, ExternalScriptMetrics *metrics)
{
    char base_url[NAVIGATION_URL_LIMIT];
    char current_document_url[NAVIGATION_URL_LIMIT];
    const char *live_document_url = document_url;
    if (runtime != NULL
        && script_runtime_copy_document_url(
            runtime, current_document_url,
            sizeof(current_document_url))) {
        live_document_url = current_document_url;
    }
    const char *live_base = fallback_base_url;
    if (document != NULL) {
        if (!document_base_url(document, live_document_url, base_url,
                               sizeof(base_url))) {
            if (tilefinch_trace_script_failures()) {
                fprintf(stderr,
                        "script-live-base-failure document-url=\"%s\"\n",
                        live_document_url == NULL ? "" : live_document_url);
            }
            return false;
        }
        live_base = base_url;
        script_runtime_invalidate_document_base(runtime);
    }
    bool executed = execute_external_node(
        runtime, budget, session, scheduler, node, live_base,
        live_document_url,
        referrer_policy,
        document == NULL ? NULL : &document->content_security_policy,
        module, executable_precounted,
        maximum_total_bytes, maximum_file_bytes,
        timeout_ms, metrics, NULL);
    return executed && !script_runtime_document_refresh_failed(runtime);
}

/* QuickJS loads a module graph one synchronous request at a time: it asks
   for a module, compiles it, then asks for each static dependency in turn.
   Over the PSP's Wi-Fi that serialized chatgpt.com's ~70 modules into
   35-53 s advances with no frame presented. When a module arrives, its
   static dependencies are requested in parallel; the synchronous load then
   waits for the matching request instead of starting a new one. A prefetch
   reserves a small response bound so it cannot starve other requests; one
   that fails or overflows falls back to the ordinary request. */
/* A module fetch that receives nothing for this long fails and is retried
   on a new connection instead of holding the synchronous graph load until
   the whole deadline. */
#define MODULE_FETCH_STALL_SECONDS 6L
#define MODULE_PREFETCH_LIMIT 6u
#define MODULE_PREFETCH_BYTES (128u * 1024u)
#define MODULE_PREFETCH_STALE_US UINT64_C(20000000)

typedef struct {
    uint64_t id;
    uint64_t started_us;
    char url[NAVIGATION_URL_LIMIT];
} ModulePrefetch;

typedef struct {
    ScriptRuntime *runtime;
    Budget *budget;
    BrowserSession *session;
    char document_url[NAVIGATION_URL_LIMIT];
    char top_level_url[NAVIGATION_URL_LIMIT];
    size_t maximum_file_bytes;
    long timeout_ms;
    ExternalScriptMetrics *metrics;
    ExternalScriptMetrics retained_metrics;
    ModuleBodyLease *body_leases;
    ModulePrefetch prefetches[MODULE_PREFETCH_LIMIT];
    size_t prefetch_count;
} ModuleLoadContext;

#include "script_bundle_diagnostics.inc"

static void pipeline_context_destroy(void *opaque)
{
    ModuleLoadContext *context = opaque;
    if (context == NULL) return;
    ModuleBodyLease *lease = context->body_leases;
    while (lease != NULL) {
        ModuleBodyLease *next = lease->next;
        browser_shared_body_release(lease->body);
        budget_free(context->budget, lease);
        lease = next;
    }
    budget_free(context->budget, context);
}

static bool pipeline_context_finish_pass(
    ModuleLoadContext *context, ExternalScriptMetrics *output, bool result)
{
    if (context != NULL && output != NULL) {
        *output = context->retained_metrics;
    }
    return result;
}

static bool pipeline_module_lease_body(ModuleLoadContext *context,
                                       BrowserSharedBody *body,
                                       char **source)
{
    if (context == NULL || body == NULL || source == NULL
        || body->data == NULL || body->length == 0
        || !script_source_has_terminator(body->data, body->length)) {
        return false;
    }
    ModuleBodyLease *lease = budget_malloc(context->budget, sizeof(*lease));
    if (lease == NULL) return false;
    lease->body = browser_shared_body_retain(body);
    if (lease->body == NULL) {
        budget_free(context->budget, lease);
        return false;
    }
    lease->source = (char *) lease->body->data;
    lease->next = context->body_leases;
    context->body_leases = lease;
    *source = lease->source;
    return true;
}

/* Why a module request was refused. Page code usually sees only "could
   not load module"; chatgpt.com's Send failed that way on the PSP. Logged in
   validation builds and with TILEFINCH_TRACE_SCRIPT_FAILURES. */
static void module_load_refused(const char *url, const char *reason)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    static unsigned logged;
    if (logged < 24u) {
        logged++;
        char message[256];
        snprintf(message, sizeof(message),
                 "tilefinch-module-refused: url=%.100s reason=%.150s",
                 url == NULL ? "" : url, reason);
        tilefinch_platform_log_message(message);
    }
#else
    if (tilefinch_trace_script_failures())
        fprintf(stderr, "module-refused reason=%s url=%s\n", reason,
                url == NULL ? "<null>" : url);
#endif
}

/* A module the page is waiting on must not be refused because other
   responses briefly hold the scheduler's byte pool (chatgpt.com's page
   fetches reserve 1 MiB each). Pump until capacity frees, bounded by the
   request timeout; completed requests release their unused bound. */
static void script_wait_for_fetch_capacity(FetchScheduler *scheduler,
                                           size_t bytes, long timeout_ms)
{
    uint64_t started = tilefinch_platform_monotonic_time_us();
    uint64_t limit = timeout_ms > 0
        ? (uint64_t) timeout_ms * UINT64_C(1000) : UINT64_C(15000000);
    size_t turns = 0;
    while (fetch_scheduler_enqueue_would_block(scheduler, bytes)
           && tilefinch_platform_monotonic_time_us() - started < limit) {
        (void) fetch_scheduler_pump(scheduler, 1, 10);
        if (!tilefinch_platform_cooperate("fetch", ++turns)) break;
    }
    if (turns != 0)
        fetch_sync_wait_note(tilefinch_platform_monotonic_time_us() - started);
}

static size_t module_prefetch_total;
static size_t module_prefetch_hit_total;

void script_loader_module_prefetch_totals(size_t *prefetches, size_t *hits)
{
    if (prefetches != NULL) *prefetches = module_prefetch_total;
    if (hits != NULL) *hits = module_prefetch_hit_total;
}

static void module_prefetch_remove(ModuleLoadContext *context, size_t index)
{
    if (index >= context->prefetch_count) return;
    memmove(&context->prefetches[index], &context->prefetches[index + 1],
            (context->prefetch_count - index - 1u)
                * sizeof(context->prefetches[0]));
    context->prefetch_count--;
}

static size_t module_prefetch_find(const ModuleLoadContext *context,
                                   const char *url)
{
    for (size_t i = 0; i < context->prefetch_count; i++)
        if (strcmp(context->prefetches[i].url, url) == 0) return i;
    return SIZE_MAX;
}

/* Release prefetches nobody will consume: the module was loaded some other
   way, or the request has sat unclaimed for a long time. */
static void module_prefetch_sweep(ModuleLoadContext *context,
                                  FetchScheduler *scheduler)
{
    uint64_t now = tilefinch_platform_monotonic_time_us();
    for (size_t i = context->prefetch_count; i != 0; i--) {
        ModulePrefetch *prefetch = &context->prefetches[i - 1u];
        if (!script_runtime_module_loaded(context->runtime, prefetch->url)
            && now - prefetch->started_us < MODULE_PREFETCH_STALE_US)
            continue;
        (void) fetch_scheduler_cancel(scheduler, prefetch->id,
                                      "module prefetch unused");
        (void) fetch_scheduler_discard(scheduler, prefetch->id);
        module_prefetch_remove(context, i - 1u);
    }
}

static bool module_specifier_prefetchable(const char *specifier,
                                          size_t length)
{
    return length > 2u && length < 512u
        && (specifier[0] == '/' || strncmp(specifier, "./", 2) == 0
            || strncmp(specifier, "../", 3) == 0);
}

/* Static `import ... from "x"`, `import "x"` and `export ... from "x"`
   specifiers. A match inside a string or comment only costs one bounded
   speculative request. Template literals (dynamic import()) are skipped. */
static void module_prefetch_dependencies(
    ModuleLoadContext *context, FetchScheduler *scheduler,
    const char *module_url, const char *referrer_policy,
    const char *document_url, TilefinchCredentialsMode credentials,
    bool initiator_opaque, const char *initiator_origin,
    const char *source, size_t length)
{
    if (context == NULL || scheduler == NULL || module_url == NULL
        || source == NULL) return;
    module_prefetch_sweep(context, scheduler);
    for (size_t at = 0; at + 6u < length
         && context->prefetch_count < MODULE_PREFETCH_LIMIT; at++) {
        size_t keyword = 0;
        if (memcmp(source + at, "from", 4) == 0) keyword = 4;
        else if (memcmp(source + at, "import", 6) == 0) keyword = 6;
        else continue;
        if (at != 0 && (isalnum((unsigned char) source[at - 1])
                        || source[at - 1] == '_' || source[at - 1] == '$'
                        || source[at - 1] == '.'))
            continue;
        size_t quote_at = at + keyword;
        while (quote_at < length && source[quote_at] == ' ') quote_at++;
        if (quote_at >= length
            || (source[quote_at] != '"' && source[quote_at] != '\'')) continue;
        char quote = source[quote_at];
        size_t end = quote_at + 1u;
        while (end < length && source[end] != quote && source[end] != '\n'
               && end - quote_at < 512u) end++;
        if (end >= length || source[end] != quote) continue;
        const char *specifier = source + quote_at + 1u;
        size_t specifier_length = end - quote_at - 1u;
        at = end;
        if (!module_specifier_prefetchable(specifier, specifier_length))
            continue;
        char relative[512];
        memcpy(relative, specifier, specifier_length);
        relative[specifier_length] = '\0';
        char resolved[NAVIGATION_URL_LIMIT];
        if (!fetch_resolve_url(module_url, relative, resolved,
                               sizeof(resolved))
            || script_runtime_module_loaded(context->runtime, resolved)
            || module_prefetch_find(context, resolved) != SIZE_MAX)
            continue;
        const BrowserCacheEntry *cached = NULL;
        if (script_module_cache_match(
                context->session, resolved, initiator_origin,
                context->top_level_url, initiator_opaque, credentials,
                &cached) == BROWSER_CACHE_FRESH) continue;
        if (fetch_scheduler_enqueue_would_block(
                scheduler, MODULE_PREFETCH_BYTES)) return;
        TilefinchRequestContext request_context = script_request_context(
            document_url, context->top_level_url, resolved, true,
            credentials, initiator_opaque);
        request_context.csp_grant = script_runtime_module_csp_grant(
            context->runtime, module_url);
        FetchRequest transport = {
            .allow_http_errors = true,
            .send_low_client_hints = true,
            .accept = "*/*",
            .stall_abort_seconds = MODULE_FETCH_STALL_SECONDS,
        };
        FetchPreparedPageRequest prepared;
        if (!fetch_prepare_page_request_context(
                &request_context, module_url, referrer_policy,
                context->session,
                script_runtime_content_security_policy(context->runtime),
                NULL, &transport, &prepared, NULL)) continue;
        uint64_t id = fetch_scheduler_enqueue(
            scheduler, resolved, fetch_prepared_page_request(&prepared),
            MODULE_PREFETCH_BYTES, context->timeout_ms);
        if (id == 0) return;
        ModulePrefetch *prefetch =
            &context->prefetches[context->prefetch_count++];
        prefetch->id = id;
        prefetch->started_us = tilefinch_platform_monotonic_time_us();
        snprintf(prefetch->url, sizeof(prefetch->url), "%s", resolved);
        context->metrics->module_prefetches++;
        module_prefetch_total++;
    }
}

static bool pipeline_module_load(void *opaque,
                                 const ScriptModuleLoadRequest *module_request,
                                 ScriptModuleLoadResult *result)
{
    ModuleLoadContext *context = opaque;
    if (context == NULL || module_request == NULL
        || module_request->request_url == NULL
        || module_request->referrer_url == NULL
        || module_request->referrer_policy == NULL
        || result == NULL) return false;
    memset(result, 0, sizeof(*result));
    const char *url = module_request->request_url;
    TilefinchCredentialsMode credentials = module_request->credentials;
    if (credentials != TILEFINCH_CREDENTIALS_SAME_ORIGIN
        && credentials != TILEFINCH_CREDENTIALS_INCLUDE) return false;
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT] = {0};
    char current_document_url[NAVIGATION_URL_LIMIT];
    const char *document_url = context->document_url;
    /* A retained module graph can outlive and be rebound away from the
       document which installed this loader.  Source-module referrers remain in
       module_request, while request credentials, CORS and Fetch Metadata must
       use the realm's live top-level document URL. */
    if (script_runtime_copy_document_url(
            context->runtime, current_document_url,
            sizeof(current_document_url))) {
        document_url = current_document_url;
    }
    char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    bool initiator_opaque = script_runtime_origin_is_opaque(context->runtime);
    if (initiator_opaque) {
        memcpy(initiator_origin, "null", sizeof("null"));
    } else if (!tilefinch_url_origin(document_url, initiator_origin,
                                     sizeof(initiator_origin))) return false;
    /* A fresh cache hit never reaches the transport's CSP check. An import
       carries its root script's nonce and parser metadata. */
    if (!tilefinch_csp_allows_request_granted(
            script_runtime_content_security_policy(context->runtime),
            TILEFINCH_DESTINATION_SCRIPT, url, module_request->csp_grant)) {
        context->metrics->policy_refusals++;
        module_load_refused(url, "content-security-policy");
        return false;
    }
    FetchScheduler *scheduler = script_runtime_fetch_scheduler(
        context->runtime);
    if (scheduler == NULL) return false;
    ScriptQuotaReservation quota = {0};
    bool progress_failed = false;
    ScriptQuotaReserveResult quota_status = script_quota_reserve_bounded(
        context->runtime, SCRIPT_QUOTA_NEW_EXECUTABLE,
        context->maximum_file_bytes, context->timeout_ms,
        &quota, &progress_failed);
    if (progress_failed) {
        module_load_refused(url, "quota-progress");
        return false;
    }
    if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
        || quota.reserved_bytes == 0) {
        script_runtime_script_quota_abort(context->runtime, &quota);
        context->metrics->skipped_quota++; script_trace_skip(url);
        module_load_refused(url, "quota-reserve");
        return false;
    }
    size_t response_limit = quota.reserved_bytes;
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus cache_status = script_module_cache_match(
        context->session, url, initiator_origin,
        context->top_level_url, initiator_opaque,
        credentials, &cached);
    if (cached != NULL
        && !script_module_mime_type_allowed(cached->content_type)) {
        cached = NULL;
        cache_status = BROWSER_CACHE_MISS;
    }
    if (cache_status == BROWSER_CACHE_FRESH
        && (cached == NULL || cached->length > response_limit)) {
        script_runtime_script_quota_abort(context->runtime, &quota);
        context->metrics->skipped_quota++; script_trace_skip(url);
        return false;
    }
    bool pressure_capped = false;
    if (cache_status != BROWSER_CACHE_FRESH
        && !script_bound_network_working_set(
                   context->runtime, context->budget, &response_limit,
                   &pressure_capped, context->metrics)) {
        script_runtime_script_quota_abort(context->runtime, &quota);
        module_load_refused(url, "network-working-set");
        return false;
    }
    context->metrics->attempted++; script_trace_attempt(url);
    const unsigned char *selected = NULL;
    const char *refusal = NULL;
    size_t selected_length = 0;
    BrowserSharedBody *selected_body = NULL;
    ScriptCacheSource cached_source = {0};
    bool cached_source_ready = cached != NULL
        && script_cache_source_acquire(
            context->budget, cached, &cached_source);
    FetchResult *fetch = NULL;
    if (cache_status == BROWSER_CACHE_FRESH) {
        if (cached_source_ready) {
            selected = (const unsigned char *) cached_source.data;
            selected_length = cached_source.length;
            selected_body = cached_source.body;
            snprintf(response_referrer_policy,
                     sizeof(response_referrer_policy), "%s",
                     cached->module_response_referrer_policy);
        }
        context->metrics->cache_hits++;
    } else {
        fetch = fetch_result_create(context->budget);
        if (fetch == NULL) {
            script_runtime_script_quota_abort(context->runtime, &quota);
            script_cache_source_release(&cached_source);
            return false;
        }
        TilefinchRequestContext request_context = script_request_context(
            document_url, context->top_level_url, url, true,
            credentials, initiator_opaque);
        request_context.csp_grant = module_request->csp_grant;
        FetchRequest transport = {
            .allow_http_errors = true,
            .send_low_client_hints = true,
            .accept = "*/*",
            .stall_abort_seconds = MODULE_FETCH_STALL_SECONDS,
            .if_none_match = cached == NULL ? NULL : cached->etag,
            .if_modified_since = cached == NULL
                                 ? NULL : cached->last_modified,
            .cors_cached_response_validated = cached != NULL
                && !cached->module_cors_redirect_origin_tainted
                && (cached->etag[0] != '\0'
                    || cached->last_modified[0] != '\0'),
        };
        FetchPreparedPageRequest prepared;
        bool request_ready = fetch_prepare_page_request_context(
            &request_context, module_request->referrer_url,
            module_request->referrer_policy, context->session,
            script_runtime_content_security_policy(context->runtime),
            NULL, &transport,
            &prepared, NULL);
        const FetchRequest *request = request_ready
            ? fetch_prepared_page_request(&prepared) : NULL;
        size_t prefetched = module_prefetch_find(context, url);
        bool fetched = false;
        /* A failure that delivered no body is usually a dead or stalled
           connection (every HTTP/2 stream on it stalls), so the next
           attempt opens a new one. */
        FetchRequest fresh_request = {0};
        bool use_fresh_connection = false;
        if (prefetched != SIZE_MAX) {
            uint64_t prefetch_id = context->prefetches[prefetched].id;
            module_prefetch_remove(context, prefetched);
            fetched = fetch_scheduler_wait(scheduler, prefetch_id, fetch)
                && fetch->status_code >= 200 && fetch->status_code < 300;
            if (fetched) {
                context->metrics->module_prefetch_hits++;
                module_prefetch_hit_total++;
            } else {
                use_fresh_connection = fetch->length == 0;
                fetch_result_free(fetch);
                fetch = fetch_result_create(context->budget);
                if (fetch == NULL) {
                    script_runtime_script_quota_abort(
                        context->runtime, &quota);
                    script_cache_source_release(&cached_source);
                    return false;
                }
            }
        }
        if (request != NULL) {
            fresh_request = *request;
            fresh_request.force_fresh_connection = true;
        }
        for (int attempt = 0; !fetched && request != NULL && attempt < 2;
             attempt++) {
            script_wait_for_fetch_capacity(
                scheduler, response_limit, context->timeout_ms);
            fetched = fetch_scheduler_request(
                scheduler, url,
                use_fresh_connection ? &fresh_request : request,
                response_limit, context->timeout_ms, fetch);
            /* A transport failure that delivered no body — no response at
               all, or headers and then a stall — is retried once on a new
               connection; a stream stuck behind others on one HTTP/2
               connection stays stuck. */
            if (fetched || fetch->length != 0 || use_fresh_connection)
                break;
            use_fresh_connection = true;
            fetch_result_free(fetch);
            fetch = fetch_result_create(context->budget);
            if (fetch == NULL) {
                script_runtime_script_quota_abort(context->runtime, &quota);
                script_cache_source_release(&cached_source);
                return false;
            }
        }
        if (!fetched) {
            if (tilefinch_trace_script_failures()) {
                size_t pool_reserved = 0, pool_maximum = 0;
                size_t slots_active = 0, slots_maximum = 0;
                fetch_scheduler_reservation_state(
                    scheduler, &pool_reserved, &pool_maximum,
                    &slots_active, &slots_maximum);
                fprintf(stderr,
                        "module-fetch-failure url=\"%s\" limit=%zu "
                        "pressure-capped=%s error=\"%s\" "
                        "pool=%zu/%zu slots=%zu/%zu\n",
                        url, response_limit,
                        pressure_capped ? "yes" : "no", fetch->error,
                        pool_reserved, pool_maximum,
                        slots_active, slots_maximum);
                fetch_scheduler_debug_dump(scheduler, "module-fetch");
            }
            if (script_fetch_was_pressure_rejected(fetch, pressure_capped)) {
                context->metrics->skipped_pressure++;
            }
            {
                size_t pool_reserved = 0, pool_maximum = 0;
                size_t slots_active = 0, slots_maximum = 0;
                fetch_scheduler_reservation_state(
                    scheduler, &pool_reserved, &pool_maximum,
                    &slots_active, &slots_maximum);
                char reason[200];
                snprintf(reason, sizeof(reason),
                         "fetch request=%d limit=%zu pool=%zu/%zu "
                         "slots=%zu/%zu error=\"%.60s\" scheduler=\"%.60s\"",
                         request != NULL, response_limit, pool_reserved,
                         pool_maximum, slots_active, slots_maximum,
                         fetch->error, fetch_scheduler_last_error(scheduler));
                module_load_refused(url, reason);
            }
            script_runtime_script_quota_abort(context->runtime, &quota);
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            return false;
        }
        const char *fetch_response_url = fetch->effective_url[0] == '\0'
            ? (fetch->status_code == 304 && cached != NULL
                ? cached->module_effective_url : url)
            : fetch->effective_url;
        bool response_ok = (fetch->status_code >= 200
                            && fetch->status_code < 300)
            || (fetch->status_code == 304 && cached_source_ready);
        if (!response_ok) refusal = "status";
        const char *accepted_content_type = fetch->status_code == 304
            && cached != NULL ? cached->content_type : fetch->content_type;
        if (response_ok) {
            response_ok = fetch->status_code == 304
                ? script_module_revalidated_mime_allowed(
                    accepted_content_type, fetch->content_type)
                : script_module_mime_type_allowed(accepted_content_type);
            if (!response_ok) refusal = "mime";
        }
        TilefinchResourceGrant resource_grant = {0};
        if (response_ok) {
            if (fetch->effective_url[0] == '\0') {
                snprintf(fetch->effective_url, sizeof(fetch->effective_url),
                         "%s", fetch_response_url);
            }
            response_ok = script_resource_grant(
                fetch, &request_context, accepted_content_type, true,
                &resource_grant);
            if (!response_ok) refusal = "resource-grant";
        }
        if (response_ok && fetch->status_code == 304) {
            response_ok = script_module_cache_record(
                context->session, url, fetch_response_url,
                initiator_origin, context->top_level_url,
                initiator_opaque, credentials, fetch,
                true, tilefinch_platform_monotonic_time_ns());
        }
        if (response_ok && fetch->status_code == 304 && cached != NULL) {
            snprintf(response_referrer_policy,
                     sizeof(response_referrer_policy), "%s",
                     cached->module_response_referrer_policy);
        } else if (response_ok) {
            bool header_present = false;
            response_ok = fetch_response_referrer_policy(
                fetch, &header_present, response_referrer_policy);
            if (!response_ok) refusal = "referrer-policy";
        }
        if (response_ok) {
            script_accept_response_cookies(
                context->session, &request_context, fetch,
                fetch_response_url);
        }
        if (response_ok && fetch->status_code == 304 && cached != NULL) {
            if (cached_source_ready) {
                selected = (const unsigned char *) cached_source.data;
                selected_length = cached_source.length;
                selected_body = cached_source.body;
                context->metrics->cache_hits++;
            }
        } else if (response_ok) {
            selected_length = fetch->length;
            (void) fetch_result_share_body(fetch);
            if (context->session != NULL && fetch->length != 0) {
                (void) script_module_cache_record(
                    context->session, url, fetch_response_url,
                    initiator_origin, context->top_level_url,
                    initiator_opaque, credentials, fetch,
                    false, tilefinch_platform_monotonic_time_ns());
            }
            /* Both sharing attempts can relocate the response, including a
               failed descriptor allocation after a successful shrink. */
            selected = (const unsigned char *) fetch->data;
            selected_body = fetch->shared_body != NULL
                && script_source_has_terminator(
                    fetch->shared_body->data, fetch->shared_body->length)
                ? fetch->shared_body : NULL;
        }
    }
    if (selected == NULL || selected_length == 0
        || selected_length > response_limit) {
        module_load_refused(url, selected == NULL
                                     ? (refusal == NULL ? "response" : refusal)
                            : selected_length == 0 ? "empty" : "too-large");
        script_runtime_script_quota_abort(context->runtime, &quota);
        if (selected_length > response_limit) {
            context->metrics->skipped_quota++; script_trace_skip(url);
        }
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    {
        const char *module_url = fetch != NULL
            && fetch->effective_url[0] != '\0'
            ? fetch->effective_url
            : (cached != NULL && cached->module_effective_url != NULL
                   ? cached->module_effective_url : url);
        module_prefetch_dependencies(
            context, scheduler, module_url, response_referrer_policy,
            document_url, credentials, initiator_opaque, initiator_origin,
            (const char *) selected, selected_length);
    }
    /* A fresh cache hit is never no-store: the HTTP cache does not keep
       such a response. */
    result->response_no_store = fetch != NULL
        && script_response_no_store(fetch);
    size_t quota_source_length = selected_length;
    if (!script_runtime_script_quota_commit(
            context->runtime, &quota, quota_source_length)) {
        script_runtime_script_quota_abort(context->runtime, &quota);
        context->metrics->skipped_quota++; script_trace_skip(url);
        module_load_refused(url, "quota-commit");
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    if (!script_admit_known_working_set(
            context->runtime, context->budget, quota_source_length,
            context->metrics)) {
        {
            char reason[160];
            snprintf(reason, sizeof(reason),
                     "working-set bytes=%zu budget-remaining=%zu "
                     "heap-remaining=%zu collections=%zu",
                     quota_source_length, budget_remaining(context->budget),
                     script_runtime_heap_remaining(context->runtime),
                     context->metrics->pressure_collections);
            module_load_refused(url, reason);
        }
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    const char *effective_url = cached != NULL
        && cached->module_effective_url != NULL
            ? cached->module_effective_url : url;
    if (fetch != NULL && fetch->effective_url[0] != '\0') {
        effective_url = fetch->effective_url;
    }
    size_t effective_length = strlen(effective_url);
    char *response_url = budget_malloc(
        context->budget, effective_length + 1);
    if (response_url == NULL) {
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    memcpy(response_url, effective_url, effective_length + 1);
    unsigned diagnostics = 0;
    size_t diagnostic_extra =
        script_bundle_diagnostics_select(url, &diagnostics);
    bool diagnostic_copy = diagnostics != 0;
    if (!diagnostic_copy && selected_body != NULL) {
        char *leased_source = NULL;
        if (pipeline_module_lease_body(
                context, selected_body, &leased_source)) {
            fetch_result_free(fetch);
            script_cache_source_release(&cached_source);
            result->source = leased_source;
            result->source_length = selected_length;
            result->response_url = response_url;
            snprintf(result->response_referrer_policy,
                     sizeof(result->response_referrer_policy), "%s",
                     response_referrer_policy);
            context->metrics->loaded++;
            context->metrics->modules++;
            context->metrics->bytes += selected_length;
            return true;
        }
    }
    if (selected_length == SIZE_MAX
        || diagnostic_extra > SIZE_MAX - selected_length - 1) {
        budget_free(context->budget, response_url);
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    char *copy = budget_malloc(context->budget,
                               selected_length + diagnostic_extra + 1);
    if (copy == NULL) {
        budget_free(context->budget, response_url);
        fetch_result_free(fetch);
        script_cache_source_release(&cached_source);
        return false;
    }
    memcpy(copy, selected, selected_length); copy[selected_length] = '\0';
    script_bundle_diagnostics_apply(diagnostics, copy, &selected_length);
    fetch_result_free(fetch);
    script_cache_source_release(&cached_source);
    result->source = copy;
    result->source_length = selected_length;
    result->response_url = response_url;
    snprintf(result->response_referrer_policy,
             sizeof(result->response_referrer_policy), "%s",
             response_referrer_policy);
    context->metrics->loaded++;
    context->metrics->modules++;
    context->metrics->bytes += selected_length;
    return true;
}

static void pipeline_module_free(void *opaque, ScriptModuleLoadResult *result)
{
    ModuleLoadContext *context = opaque;
    if (context == NULL || result == NULL) return;
    char *source = result->source;
    budget_free(context->budget, result->response_url);
    result->response_url = NULL;
    ModuleBodyLease **at = &context->body_leases;
    while (*at != NULL) {
        ModuleBodyLease *lease = *at;
        if (lease->source == source) {
            *at = lease->next;
            browser_shared_body_release(lease->body);
            budget_free(context->budget, lease);
            memset(result, 0, sizeof(*result));
            return;
        }
        at = &lease->next;
    }
    budget_free(context->budget, source);
    memset(result, 0, sizeof(*result));
}

static bool script_was_parser_executed(
    const StreamingScriptState *streaming, long handle)
{
    if (streaming == NULL) return false;
    for (size_t i = 0; i < streaming->parser_executed_count; i++) {
        if (streaming->parser_executed[i] == handle) return true;
    }
    return false;
}

static bool script_node_is_attached(const lxb_dom_node_t *node)
{
    /* A parser-retained section is intentionally rooted outside the active
       document while it is materialized.  Destructive DOM replacement,
       however, detaches the stale script itself. */
    return node != NULL && node->parent != NULL;
}

typedef enum {
    SCRIPT_ADMISSION_PENDING = 0,
    SCRIPT_ADMISSION_COUNTED,
    SCRIPT_ADMISSION_INLINE_DATA,
    SCRIPT_ADMISSION_MODULE_ALIAS,
    SCRIPT_ADMISSION_DENIED
} ScriptAdmission;

#define SCRIPT_MODULE_ALIAS_TABLE_CAPACITY \
    (EXTERNAL_SCRIPT_HARD_LIMIT * 2u)

typedef struct {
    long scripts[EXTERNAL_SCRIPT_HARD_LIMIT];
    long deferred[EXTERNAL_SCRIPT_HARD_LIMIT];
    uint32_t module_hashes[EXTERNAL_SCRIPT_HARD_LIMIT];
    uint16_t module_slots[SCRIPT_MODULE_ALIAS_TABLE_CAPACITY];
    uint16_t handle_slots[SCRIPT_MODULE_ALIAS_TABLE_CAPACITY];
    uint16_t module_alias_of[EXTERNAL_SCRIPT_HARD_LIMIT];
    uint8_t admission[EXTERNAL_SCRIPT_HARD_LIMIT];
    /* First half: module, second half: quota was already claimed. */
    uint8_t deferred_flags[EXTERNAL_SCRIPT_HARD_LIMIT / 4u];
} ScriptExecutionPlan;

static size_t script_plan_handle_slot(long handle)
{
    uintptr_t value = (uintptr_t) handle;
    value ^= value >> 16;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 32;
#endif
    value *= (uintptr_t) UINT32_C(2654435761);
    return (size_t) value & (SCRIPT_MODULE_ALIAS_TABLE_CAPACITY - 1u);
}

static bool script_plan_record_handle(
    ScriptExecutionPlan *plan, long handle, size_t index)
{
    size_t slot = script_plan_handle_slot(handle);
    for (size_t probes = 0;
         probes < SCRIPT_MODULE_ALIAS_TABLE_CAPACITY; probes++) {
        uint16_t existing_plus_one = plan->handle_slots[slot];
        if (existing_plus_one == 0) {
            plan->handle_slots[slot] = (uint16_t) index + 1u;
            return true;
        }
        size_t existing = (size_t) existing_plus_one - 1u;
        if (plan->scripts[existing] == handle) return false;
        slot = (slot + 1u) & (SCRIPT_MODULE_ALIAS_TABLE_CAPACITY - 1u);
    }
    return false;
}

static bool script_execution_plan_finish(
    ModuleLoadContext *context, ScriptExecutionPlan *plan,
    ExternalScriptMetrics *output, bool result)
{
    if (context != NULL) budget_free(context->budget, plan);
    return pipeline_context_finish_pass(context, output, result);
}

static bool script_plan_flag(
    const ScriptExecutionPlan *plan, size_t index, bool precounted)
{
    size_t bit = index + (precounted ? EXTERNAL_SCRIPT_HARD_LIMIT : 0u);
    return plan != NULL
        && (plan->deferred_flags[bit / 8u]
            & (uint8_t) (1u << (bit & 7u))) != 0;
}

static void script_plan_set_flag(
    ScriptExecutionPlan *plan, size_t index, bool precounted, bool value)
{
    size_t bit = index + (precounted ? EXTERNAL_SCRIPT_HARD_LIMIT : 0u);
    uint8_t mask = (uint8_t) (1u << (bit & 7u));
    if (value) plan->deferred_flags[bit / 8u] |= mask;
    else plan->deferred_flags[bit / 8u] &= (uint8_t) ~mask;
}

static uint32_t script_module_reference_hash(
    const char *source, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) source[i];
        hash *= UINT32_C(16777619);
    }
    return hash == 0 ? 1u : hash;
}

static bool script_attribute_token_equals(
    lxb_dom_node_t *node, const char *name, const char *expected)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t expected_length = strlen(expected);
    return value != NULL && length == expected_length
        && strncasecmp(value, expected, length) == 0;
}

static bool script_is_critical_bootstrap(lxb_dom_node_t *node)
{
    if (script_kind(node) == SCRIPT_KIND_MODULE) return true;
    /* `defer` defines ordering, not importance; production pages commonly
       apply it to consent, analytics, and other optional clients. Authors can
       opt a classic root into the reserve with the standard priority hint. */
    return script_attribute_token_equals(node, "fetchpriority", "high");
}

static bool script_external_module_reference_equals(
    lxb_dom_node_t *left, lxb_dom_node_t *right)
{
    if (script_kind(left) != SCRIPT_KIND_MODULE
        || script_kind(right) != SCRIPT_KIND_MODULE) return false;
    size_t left_length = 0, right_length = 0;
    const char *left_source = script_source_attribute(left, &left_length);
    const char *right_source = script_source_attribute(right, &right_length);
    return left_source != NULL && right_source != NULL && left_length != 0
        && left_length == right_length
        && memcmp(left_source, right_source, left_length) == 0;
}

static void script_plan_initial_admission(
    ScriptRuntime *runtime, const StreamingScriptState *streaming,
    ScriptExecutionPlan *plan, size_t script_count, size_t maximum_scripts,
    ExternalScriptMetrics *metrics)
{
    for (size_t i = 0; i < EXTERNAL_SCRIPT_HARD_LIMIT; i++) {
        plan->module_alias_of[i] = UINT16_MAX;
    }
    /* Strict assignment-only JSON is installed without compilation. Mark it
       before reserving executable slots so server-rendered data carriers do
       not displace later, small logic scripts. The live execution check is
       repeated because an earlier author script may mutate a later node. */
    for (size_t i = 0; i < script_count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->scripts[i]);
        if (!script_node_is_attached(node)
            || script_runtime_dynamic_script_is_scheduled(runtime, node)
            || script_was_parser_executed(streaming, plan->scripts[i])
            || script_kind(node) != SCRIPT_KIND_CLASSIC) continue;
        size_t source_length = 0;
        const char *source = script_source_attribute(node, &source_length);
        if ((source == NULL || source_length == 0)
            && script_runtime_inline_data_candidate(node, NULL)) {
            plan->admission[i] = SCRIPT_ADMISSION_INLINE_DATA;
        }
    }
    /* Parser markup often repeats the same component module once per
       instance. Those elements share one module-map entry and therefore one
       executable quota slot. Mark exact parser references as aliases before
       quota planning; the live resolved-URL check below remains authoritative
       if an earlier script changes <base>. */
    for (size_t i = 0; i < script_count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->scripts[i]);
        if (!script_node_is_attached(node)
            || script_runtime_dynamic_script_is_scheduled(runtime, node)
            || script_was_parser_executed(streaming, plan->scripts[i])
            || script_kind(node) != SCRIPT_KIND_MODULE) continue;
        size_t source_length = 0;
        const char *source = script_source_attribute(node, &source_length);
        if (source == NULL || source_length == 0) continue;
        uint32_t hash = script_module_reference_hash(source, source_length);
        plan->module_hashes[i] = hash;
        size_t slot = hash & (SCRIPT_MODULE_ALIAS_TABLE_CAPACITY - 1u);
        for (size_t probes = 0;
             probes < SCRIPT_MODULE_ALIAS_TABLE_CAPACITY; probes++) {
            uint16_t canonical_plus_one = plan->module_slots[slot];
            if (canonical_plus_one == 0) {
                plan->module_slots[slot] = (uint16_t) i + 1u;
                break;
            }
            size_t canonical = (size_t) canonical_plus_one - 1u;
            if (plan->module_hashes[canonical] == hash) {
                lxb_dom_node_t *prior =
                    script_runtime_node_handle_resolve(
                        runtime, plan->scripts[canonical]);
                if (script_external_module_reference_equals(prior, node)) {
                    plan->admission[i] =
                        SCRIPT_ADMISSION_MODULE_ALIAS;
                    plan->module_alias_of[i] = (uint16_t) canonical;
                    break;
                }
            }
            slot = (slot + 1u)
                   & (SCRIPT_MODULE_ALIAS_TABLE_CAPACITY - 1u);
        }
    }

    /* The historical envelope is the first N executable roots, not the first
       N script elements: inert/data-only elements consume no browser work in
       a conforming implementation. Find that bounded cutoff without changing
       source-order execution. */
    size_t prefix_limit = script_count;
    size_t executable_roots = 0;
    for (size_t i = 0; i < script_count; i++) {
        if (plan->admission[i] != SCRIPT_ADMISSION_PENDING) continue;
        if (executable_roots == maximum_scripts) {
            prefix_limit = i;
            break;
        }
        executable_roots++;
    }
    size_t critical_claims = 0;
    for (size_t i = prefix_limit;
         i < script_count
         && critical_claims < SCRIPT_CRITICAL_BOOTSTRAP_RESERVE; i++) {
        if (plan->admission[i] != SCRIPT_ADMISSION_PENDING) continue;
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->scripts[i]);
        if (!script_node_is_attached(node)
            || script_runtime_dynamic_script_is_scheduled(runtime, node)
            || script_was_parser_executed(streaming, plan->scripts[i])
            || !script_is_critical_bootstrap(node)) continue;
        if (script_quota_claim_executable(runtime)) {
            plan->admission[i] = SCRIPT_ADMISSION_COUNTED;
            critical_claims++;
        } else {
            plan->admission[i] = SCRIPT_ADMISSION_DENIED;
            metrics->skipped_quota++;
        }
    }
    /* Preserve the established source-prefix work envelope. Deduplicating a
       module root must not silently authorize enough additional unique roots
       to fill the realm again; only the bounded critical reserve above may
       reach beyond this prefix. Admission priority does not alter execution
       order. */
    for (size_t i = 0; i < prefix_limit; i++) {
        if (plan->admission[i] != SCRIPT_ADMISSION_PENDING) continue;
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->scripts[i]);
        if (!script_node_is_attached(node)
            || script_runtime_dynamic_script_is_scheduled(runtime, node)
            || script_was_parser_executed(streaming, plan->scripts[i])) continue;
        if (script_quota_claim_executable(runtime)) {
            plan->admission[i] = SCRIPT_ADMISSION_COUNTED;
        } else {
            plan->admission[i] = SCRIPT_ADMISSION_DENIED;
            metrics->skipped_quota++;
        }
    }
    for (size_t i = prefix_limit; i < script_count; i++) {
        if (plan->admission[i] == SCRIPT_ADMISSION_PENDING) {
            plan->admission[i] = SCRIPT_ADMISSION_DENIED;
            metrics->skipped_quota++;
        }
    }
    /* An alias inherits only an admitted canonical root. If the first exact
       reference was outside both envelopes, later repetitions cannot use the
       unused module-map path to consume the remaining realm quota one by one.
       A live <base> mutation is handled separately at execution time. */
    for (size_t i = 0; i < script_count; i++) {
        if (plan->admission[i] != SCRIPT_ADMISSION_MODULE_ALIAS) continue;
        uint16_t canonical = plan->module_alias_of[i];
        if (canonical == UINT16_MAX || (size_t) canonical >= i
            || plan->admission[(size_t) canonical]
                   != SCRIPT_ADMISSION_COUNTED) {
            plan->admission[i] = SCRIPT_ADMISSION_DENIED;
            metrics->skipped_quota++;
        }
    }
}

/* One loader context per realm. Parser-blocking classic scripts may call
   import() before the document pipeline starts (chatgpt.com's inline
   bootstrap does), so the first of the two to need it installs the context
   and the other adopts it: replacing it would release module bodies that
   earlier compiled modules still lease. */
static ModuleLoadContext *pipeline_module_loader_install(
    ScriptRuntime *runtime, Budget *budget, BrowserSession *session,
    const char *document_url, size_t maximum_file_bytes, long timeout_ms)
{
    ModuleLoadContext *module_context = script_runtime_module_loader_opaque(
        runtime, pipeline_module_load);
    bool installed = module_context != NULL;
    if (!installed) {
        module_context = budget_calloc(budget, 1, sizeof(*module_context));
        if (module_context == NULL) return NULL;
        module_context->runtime = runtime;
        module_context->budget = budget;
        module_context->metrics = &module_context->retained_metrics;
    }
    module_context->session = session;
    /* The loader owns no page scheduler. Its realm creates a bounded view only
       when a dependency is actually requested, so idle documents and child
       realms do not each allocate a CURLM/view. The runtime retains the shared
       scheduler domain across low-memory document replacement. */
    snprintf(module_context->document_url,
             sizeof(module_context->document_url), "%s", document_url);
    if (!script_runtime_copy_top_level_url(
            runtime, module_context->top_level_url,
            sizeof(module_context->top_level_url))) {
        if (!installed) budget_free(budget, module_context);
        return NULL;
    }
    module_context->maximum_file_bytes = maximum_file_bytes;
    module_context->timeout_ms = timeout_ms;
    if (!installed) {
        script_runtime_set_module_loader_owned(
            runtime, pipeline_module_load, pipeline_module_free,
            module_context, pipeline_context_destroy);
    }
    return module_context;
}

static bool document_scripts_execute_internal(
    PocDocument *document, ScriptRuntime *runtime, Budget *budget,
    BrowserSession *session, const char *base_url,
    const char *document_url,
    const char *referrer_policy, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes, long timeout_ms,
    FetchScheduler *scheduler, const StreamingScriptState *streaming,
    lxb_dom_node_t *script_root, ExternalScriptMetrics *metrics,
    ScriptResult *result)
{
    if (document == NULL || runtime == NULL || budget == NULL
        || base_url == NULL || document_url == NULL || metrics == NULL
        || maximum_file_bytes == 0 || scheduler == NULL
        || script_root == NULL) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr,
                    "document-script-pipeline-invalid-input document=%s "
                    "runtime=%s budget=%s base=%s url=%s metrics=%s "
                    "file-limit=%zu scheduler=%s root=%s\n",
                    document == NULL ? "null" : "set",
                    runtime == NULL ? "null" : "set",
                    budget == NULL ? "null" : "set",
                    base_url == NULL ? "null" : "set",
                    document_url == NULL ? "null" : "set",
                    metrics == NULL ? "null" : "set", maximum_file_bytes,
                    scheduler == NULL ? "null" : "set",
                    script_root == NULL ? "null" : "set");
        }
        return false;
    }
    (void) maximum_scripts;
    (void) maximum_total_bytes;
    memset(metrics, 0, sizeof(*metrics));
    if (streaming != NULL) {
        *metrics = streaming->early;
    }
    size_t script_count = 0, deferred_count = 0;
    ScriptExecutionPlan *plan = budget_calloc(
        budget, 1, sizeof(*plan));
    if (plan == NULL) return false;
    ModuleLoadContext *module_context = pipeline_module_loader_install(
        runtime, budget, session, document_url, maximum_file_bytes,
        timeout_ms);
    if (module_context == NULL) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr, "document-script-module-context-allocation-failure\n");
        }
        budget_free(budget, plan);
        return false;
    }
    ExternalScriptMetrics *output_metrics = metrics;
    module_context->retained_metrics = *output_metrics;
    metrics = module_context->metrics;
    size_t collection_limit = EXTERNAL_SCRIPT_HARD_LIMIT;
    size_t discovered_before_scan = metrics->discovered;
    collect_executable_scripts(runtime, script_root, plan->scripts,
                               collection_limit, &script_count,
                               &metrics->discovered, streaming,
                               &metrics->skipped_nomodule);
    for (size_t i = 0; i < script_count; i++) {
        (void) script_plan_record_handle(plan, plan->scripts[i], i);
    }
    script_plan_initial_admission(
        runtime, streaming, plan, script_count, maximum_scripts, metrics);
    size_t scan_discovered = metrics->discovered - discovered_before_scan;
    if (scan_discovered > script_count) {
        metrics->skipped_quota += scan_discovered - script_count;
    }
    for (size_t i = 0; i < script_count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->scripts[i]);
        if (!script_node_is_attached(node)) continue;
        if (script_runtime_dynamic_script_is_scheduled(runtime, node)) {
            continue;
        }
        if (script_was_parser_executed(streaming, plan->scripts[i])) continue;
        if (plan->admission[i] == SCRIPT_ADMISSION_DENIED
            || plan->admission[i] == SCRIPT_ADMISSION_PENDING) continue;
        ScriptKind kind = script_kind(node);
        bool module = kind == SCRIPT_KIND_MODULE;
        if (kind == SCRIPT_KIND_INERT) continue;
        size_t source_length = 0;
        const char *source = script_source_attribute(node, &source_length);
        bool external = source != NULL && source_length != 0;
        bool asynchronous = external && attribute_present(node, "async");
        bool defer = module || (external && attribute_present(node, "defer"));
        if (module) metrics->modules++;
        if (defer && !asynchronous) {
            plan->deferred[deferred_count] = plan->scripts[i];
            script_plan_set_flag(
                plan, deferred_count, false, module);
            script_plan_set_flag(
                plan, deferred_count, true,
                plan->admission[i] == SCRIPT_ADMISSION_COUNTED);
            deferred_count++;
            metrics->deferred++;
            continue;
        }
        if (asynchronous) metrics->asynchronous++;
        else metrics->parser_blocking++;
        if (external) {
            if (!execute_external_node_live(
                    document, runtime, budget, session, scheduler, node,
                    base_url, document_url, referrer_policy, module,
                    plan->admission[i] == SCRIPT_ADMISSION_COUNTED,
                    maximum_total_bytes, maximum_file_bytes, timeout_ms,
                    metrics)) {
                return script_execution_plan_finish(
                    module_context, plan, output_metrics, false);
            }
        } else if (!script_execute_inline_admitted(
                       runtime, budget, node, module,
                       plan->admission[i] == SCRIPT_ADMISSION_COUNTED,
                       plan->admission[i] == SCRIPT_ADMISSION_INLINE_DATA,
                       &document->content_security_policy, metrics)) {
            return script_execution_plan_finish(
                module_context, plan, output_metrics, false);
        }
    }
    for (size_t i = 0; i < deferred_count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(
            runtime, plan->deferred[i]);
        if (!script_node_is_attached(node)) continue;
        if (script_runtime_dynamic_script_is_scheduled(runtime, node)) {
            continue;
        }
        size_t source_length = 0;
        const char *source = script_source_attribute(node, &source_length);
        if (source != NULL && source_length != 0) {
            if (!execute_external_node_live(
                    document, runtime, budget, session, scheduler, node,
                    base_url, document_url, referrer_policy,
                    script_plan_flag(plan, i, false),
                    script_plan_flag(plan, i, true),
                    maximum_total_bytes,
                    maximum_file_bytes, timeout_ms, metrics)) {
                return script_execution_plan_finish(
                    module_context, plan, output_metrics, false);
            }
        } else if (!script_execute_inline_admitted(
                       runtime, budget, node,
                       script_plan_flag(plan, i, false),
                       script_plan_flag(plan, i, true), false,
                       &document->content_security_policy, metrics)) {
            return script_execution_plan_finish(
                module_context, plan, output_metrics, false);
        }
    }
    /* Scripts inserted by an earlier script are not part of the parser's
       original list. Discover them in bounded waves, as browsers do when an
       appendChild() makes an external script eligible for execution. */
    for (size_t wave = 0;
         wave < 8 && script_count < collection_limit; wave++) {
        long *current = plan->deferred;
        memset(current, 0, sizeof(plan->deferred));
        size_t current_count = 0, current_discovered = 0, added = 0;
        collect_executable_scripts(runtime, script_root, current,
                                   collection_limit,
                                   &current_count, &current_discovered,
                                   streaming, NULL);
        for (size_t i = 0;
             i < current_count
             && script_count < collection_limit; i++) {
            if (!script_plan_record_handle(
                    plan, current[i], script_count)) continue;
            plan->scripts[script_count++] = current[i];
            metrics->discovered++;
            added++;
            lxb_dom_node_t *node = script_runtime_node_handle_resolve(
                runtime, current[i]);
            if (!script_node_is_attached(node)) continue;
            if (script_runtime_dynamic_script_is_scheduled(runtime, node)) {
                continue;
            }
            ScriptKind kind = script_kind(node);
            bool module = kind == SCRIPT_KIND_MODULE;
            if (kind == SCRIPT_KIND_INERT) continue;
            if (module) metrics->modules++;
            metrics->asynchronous++;
            size_t source_length = 0;
            const char *source = script_source_attribute(
                node, &source_length);
            if (source != NULL && source_length != 0) {
                if (!execute_external_node_live(
                        document, runtime, budget, session, scheduler, node,
                        base_url, document_url, referrer_policy, module,
                        false,
                        maximum_total_bytes, maximum_file_bytes, timeout_ms,
                        metrics)) {
                    return script_execution_plan_finish(
                        module_context, plan, output_metrics, false);
                }
            } else if (!script_execute_inline_admitted(
                           runtime, budget, node, module, false, !module,
                           &document->content_security_policy, metrics)) {
                return script_execution_plan_finish(
                    module_context, plan, output_metrics, false);
            }
        }
        if (added == 0) break;
    }
    bool finished = script_runtime_finish_loading(runtime, result);
    if (!finished && tilefinch_trace_script_failures()) {
        fprintf(stderr,
                "document-load-event-failure error=\"%s\" context=\"%s\"\n",
                result == NULL ? "<diagnostic unavailable>" : result->error,
                result == NULL ? "" : result->error_source_context);
        script_runtime_report_memory(runtime, stderr);
    }
    return script_execution_plan_finish(
        module_context, plan, output_metrics, finished);
}

DocumentScriptProcessResult document_scripts_process_closed(
    ScriptRuntime *runtime, Budget *budget, BrowserSession *session,
    const char *base_url, const char *document_url,
    const char *referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_scripts, size_t maximum_total_bytes,
    size_t maximum_file_bytes, long timeout_ms, FetchScheduler *scheduler,
    lxb_dom_node_t *element, StreamingScriptState *state)
{
    if (runtime == NULL || budget == NULL || base_url == NULL
        || document_url == NULL
        || scheduler == NULL || element == NULL || state == NULL
        || state->parser_executed_count >= 256
        || !document_script_is_parser_blocking(element)) {
        return DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    }
    (void) maximum_scripts;
    (void) maximum_total_bytes;
    long element_handle = script_runtime_node_weak_handle(runtime, element);
    if (element_handle != 0
        && script_was_parser_executed(state, element_handle)) {
        return DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    }
    /* A failed install leaves import() unavailable to this script only;
       the document pipeline retries it. */
    (void) pipeline_module_loader_install(
        runtime, budget, session, document_url, maximum_file_bytes,
        timeout_ms);
    state->early.discovered++;
    state->early.parser_blocking++;
    if (element_handle == 0) {
        state->early.skipped_quota++;
        return DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    }
    state->parser_executed[state->parser_executed_count++] = element_handle;
    size_t source_length = 0;
    const char *source = script_source_attribute(element, &source_length);
    bool external = source != NULL && source_length != 0;
    bool ok = true;
    DocumentScriptProcessResult process_result =
        DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    if (external) {
        ok = execute_external_node(
            runtime, budget, session, scheduler, element, base_url,
            document_url, referrer_policy, content_security_policy,
            false, false, maximum_total_bytes,
            maximum_file_bytes, timeout_ms, &state->early,
            &process_result);
    } else {
        ok = script_execute_inline_admitted(
            runtime, budget, element, false, false, true,
            content_security_policy, &state->early);
    }
    if (script_runtime_document_refresh_failed(runtime)) {
        return DOCUMENT_SCRIPT_PROCESS_HARD_FAILURE;
    }
    return ok ? process_result : DOCUMENT_SCRIPT_PROCESS_HARD_FAILURE;
}

bool document_scripts_finish_streaming(
    PocDocument *document, ScriptRuntime *runtime, Budget *budget,
    BrowserSession *session, const char *base_url,
    const char *document_url,
    const char *referrer_policy, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes, long timeout_ms,
    FetchScheduler *scheduler, StreamingScriptState *state,
    ExternalScriptMetrics *metrics, ScriptResult *result)
{
    return document_scripts_execute_internal(
        document, runtime, budget, session, base_url, document_url,
        referrer_policy,
        maximum_scripts, maximum_total_bytes, maximum_file_bytes, timeout_ms,
        scheduler, state, lxb_dom_interface_node(document->html), metrics,
        result);
}

bool document_scripts_execute(PocDocument *document,
                              ScriptRuntime *runtime, Budget *budget,
                              BrowserSession *session,
                              const char *base_url,
                              const char *document_url,
                              const char *referrer_policy,
                              size_t maximum_scripts,
                              size_t maximum_total_bytes,
                              size_t maximum_file_bytes,
                              long timeout_ms,
                              FetchScheduler *scheduler,
                              ExternalScriptMetrics *metrics,
                              ScriptResult *result)
{
    return document_scripts_execute_internal(
        document, runtime, budget, session, base_url, document_url,
        referrer_policy,
        maximum_scripts, maximum_total_bytes, maximum_file_bytes, timeout_ms,
        scheduler, NULL, lxb_dom_interface_node(document->html), metrics,
        result);
}

bool document_body_scripts_execute(PocDocument *document,
                                   ScriptRuntime *runtime, Budget *budget,
                                   BrowserSession *session,
                                   const char *base_url,
                                   const char *document_url,
                                   const char *referrer_policy,
                                   size_t maximum_scripts,
                                   size_t maximum_total_bytes,
                                   size_t maximum_file_bytes,
                                   long timeout_ms,
                                   FetchScheduler *scheduler,
                                   ExternalScriptMetrics *metrics,
                                   ScriptResult *result)
{
    lxb_dom_node_t *body = document == NULL ? NULL
                                           : document_body_node(document);
    return document_scripts_execute_internal(
        document, runtime, budget, session, base_url, document_url,
        referrer_policy, maximum_scripts, maximum_total_bytes,
        maximum_file_bytes, timeout_ms, scheduler, NULL, body, metrics,
        result);
}
