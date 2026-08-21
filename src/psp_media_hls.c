#include "tilefinch/psp_media_hls.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/navigation.h"
#include "tilefinch/user_agent.h"

#define PSP_HLS_REQUEST_LIMIT 2u
#define PSP_HLS_TIMEOUT_MS 30000
#define PSP_HLS_MASTER_DEPTH_LIMIT 4u

typedef struct {
    uint64_t background_id;
    size_t consumed;
    bool headers_admitted;
    char url[NAVIGATION_URL_LIMIT];
} PspHlsRequest;

struct PspMediaHlsContext {
    Budget *budget;
    BrowserSession *session;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    char document_url[NAVIGATION_URL_LIMIT];
    char playlist_url[NAVIGATION_URL_LIMIT];
    unsigned char *playlist_bytes;
    size_t playlist_length;
    MediaHlsPlaylist *playlist;
    MediaHlsSource *source;
    unsigned master_depth;
    PspHlsRequest requests[PSP_HLS_REQUEST_LIMIT];
    bool failed;
    char error[192];
};

static void psp_hls_error(PspMediaHlsContext *context, const char *message)
{
    if (context == NULL || context->failed) return;
    snprintf(context->error, sizeof(context->error), "%s", message);
    context->failed = true;
}

static PspHlsRequest *psp_hls_request(
    PspMediaHlsContext *context, uint64_t id)
{
    if (context == NULL || id == 0) return NULL;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].background_id == id)
            return &context->requests[i];
    return NULL;
}

static uint64_t psp_hls_start(void *opaque, const char *url,
                              size_t maximum_bytes,
                              char *error, size_t error_size)
{
    PspMediaHlsContext *context = opaque;
    if (context == NULL || url == NULL
        || maximum_bytes > MEDIA_HLS_MAXIMUM_SEGMENT_BYTES) return 0;
    size_t slot = PSP_HLS_REQUEST_LIMIT;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].background_id == 0) { slot = i; break; }
    if (slot == PSP_HLS_REQUEST_LIMIT) return 0;

    FetchPreparedPageRequest *prepared = budget_malloc_category(
        context->budget, BUDGET_CATEGORY_NAVIGATION, sizeof(*prepared));
    if (prepared == NULL) {
        snprintf(error, error_size, "%s", "HLS request budget refused");
        return 0;
    }
    TilefinchRequestContext authority = {
        .target_url = url,
        .initiator_url = context->document_url,
        .top_level_url = context->document_url,
        .method = "GET",
        .mode = context->mode,
        .credentials = context->credentials,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    FetchRequest transport = {
        .allow_http_errors = true,
        .accept = "application/vnd.apple.mpegurl,video/mp2t,video/*;q=0.9,*/*;q=0.5",
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .connect_timeout_ms = 3000,
        .redirect_same_origin_only = true
    };
    FetchRequestValidationError validation_error;
    bool prepared_ok = fetch_prepare_page_request_context(
        &authority, context->document_url, NULL, context->session, NULL,
        context->session == NULL ? NULL : context->session->content_blocker,
        &transport, prepared, &validation_error);
    const FetchRequest *request = prepared_ok
        ? fetch_prepared_page_request(prepared) : NULL;
    FetchBackgroundEnqueueStatus enqueue_status =
        FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    uint64_t id = request == NULL ? 0
        : fetch_background_transport_enqueue_media_stream_sized_diagnosed(
              url, request, maximum_bytes, PSP_HLS_TIMEOUT_MS,
              MEDIA_HLS_TRANSPORT_CHUNK_BYTES, &enqueue_status);
    budget_free(context->budget, prepared);
    if (id == 0) {
        if (request == NULL
            || (enqueue_status != FETCH_BACKGROUND_ENQUEUE_SATURATED
                && enqueue_status
                    != FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED)) {
            snprintf(error, error_size, "%s",
                     request == NULL ? "HLS request authority refused"
                                     : "HLS transport cannot admit request");
        }
        return 0;
    }
    context->requests[slot] = (PspHlsRequest) {
        .background_id = id
    };
    snprintf(context->requests[slot].url,
             sizeof(context->requests[slot].url), "%s", url);
    return id;
}

static bool psp_hls_admit_headers(PspMediaHlsContext *context,
                                  PspHlsRequest *request,
                                  char *error, size_t error_size)
{
    FetchResult *metadata = fetch_result_create(context->budget);
    if (metadata == NULL) {
        snprintf(error, error_size, "%s", "HLS response metadata budget");
        return false;
    }
    bool available = fetch_background_transport_take_headers(
        request->background_id, metadata);
    if (!available) {
        fetch_result_free(metadata);
        return false;
    }
    TilefinchRequestContext authority = {
        .target_url = request->url,
        .initiator_url = context->document_url,
        .top_level_url = context->document_url,
        .method = "GET",
        .mode = context->mode,
        .credentials = context->credentials,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    TilefinchResourceGrant grant;
    TilefinchResourceDeniedReason denied;
    bool admitted = metadata->status_code >= 200
        && metadata->status_code < 300
        && fetch_resource_grant_create(
            metadata, &authority,
            context->mode == TILEFINCH_REQUEST_MODE_CORS,
            true, false, &grant, &denied);
    if (!admitted)
        snprintf(error, error_size,
                 "HLS response authority refused (HTTP %ld)",
                 metadata->status_code);
    fetch_result_free(metadata);
    request->headers_admitted = admitted;
    return admitted;
}

static MediaHlsTransportPollResult psp_hls_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    PspMediaHlsContext *context = opaque;
    PspHlsRequest *request = psp_hls_request(context, handle);
    if (length != NULL) *length = 0;
    if (request == NULL) {
        snprintf(error, error_size, "%s", "HLS request disappeared");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(handle, &progress)) {
        *request = (PspHlsRequest) {0};
        snprintf(error, error_size, "%s", "HLS worker lost request");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    if (!request->headers_admitted) {
        if (!psp_hls_admit_headers(context, request, error, error_size)) {
            if (error != NULL && error[0] != '\0') {
                (void) fetch_background_transport_cancel(
                    handle, "HLS response authority refused");
                *request = (PspHlsRequest) {0};
                return MEDIA_HLS_TRANSPORT_ERROR;
            }
            return MEDIA_HLS_TRANSPORT_WAIT;
        }
    }
    if (fetch_background_transport_take_chunk(
            handle, destination, capacity, length)) {
        request->consumed += *length;
        return MEDIA_HLS_TRANSPORT_CHUNK;
    }
    if (!progress.complete) return MEDIA_HLS_TRANSPORT_WAIT;

    FetchResult *result = fetch_result_create(context->budget);
    if (result == NULL) {
        snprintf(error, error_size, "%s", "HLS completion budget refused");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    bool success = fetch_background_transport_take_fetch_result_consumed(
        handle, context->budget, result, request->consumed);
    if (success) {
        TilefinchRequestContext cookie_context = {
            .target_url = request->url,
            .initiator_url = context->document_url,
            .top_level_url = context->document_url,
            .method = "GET",
            .mode = context->mode,
            .credentials = context->credentials,
            .destination = TILEFINCH_DESTINATION_MEDIA
        };
        for (size_t i = 0; i < result->set_cookie_count; i++) {
            cookie_context.target_url = fetch_set_cookie_url(
                result, i, result->effective_url);
            (void) browser_session_cookie_set_http_context(
                context->session, &cookie_context, result->set_cookies[i]);
        }
    } else {
        snprintf(error, error_size, "HLS fetch failed: %.120s",
                 result->error[0] == '\0' ? "incomplete response"
                                          : result->error);
    }
    fetch_result_free(result);
    *request = (PspHlsRequest) {0};
    return success ? MEDIA_HLS_TRANSPORT_COMPLETE
                   : MEDIA_HLS_TRANSPORT_ERROR;
}

static void psp_hls_cancel(void *opaque, uint64_t handle)
{
    PspMediaHlsContext *context = opaque;
    PspHlsRequest *request = psp_hls_request(context, handle);
    if (request == NULL) return;
    (void) fetch_background_transport_cancel(handle, "HLS route closed");
    *request = (PspHlsRequest) {0};
}

PspMediaHlsContext *psp_media_hls_create(
    Budget *budget, BrowserSession *session,
    const char *playlist_url, const char *document_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    char *error, size_t error_size)
{
    if (budget == NULL || session == NULL || playlist_url == NULL
        || document_url == NULL || !fetch_background_transport_available()) {
        snprintf(error, error_size, "%s", "HLS transport unavailable");
        return NULL;
    }
    PspMediaHlsContext *context = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*context));
    unsigned char *bytes = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES);
    if (context == NULL || bytes == NULL) {
        budget_free(budget, bytes);
        budget_free(budget, context);
        snprintf(error, error_size, "%s", "HLS playlist budget refused");
        return NULL;
    }
    context->budget = budget;
    context->session = session;
    context->mode = mode;
    context->credentials = credentials;
    context->playlist_bytes = bytes;
    snprintf(context->playlist_url, sizeof(context->playlist_url), "%s",
             playlist_url);
    snprintf(context->document_url, sizeof(context->document_url), "%s",
             document_url);
    return context;
}

PspMediaHlsOpenStatus psp_media_hls_pump(
    PspMediaHlsContext *context, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (context == NULL || context->failed) {
        snprintf(error, error_size, "%s", context == NULL
            ? "HLS source unavailable" : context->error);
        return PSP_MEDIA_HLS_OPEN_FAILED;
    }
    if (context->source != NULL) {
        MediaHlsPrimeStatus status = media_hls_source_prime(
            context->source, error, error_size);
        return status == MEDIA_HLS_PRIME_READY ? PSP_MEDIA_HLS_OPEN_READY
            : status == MEDIA_HLS_PRIME_FAILED ? PSP_MEDIA_HLS_OPEN_FAILED
                                                : PSP_MEDIA_HLS_OPEN_PENDING;
    }
    if (context->requests[0].background_id == 0
        && context->requests[1].background_id == 0) {
        char start_error[160] = {0};
        uint64_t id = psp_hls_start(
            context, context->playlist_url,
            MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES,
            start_error, sizeof(start_error));
        if (id == 0) {
            if (start_error[0] != '\0') psp_hls_error(context, start_error);
            return context->failed ? PSP_MEDIA_HLS_OPEN_FAILED
                                   : PSP_MEDIA_HLS_OPEN_PENDING;
        }
    }
    PspHlsRequest *request = context->requests[0].background_id != 0
        ? &context->requests[0] : &context->requests[1];
    size_t length = 0;
    MediaHlsTransportPollResult result = psp_hls_poll(
        context, request->background_id,
        context->playlist_bytes + context->playlist_length,
        MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES - context->playlist_length,
        &length, error, error_size);
    if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
        context->playlist_length += length;
        return PSP_MEDIA_HLS_OPEN_PENDING;
    }
    if (result == MEDIA_HLS_TRANSPORT_WAIT) return PSP_MEDIA_HLS_OPEN_PENDING;
    if (result == MEDIA_HLS_TRANSPORT_ERROR) {
        psp_hls_error(context, error[0] == '\0'
            ? "HLS playlist fetch failed" : error);
        return PSP_MEDIA_HLS_OPEN_FAILED;
    }
    context->playlist = media_hls_playlist_parse(
        context->budget, context->playlist_url,
        context->playlist_bytes, context->playlist_length,
        error, error_size);
    context->playlist_length = 0;
    if (context->playlist == NULL) {
        psp_hls_error(context, error);
        return PSP_MEDIA_HLS_OPEN_FAILED;
    }
    if (media_hls_playlist_kind(context->playlist)
            == MEDIA_HLS_PLAYLIST_MASTER) {
        char selected[NAVIGATION_URL_LIMIT];
        bool selected_ok = media_hls_playlist_select_variant(
            context->playlist, 432u, 240u, 240u,
            selected, sizeof(selected));
        media_hls_playlist_destroy(context->playlist);
        context->playlist = NULL;
        if (!selected_ok) {
            psp_hls_error(context, "HLS has no compatible 240p variant");
            return PSP_MEDIA_HLS_OPEN_FAILED;
        }
        if (++context->master_depth > PSP_HLS_MASTER_DEPTH_LIMIT) {
            psp_hls_error(context, "HLS master nesting exceeds its bound");
            return PSP_MEDIA_HLS_OPEN_FAILED;
        }
        snprintf(context->playlist_url, sizeof(context->playlist_url), "%s",
                 selected);
        return PSP_MEDIA_HLS_OPEN_PENDING;
    }
    MediaHlsTransport transport = {
        .opaque = context,
        .start = psp_hls_start,
        .poll = psp_hls_poll,
        .cancel = psp_hls_cancel
    };
    context->source = media_hls_source_create(
        context->budget, context->playlist, &transport,
        error, error_size);
    if (context->source == NULL) {
        psp_hls_error(context, error);
        return PSP_MEDIA_HLS_OPEN_FAILED;
    }
    context->playlist = NULL; /* source owns it */
    return PSP_MEDIA_HLS_OPEN_PENDING;
}

bool psp_media_hls_sample_source(
    PspMediaHlsContext *context, MediaSampleSource *source)
{
    return context != NULL && context->source != NULL
        && media_hls_source_sample_source(context->source, source);
}

bool psp_media_hls_stream_info(
    PspMediaHlsContext *context, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio)
{
    return context != NULL && context->source != NULL
        && media_hls_source_stream_info(context->source, video, audio);
}

bool psp_media_hls_stats(
    PspMediaHlsContext *context, MediaHlsStats *stats)
{
    if (context == NULL || context->source == NULL || stats == NULL)
        return false;
    media_hls_source_stats(context->source, stats);
    return true;
}

void psp_media_hls_destroy(PspMediaHlsContext *context)
{
    if (context == NULL) return;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].background_id != 0)
            psp_hls_cancel(context, context->requests[i].background_id);
    media_hls_source_destroy(context->source);
    media_hls_playlist_destroy(context->playlist);
    budget_free(context->budget, context->playlist_bytes);
    budget_free(context->budget, context);
}
