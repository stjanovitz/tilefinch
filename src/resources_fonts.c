/* The web-font loader: fetches the @font-face sources a stylesheet's
   fonts need, a few at a time between frames (fonts_external_loader_*), or
   to completion (fonts_load_external). Split out of resources.c. */

#include "tilefinch/resources.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/platform.h"
#include "tilefinch/url.h"
#include "data_url.h"
#include "resources_internal.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

static void font_accept_response_cookies(
    BrowserSession *session, TilefinchRequestContext *context,
    const char *fallback_url, const FetchResult *fetch)
{
    if (session == NULL || context == NULL || fetch == NULL) return;
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        context->target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            session, context, fetch->set_cookies[i]);
    }
    context->target_url = fallback_url;
}

static size_t font_charge_encoded_bytes(ExternalFontStats *stats,
                                        size_t bytes, size_t maximum)
{
    if (stats == NULL || bytes == 0 || stats->encoded_bytes >= maximum) {
        return 0;
    }
    size_t remaining = maximum - stats->encoded_bytes;
    size_t charged = bytes < remaining ? bytes : remaining;
    stats->encoded_bytes += charged;
    if (charged != bytes) stats->skipped_limit++;
    return charged;
}

static bool font_loader_arguments_valid(
    Stylesheet *sheet, Budget *budget, const char *document_base_url,
    const char *document_url, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, FetchScheduler *scheduler,
    ExternalFontStats *stats)
{
    if (sheet == NULL || budget == NULL || document_base_url == NULL
        || document_url == NULL || stats == NULL || maximum_attempts == 0
        || maximum_attempts > TILEFINCH_WEB_FONT_FAMILY_LIMIT * 2u
                               * STYLE_WEB_FONT_SOURCE_LIMIT
        || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0
        || maximum_face_backend_bytes == 0 || scheduler == NULL) return false;
    return true;
}

static void font_loader_update_elapsed(ExternalFontLoader *loader,
                                       ExternalFontStats *stats)
{
    if (loader == NULL || stats == NULL || loader->started_ms <= 0.0) return;
    double elapsed = resource_now_ms() - loader->started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    stats->elapsed_ms = (uint64_t) elapsed;
}

bool fonts_external_loader_begin(ExternalFontLoader *loader,
                                 Stylesheet *sheet, long timeout_ms,
                                 ExternalFontStats *stats)
{
    if (loader == NULL || sheet == NULL || stats == NULL
        || timeout_ms <= 0) return false;
    memset(loader, 0, sizeof(*loader));
    memset(stats, 0, sizeof(*stats));
    StylesheetWebFontStats discovery = {0};
    (void) stylesheet_web_font_stats(sheet, &discovery);
    stats->declarations_discovered = discovery.declarations_discovered;
    stats->sources_discovered = stylesheet_web_font_source_count(sheet);
    stats->unsupported = discovery.unsupported_sources;
    stats->duplicate_sources = discovery.duplicate_sources;
    stats->skipped_limit = discovery.skipped_family_limit
                           + discovery.skipped_source_limit;
    loader->started_ms = resource_now_ms();
    loader->deadline_ms = loader->started_ms + (double) timeout_ms;
    loader->active = stats->sources_discovered != 0;
    return true;
}

bool fonts_external_loader_pending(const ExternalFontLoader *loader)
{
    return loader != NULL && loader->active;
}

_Static_assert(TILEFINCH_WEB_FONT_FAMILY_LIMIT <= 8
    && TILEFINCH_WEB_FONT_FAMILY_LIMIT * TILEFINCH_WEB_FONT_WEIGHT_LIMIT <= 64
    && STYLE_WEB_FONT_TOTAL_SOURCE_LIMIT <= 64,
    "font continuation masks must cover all sources and faces");

static bool font_loader_source_required(const ExternalFontLoader *loader,
    const Stylesheet *sheet, const StylesheetWebFontSource *source)
{
    if (!loader->usage_filter) return true;
    if (source->family_slot >= TILEFINCH_WEB_FONT_FAMILY_LIMIT
        || source->face_slot >= TILEFINCH_WEB_FONT_WEIGHT_LIMIT) return false;
    unsigned shift = source->family_slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT;
    uint64_t bit = UINT64_C(1) << (shift + source->face_slot);
    if (!(loader->required_faces & bit)) return false;
    if (loader->preferred_faces & bit) return true;
    const WebFontSet *fonts = stylesheet_web_font_set(sheet);
    for (unsigned i = 0; i < TILEFINCH_WEB_FONT_WEIGHT_LIMIT; i++)
        if ((loader->preferred_faces & (UINT64_C(1) << (shift+i)))
            && (fonts == NULL || !fonts->families[source->family_slot].faces[i].loaded)) return true;
    return false; /* No fallback download after all desired faces loaded. */
}

static bool font_loader_next(ExternalFontLoader *loader, const Stylesheet *sheet)
{
    size_t count = stylesheet_web_font_source_count(sheet);
    size_t fallback = SIZE_MAX;
    for (size_t at = 0; at < count && at < STYLE_WEB_FONT_TOTAL_SOURCE_LIMIT; at++) {
        StylesheetWebFontSource source = {0};
        if ((loader->processed_sources & (UINT64_C(1) << at)) == 0
            && (!loader->usage_filter
                || (stylesheet_web_font_source(sheet, at, &source)
                    && font_loader_source_required(loader, sheet, &source)))) {
            uint64_t bit = UINT64_C(1) << (source.family_slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT + source.face_slot);
            if (loader->usage_filter && !(loader->preferred_faces & bit)) {
                if (fallback == SIZE_MAX) fallback = at;
                continue;
            }
            loader->next_source = at;
            return true;
        }
    }
    loader->next_source = fallback == SIZE_MAX ? count : fallback;
    return fallback != SIZE_MAX;
}

void fonts_external_loader_prioritize_faces(ExternalFontLoader *loader,
    const Stylesheet *sheet, uint64_t required_faces, uint64_t preferred_faces)
{
    preferred_faces &= required_faces;
    if (loader == NULL || sheet == NULL || loader->cancelled
        || loader->deadline_ms <= loader->started_ms
        || (loader->usage_filter
            && loader->required_faces == required_faces
            && loader->preferred_faces == preferred_faces)) return;
    bool was_active = loader->active;
    loader->usage_filter = true;
    loader->required_faces = required_faces;
    loader->preferred_faces = preferred_faces;
    loader->active = loader->request_id != 0 || font_loader_next(loader, sheet);
    if (!was_active && loader->active) {
        double window = loader->deadline_ms - loader->started_ms;
        loader->started_ms = resource_now_ms();
        loader->deadline_ms = loader->started_ms + window;
    }
}

void fonts_external_loader_cancel(ExternalFontLoader *loader,
                                  FetchScheduler *scheduler,
                                  ExternalFontStats *stats)
{
    if (loader == NULL) return;
    if (loader->request_id != 0 && scheduler != NULL) {
        (void) fetch_scheduler_cancel(
            scheduler, loader->request_id, "font continuation cancelled");
        (void) fetch_scheduler_discard(scheduler, loader->request_id);
    }
    loader->request_id = 0;
    loader->active = false;
    loader->cancelled = true;
    font_loader_update_elapsed(loader, stats);
}

static void font_loader_expire(ExternalFontLoader *loader,
                               FetchScheduler *scheduler,
                               ExternalFontStats *stats)
{
    if (loader == NULL || stats == NULL) return;
    stats->deadline_exceeded = true;
    if (loader->active
        && loader->next_source < stats->sources_discovered) {
        stats->deadline_cancelled +=
            stats->sources_discovered - loader->next_source;
    }
    fonts_external_loader_cancel(loader, scheduler, stats);
}

static bool font_loader_resolve_source(
    Stylesheet *sheet, size_t index, const char *document_base_url,
    const char *document_url, const char *referrer_policy,
    StylesheetWebFontSource *source, FontFace **destination,
    const char **referrer_source, const char **source_referrer_policy,
    char resolved[4096])
{
    if (!stylesheet_web_font_source(sheet, index, source)) return false;
    *destination = stylesheet_web_font_face_at(
        sheet, source->family_slot, source->face_slot);
    if (*destination == NULL) return false;
    const char *source_base = source->source_base_url != NULL
        ? source->source_base_url : document_base_url;
    *referrer_source = source->source_base_url != NULL
        ? source->source_base_url : document_url;
    *source_referrer_policy = source->source_referrer_policy != NULL
        ? source->source_referrer_policy : referrer_policy;
    if (source->reference == NULL) return false;
    if (strncasecmp(source->reference, "data:", 5) == 0) {
        resolved[0] = '\0';
        return true;
    }
    return fetch_resolve_url(source_base, source->reference, resolved, 4096);
}

static void font_loader_publish_encoded(
    Stylesheet *sheet, FontFace *destination, Budget *budget,
    const StylesheetWebFontSource *source, const unsigned char *bytes,
    size_t length, size_t maximum_face_backend_bytes,
    ExternalFontStats *stats, bool *face_loaded)
{
    bool shared = false;
    const WebFontSet *loaded_fonts = stylesheet_web_font_set(sheet);
    for (size_t family = 0; loaded_fonts != NULL && !shared
         && family < TILEFINCH_WEB_FONT_FAMILY_LIMIT; family++) {
        for (size_t face = 0; !shared && face < TILEFINCH_WEB_FONT_WEIGHT_LIMIT; face++)
            shared = font_face_share_encoded(destination,
                &loaded_fonts->families[family].faces[face], budget,
                bytes, length, maximum_face_backend_bytes);
    }
    if (shared || font_face_load_encoded(destination, budget, bytes, length,
                                         maximum_face_backend_bytes)) {
        stats->loaded_faces++;
        destination->authored_weight = source->weight;
        if (!shared) stats->retained_encoded_bytes += destination->data_length;
        *face_loaded = true;
    } else {
        stats->failed++;
        stats->unsupported++;
    }
}

bool fonts_external_loader_step(
    ExternalFontLoader *loader, Stylesheet *sheet, Budget *budget,
    const char *document_base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, FetchScheduler *scheduler,
    BrowserSession *session, const TilefinchContentSecurityPolicy *csp,
    ExternalFontStats *stats,
    unsigned maximum_wait_ms, bool *face_loaded)
{
    if (loader == NULL || face_loaded == NULL
        || !font_loader_arguments_valid(
            sheet, budget, document_base_url, document_url,
            maximum_attempts, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_face_backend_bytes,
            scheduler, stats)) return false;
    *face_loaded = false;
    if (!loader->active) return true;
    if (resource_now_ms() >= loader->deadline_ms) {
        font_loader_expire(loader, scheduler, stats);
        return true;
    }

    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(document_url, origin, sizeof(origin))) {
        stats->failed += stats->sources_discovered - loader->next_source;
        loader->active = false;
        font_loader_update_elapsed(loader, stats);
        return true;
    }

    if (loader->request_id != 0) {
        FetchPumpQuota quota = {
            .maximum_body_callbacks = 2,
            .maximum_body_bytes = 32u * 1024u,
            .maximum_time_us = 2000
        };
        FetchPumpMetrics metrics = {0};
        (void) fetch_scheduler_pump_bounded(
            scheduler, 1, maximum_wait_ms > 10 ? 10 : maximum_wait_ms,
            &quota, &metrics);
        FetchRequestProgress progress = {0};
        if (!fetch_scheduler_request_progress(
                scheduler, loader->request_id, &progress)
            || !progress.complete) {
            if (resource_now_ms() >= loader->deadline_ms) {
                font_loader_expire(loader, scheduler, stats);
            } else {
                font_loader_update_elapsed(loader, stats);
            }
            return true;
        }
        FetchResult *fetched = fetch_result_create(budget);
        if (fetched == NULL) {
            (void) fetch_scheduler_discard(
                scheduler, loader->request_id);
            loader->request_id = 0;
            loader->processed_sources |= UINT64_C(1) << loader->pending_source;
            loader->active = font_loader_next(loader, sheet);
            stats->failed++;
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        bool success = false;
        if (!fetch_scheduler_take(
                scheduler, loader->request_id, &success, fetched)) {
            fetch_result_free(fetched);
            if (resource_now_ms() >= loader->deadline_ms) {
                font_loader_expire(loader, scheduler, stats);
            } else {
                font_loader_update_elapsed(loader, stats);
            }
            return true;
        }
        size_t settled_index = loader->pending_source;
        loader->request_id = 0;
        loader->processed_sources |= UINT64_C(1) << settled_index;
        loader->active = font_loader_next(loader, sheet);
        StylesheetWebFontSource source = {0};
        FontFace *destination = NULL;
        const char *referrer_source = NULL;
        const char *source_referrer_policy = NULL;
        char resolved[4096];
        if (!font_loader_resolve_source(
                sheet, settled_index, document_base_url, document_url,
                referrer_policy, &source, &destination, &referrer_source,
                &source_referrer_policy, resolved)) {
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        size_t source_bytes_charged = font_charge_encoded_bytes(
            stats, fetched->received_body_bytes,
            maximum_total_encoded_bytes);
        if (!success) {
            if (fetched->timed_out) stats->deadline_exceeded = true;
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        TilefinchRequestContext request_context = {
            .target_url = resolved,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_CORS,
            .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            .destination = TILEFINCH_DESTINATION_FONT
        };
        font_accept_response_cookies(
            session, &request_context, resolved, fetched);
        if (fetched->status_code < 200 || fetched->status_code > 299) {
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        size_t additional_bytes =
            fetched->length > source_bytes_charged
                ? fetched->length - source_bytes_charged : 0;
        size_t request_maximum =
            maximum_total_encoded_bytes >=
                    stats->encoded_bytes - source_bytes_charged
                ? maximum_total_encoded_bytes
                    - (stats->encoded_bytes - source_bytes_charged)
                : 0;
        if (request_maximum > maximum_single_encoded_bytes) {
            request_maximum = maximum_single_encoded_bytes;
        }
        if (fetched->length > request_maximum
            || additional_bytes > maximum_total_encoded_bytes
                                   - stats->encoded_bytes) {
            stats->skipped_limit++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        if (additional_bytes != 0) {
            (void) font_charge_encoded_bytes(
                stats, additional_bytes, maximum_total_encoded_bytes);
        }
        font_loader_publish_encoded(sheet, destination, budget, &source,
            (const unsigned char *) fetched->data, fetched->length,
            maximum_face_backend_bytes, stats, face_loaded);
        fetch_result_free(fetched);
        font_loader_update_elapsed(loader, stats);
        return true;
    }

    unsigned source_turns = 0;
    while (source_turns++ < STYLE_WEB_FONT_TOTAL_SOURCE_LIMIT
           && font_loader_next(loader, sheet)) {
        if (resource_now_ms() >= loader->deadline_ms) {
            font_loader_expire(loader, scheduler, stats);
            return true;
        }
        size_t index = loader->next_source;
        if ((loader->processed_sources & (UINT64_C(1) << index)) != 0) {
            loader->next_source++;
            continue;
        }
        StylesheetWebFontSource candidate = {0};
        if (loader->usage_filter && stylesheet_web_font_source(sheet, index, &candidate)
            && !font_loader_source_required(loader, sheet, &candidate)) {
            loader->next_source++;
            continue;
        }
        loader->processed_sources |= UINT64_C(1) << index;
        StylesheetWebFontSource source = {0};
        FontFace *destination = NULL;
        const char *referrer_source = NULL;
        const char *source_referrer_policy = NULL;
        char resolved[4096];
        if (!font_loader_resolve_source(
                sheet, index, document_base_url, document_url,
                referrer_policy, &source, &destination, &referrer_source,
                &source_referrer_policy, resolved)) {
            stats->failed++;
            loader->next_source++;
            continue;
        }
        bool embedded = strncasecmp(source.reference, "data:", 5) == 0;
        if (!tilefinch_csp_allows_request(
                csp, TILEFINCH_DESTINATION_FONT,
                embedded ? source.reference : resolved)) {
            stats->failed++;
            loader->next_source++;
            continue;
        }
        if (destination->loaded) {
            stats->duplicate_sources++;
            loader->next_source++;
            continue;
        }
        if (stats->attempted >= maximum_attempts
            || stats->encoded_bytes >= maximum_total_encoded_bytes) {
            stats->skipped_limit++;
            loader->next_source++;
            continue;
        }
        size_t remaining_bytes = maximum_total_encoded_bytes
                                 - stats->encoded_bytes;
        size_t maximum = maximum_single_encoded_bytes < remaining_bytes
                         ? maximum_single_encoded_bytes : remaining_bytes;
        if (maximum == 0) {
            stats->skipped_limit++;
            loader->next_source++;
            continue;
        }
        if (embedded) {
            unsigned char *bytes = NULL;
            size_t length = 0;
            char media_type[128];
            stats->attempted++;
            DataUrlDecodeResult decoded = data_url_decode(budget,
                source.reference, strlen(source.reference), maximum,
                &bytes, &length, media_type, sizeof(media_type));
            if (decoded == DATA_URL_DECODED) {
                (void) font_charge_encoded_bytes(stats, length,
                                                  maximum_total_encoded_bytes);
                font_loader_publish_encoded(sheet, destination, budget, &source,
                    bytes, length, maximum_face_backend_bytes, stats, face_loaded);
            } else if (decoded == DATA_URL_TOO_LARGE) {
                stats->skipped_limit++;
            } else {
                stats->failed++;
                if (decoded != DATA_URL_NO_MEMORY) stats->unsupported++;
            }
            budget_free(budget, bytes);
            loader->next_source++;
            loader->active = font_loader_next(loader, sheet);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        /* A full scheduler reports both capacity pressure and policy refusal
           as a zero request id.  Settle blocker refusals here so the font
           continuation does not mistake a permanent decision for transient
           backpressure and retry it until the stage deadline. */
        if (session != NULL
            && content_blocker_would_block(
                   session->content_blocker, resolved, document_url,
                   "font", "cors")) {
            (void) content_blocker_should_block(
                session->content_blocker, resolved, document_url,
                "font", "cors");
            stats->attempted++;
            stats->failed++;
            loader->next_source++;
            continue;
        }
        /* The shared HTTP cache is intentionally not consulted here.  Its
           generic entries do not yet retain the request origin, CORS result,
           credentials mode, and redirect-origin taint needed to prove that a
           cached representation is reusable by this font request.  In
           particular, a same-origin cache key may hold a no-CORS response
           reached through a cross-origin redirect.  Page-local decoded faces
           remain retained by the stylesheet, so repeated glyph use is still
           allocation- and network-free. */
        TilefinchRequestContext request_context = {
            .target_url = resolved,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_CORS,
            .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            .destination = TILEFINCH_DESTINATION_FONT
        };
        FetchRequest transport = {
            .allow_http_errors = true,
            .accept = "font/woff,application/font-woff;q=0.9,*/*;q=0.1",
        };
        FetchPreparedPageRequest *prepared = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, sizeof(*prepared));
        if (prepared == NULL) {
            loader->processed_sources &= ~(UINT64_C(1) << index);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        if (!fetch_prepare_page_request_context(
                &request_context, referrer_source, source_referrer_policy,
                session, csp, NULL, &transport, prepared, NULL)) {
            budget_free(budget, prepared);
            stats->failed++;
            loader->next_source++;
            continue;
        }
        const FetchRequest *request = fetch_prepared_page_request(prepared);
        double remaining_ms = loader->deadline_ms - resource_now_ms();
        long request_timeout = remaining_ms < 1.0
            ? 1 : (long) remaining_ms;
        uint64_t request_id = fetch_scheduler_enqueue(
            scheduler, resolved, request, maximum, request_timeout);
        budget_free(budget, prepared);
        if (request_id == 0) {
            loader->processed_sources &= ~(UINT64_C(1) << index);
            /* A shared page scheduler can be temporarily full. Preserve
               source order and retry this source during the next idle slice. */
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        stats->attempted++;
        loader->pending_source = index;
        loader->request_id = request_id;
        font_loader_update_elapsed(loader, stats);
        return true;
    }
    loader->active = false;
    font_loader_update_elapsed(loader, stats);
    return true;
}

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
                         ExternalFontStats *stats)
{
    if (timeout_ms <= 0 || stats == NULL || budget == NULL) return false;
    bool owns_scheduler = scheduler == NULL;
    if (owns_scheduler) {
        scheduler = fetch_scheduler_create(
            budget, 1, maximum_single_encoded_bytes);
        if (scheduler == NULL) return false;
    }
    if (!font_loader_arguments_valid(
            sheet, budget, document_base_url, document_url,
            maximum_attempts, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_face_backend_bytes,
            scheduler, stats)) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    ExternalFontLoader loader = {0};
    bool ok = fonts_external_loader_begin(
        &loader, sheet, timeout_ms, stats);
    while (ok && fonts_external_loader_pending(&loader)) {
        bool face_loaded = false;
        ok = fonts_external_loader_step(
            &loader, sheet, budget, document_base_url, document_url,
            referrer_policy, maximum_attempts,
            maximum_total_encoded_bytes, maximum_single_encoded_bytes,
            maximum_face_backend_bytes, scheduler, session, csp, stats, 10,
            &face_loaded);
    }
    if (!ok) {
        fonts_external_loader_cancel(&loader, scheduler, stats);
    }
    if (owns_scheduler) fetch_scheduler_destroy(scheduler);
    return ok;
}
