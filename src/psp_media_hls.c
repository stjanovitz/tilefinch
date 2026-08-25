#include "tilefinch/psp_media_hls.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/navigation.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/url.h"

#include "psp_media_hls_policy.h"
#include "psp_hls_gzip.h"

#define PSP_HLS_REQUEST_LIMIT 4u
#define PSP_HLS_SOURCE_LIMIT 2u
#define PSP_HLS_TIMEOUT_MS 30000
#define PSP_HLS_MASTER_DEPTH_LIMIT 4u
#define PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES MEDIA_HLS_TRANSPORT_CHUNK_BYTES

typedef struct {
    uint64_t background_id;
    size_t consumed;
    bool headers_admitted;
    bool raw_gzip;
    char url[NAVIGATION_URL_LIMIT];
} PspHlsRequest;

typedef enum {
    PSP_HLS_PLAYLIST_ENCODING_UNDECIDED = 0,
    PSP_HLS_PLAYLIST_ENCODING_IDENTITY,
    PSP_HLS_PLAYLIST_ENCODING_GZIP
} PspHlsPlaylistEncoding;

struct PspMediaHlsContext {
    Budget *budget;
    BrowserSession *session;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    char document_url[NAVIGATION_URL_LIMIT];
    char playlist_url[PSP_HLS_SOURCE_LIMIT][NAVIGATION_URL_LIMIT];
    unsigned char *playlist_chunk;
    unsigned char *playlist_wire_chunk;
    MediaHlsPlaylistStream *playlist_stream;
    MediaHlsSource *source[PSP_HLS_SOURCE_LIMIT];
    MediaHlsTrackSelection track_selection[PSP_HLS_SOURCE_LIMIT];
    size_t source_count;
    size_t opening_source;
    size_t priming_source;
    size_t playlist_request_source;
    uint64_t playlist_request_id;
    size_t playlist_bytes_received;
    unsigned playlist_open_attempts;
    unsigned master_depth;
    long last_transport_code;
    bool last_transport_retryable;
    PspHlsGzip playlist_inflater;
    size_t playlist_wire_length;
    size_t playlist_wire_offset;
    PspHlsPlaylistEncoding playlist_encoding;
    PspHlsRequest requests[PSP_HLS_REQUEST_LIMIT];
    bool failed;
    char error[192];
};

static void psp_hls_reset_playlist_inflater(PspMediaHlsContext *context)
{
    if (context == NULL) return;
    psp_hls_gzip_reset(&context->playlist_inflater);
    context->playlist_wire_length = 0;
    context->playlist_wire_offset = 0;
    context->playlist_encoding = PSP_HLS_PLAYLIST_ENCODING_UNDECIDED;
}

static void psp_hls_error(PspMediaHlsContext *context, const char *message)
{
    if (context == NULL || context->failed) return;
    snprintf(context->error, sizeof(context->error), "%s", message);
    context->failed = true;
}

static PspMediaHlsOpenStatus psp_hls_open_fail(
    PspMediaHlsContext *context, const char *message,
    char *error, size_t error_size)
{
    psp_hls_error(context, message);
    if (error != NULL && error_size != 0) {
        snprintf(error, error_size, "%s",
                 context != NULL && context->error[0] != '\0'
                     ? context->error : "HLS source failed");
    }
    return PSP_MEDIA_HLS_OPEN_FAILED;
}

static void psp_hls_destroy_playlist_stream(PspMediaHlsContext *context)
{
    if (context == NULL) return;
    if (context->playlist_stream != NULL) {
        size_t bytes = media_hls_playlist_stream_bytes_seen(
            context->playlist_stream);
        context->playlist_bytes_received = bytes
                > SIZE_MAX - context->playlist_bytes_received
            ? SIZE_MAX : context->playlist_bytes_received + bytes;
        media_hls_playlist_stream_destroy(context->playlist_stream);
        context->playlist_stream = NULL;
    }
    psp_hls_reset_playlist_inflater(context);
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

static bool psp_hls_redirect_url_supported(const char *value)
{
    TilefinchUrl url;
    return tilefinch_url_parse(value, &url)
        && (url.scheme == TILEFINCH_URL_SCHEME_HTTP
            || url.scheme == TILEFINCH_URL_SCHEME_HTTPS);
}

static uint64_t psp_hls_start_sized(
    void *opaque, const char *url, size_t maximum_bytes,
    size_t publication_bytes, bool raw_gzip,
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
        /* A long live DVR playlist can expand far beyond one publication
           quantum. Let the browser-owned playlist pump inflate gzip in
           bounded slices; libcurl must never retain a decoded tail while its
           worker is paused. Segments call this with raw_gzip=false. */
        .raw_gzip_encoding = raw_gzip,
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .connect_timeout_ms = 3000,
        .redirect_same_origin_only = true
    };
    FetchRequestValidationError validation_error;
    bool prepared_ok = fetch_prepare_page_request_context(
        &authority, context->document_url, NULL, context->session, NULL,
        context->session == NULL ? NULL : context->session->content_blocker,
        &transport, prepared, &validation_error);
    const FetchRequest *authorized = prepared_ok
        ? fetch_prepared_page_request(prepared) : NULL;
    FetchRequest stream_request = authorized == NULL
        ? (FetchRequest) {0} : *authorized;
    /* The prepared request owns the page authority and cookie snapshot. The
       streaming worker additionally requires a redirect validator because
       it has no BrowserSession on its thread. Same-origin remains the hard
       redirect boundary; this callback admits only syntactically valid HTTP
       targets before that origin comparison is applied. */
    stream_request.redirect_url_validator =
        psp_hls_redirect_url_supported;
    stream_request.redirect_same_origin_only = true;
    stream_request.page_context = NULL;
    stream_request.content_security_policy = NULL;
    stream_request.content_blocker = NULL;
    stream_request.cookie_session = NULL;
    stream_request.cookie_context = NULL;
    stream_request.prepared_page_version = 0;
    const FetchRequest *request = authorized == NULL
        ? NULL : &stream_request;
    FetchBackgroundEnqueueStatus enqueue_status =
        FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    uint64_t id = request == NULL ? 0
        : fetch_background_transport_enqueue_media_stream_sized_diagnosed(
              url, request, maximum_bytes, PSP_HLS_TIMEOUT_MS,
              publication_bytes, &enqueue_status);
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
        .background_id = id,
        .raw_gzip = raw_gzip
    };
    snprintf(context->requests[slot].url,
             sizeof(context->requests[slot].url), "%s", url);
    return id;
}

static uint64_t psp_hls_start(void *opaque, const char *url,
                              size_t maximum_bytes,
                              char *error, size_t error_size)
{
    return psp_hls_start_sized(
        opaque, url, maximum_bytes, MEDIA_HLS_TRANSPORT_CHUNK_BYTES,
        false, error, error_size);
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
        if (context != NULL) context->last_transport_retryable = false;
        snprintf(error, error_size, "%s", "HLS request disappeared");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(handle, &progress)) {
        context->last_transport_retryable = false;
        *request = (PspHlsRequest) {0};
        snprintf(error, error_size, "%s", "HLS worker lost request");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    if (!request->headers_admitted) {
        if (!psp_hls_admit_headers(context, request, error, error_size)) {
            if (error != NULL && error[0] != '\0') {
                context->last_transport_retryable = false;
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
        context->last_transport_retryable = false;
        snprintf(error, error_size, "%s", "HLS completion budget refused");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    bool success = fetch_background_transport_take_fetch_result_consumed(
        handle, context->budget, result, request->consumed);
    context->last_transport_code = result->transport_code;
    context->last_transport_retryable = !success
        && (result->transport_code != 0 || result->timed_out
            || (result->status_code >= 200 && result->status_code < 300
                && result->received_body_bytes != request->consumed));
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

static bool psp_hls_playlist_inflater_begin(
    PspMediaHlsContext *context, char *error, size_t error_size)
{
    if (!psp_hls_gzip_begin(
            &context->playlist_inflater, context->budget)) {
        snprintf(error, error_size, "%s",
                 "HLS gzip playlist exceeds memory budget");
        return false;
    }
    return true;
}

static MediaHlsTransportPollResult psp_hls_playlist_decode_fail(
    PspMediaHlsContext *context, PspHlsRequest *request,
    uint64_t handle, const char *message, int detail,
    char *error, size_t error_size)
{
    context->last_transport_retryable = false;
    if (detail == 0)
        snprintf(error, error_size, "%s", message);
    else
        snprintf(error, error_size, "%s (%d)", message, detail);
    (void) fetch_background_transport_cancel(handle, message);
    if (request != NULL) *request = (PspHlsRequest) {0};
    return MEDIA_HLS_TRANSPORT_ERROR;
}

static MediaHlsTransportPollResult psp_hls_poll_playlist(
    PspMediaHlsContext *context, uint64_t handle,
    unsigned char *destination, size_t capacity, size_t *length,
    char *error, size_t error_size)
{
    if (length != NULL) *length = 0;
    if (context == NULL || destination == NULL || capacity == 0
        || length == NULL || context->playlist_wire_chunk == NULL) {
        snprintf(error, error_size, "%s", "invalid HLS playlist pump");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    PspHlsRequest *request = psp_hls_request(context, handle);
    if (request == NULL || !request->raw_gzip) {
        return psp_hls_poll(
            context, handle, destination, capacity, length,
            error, error_size);
    }

    /* One browser pump performs at most one transport take and one bounded
       inflate call. A compressed input slice remains resident until zlib has
       consumed it, so neither side retains a pointer into a callback stack. */
    if (context->playlist_wire_offset
            == context->playlist_wire_length) {
        context->playlist_wire_offset = 0;
        context->playlist_wire_length = 0;
        MediaHlsTransportPollResult transport = psp_hls_poll(
            context, handle, context->playlist_wire_chunk,
            PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES,
            &context->playlist_wire_length, error, error_size);
        if (transport != MEDIA_HLS_TRANSPORT_CHUNK) {
            if (transport == MEDIA_HLS_TRANSPORT_COMPLETE
                && context->playlist_encoding
                     == PSP_HLS_PLAYLIST_ENCODING_GZIP
                && !context->playlist_inflater.finished) {
                snprintf(error, error_size, "%s",
                         "HLS gzip playlist ended before its trailer");
                return MEDIA_HLS_TRANSPORT_ERROR;
            }
            return transport;
        }
        if (context->playlist_wire_length == 0)
            return MEDIA_HLS_TRANSPORT_WAIT;
        if (context->playlist_encoding
                == PSP_HLS_PLAYLIST_ENCODING_UNDECIDED) {
            context->playlist_encoding =
                context->playlist_wire_length >= 2u
                && context->playlist_wire_chunk[0] == 0x1fu
                && context->playlist_wire_chunk[1] == 0x8bu
                    ? PSP_HLS_PLAYLIST_ENCODING_GZIP
                    : PSP_HLS_PLAYLIST_ENCODING_IDENTITY;
            if (context->playlist_encoding
                    == PSP_HLS_PLAYLIST_ENCODING_GZIP
                && !psp_hls_playlist_inflater_begin(
                    context, error, error_size)) {
                return psp_hls_playlist_decode_fail(
                    context, request, handle,
                    "HLS gzip playlist exceeds memory budget", 0,
                    error, error_size);
            }
        }
    }

    size_t available = context->playlist_wire_length
        - context->playlist_wire_offset;
    if (context->playlist_encoding == PSP_HLS_PLAYLIST_ENCODING_IDENTITY) {
        size_t copied = available < capacity ? available : capacity;
        memcpy(destination,
               context->playlist_wire_chunk + context->playlist_wire_offset,
               copied);
        context->playlist_wire_offset += copied;
        *length = copied;
        return MEDIA_HLS_TRANSPORT_CHUNK;
    }
    if (!context->playlist_inflater.initialized
        || context->playlist_inflater.finished) {
        return psp_hls_playlist_decode_fail(
            context, request, handle, "invalid HLS gzip stream state", 0,
            error, error_size);
    }
    int status = Z_OK;
    size_t consumed = 0;
    size_t produced = 0;
    PspHlsGzipStatus gzip_status = psp_hls_gzip_pump(
        &context->playlist_inflater,
        context->playlist_wire_chunk + context->playlist_wire_offset,
        available, destination, capacity, &consumed, &produced, &status);
    context->playlist_wire_offset += consumed;
    if (gzip_status == PSP_HLS_GZIP_REFUSED) {
        return psp_hls_playlist_decode_fail(
            context, request, handle,
            "HLS gzip playlist exceeds memory budget", status,
            error, error_size);
    }
    if (gzip_status == PSP_HLS_GZIP_MALFORMED) {
        return psp_hls_playlist_decode_fail(
            context, request, handle, "HLS gzip playlist is malformed",
            status, error, error_size);
    }
    if (produced != 0) {
        *length = produced;
        return MEDIA_HLS_TRANSPORT_CHUNK;
    }
    if (gzip_status == PSP_HLS_GZIP_STALLED) {
        return psp_hls_playlist_decode_fail(
            context, request, handle, "HLS gzip playlist made no progress",
            0, error, error_size);
    }
    return MEDIA_HLS_TRANSPORT_WAIT;
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
    unsigned char *chunk = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, PSP_HLS_PLAYLIST_CHUNK_BYTES);
    unsigned char *wire_chunk = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE,
        PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES);
    if (context == NULL || chunk == NULL || wire_chunk == NULL) {
        budget_free(budget, wire_chunk);
        budget_free(budget, chunk);
        budget_free(budget, context);
        snprintf(error, error_size, "%s", "HLS playlist budget refused");
        return NULL;
    }
    context->budget = budget;
    context->session = session;
    context->mode = mode;
    context->credentials = credentials;
    context->playlist_chunk = chunk;
    context->playlist_wire_chunk = wire_chunk;
    context->source_count = 1u;
    context->track_selection[0] = MEDIA_HLS_TRACK_MIXED;
    snprintf(context->playlist_url[0], sizeof(context->playlist_url[0]), "%s",
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
    if (context->opening_source >= context->source_count) {
        while (context->priming_source < context->source_count) {
            MediaHlsPrimeStatus status = media_hls_source_prime(
                context->source[context->priming_source],
                error, error_size);
            if (status == MEDIA_HLS_PRIME_FAILED) {
                return psp_hls_open_fail(
                    context, error[0] == '\0'
                        ? "HLS source prime failed" : error,
                    error, error_size);
            }
            if (status == MEDIA_HLS_PRIME_PENDING)
                return PSP_MEDIA_HLS_OPEN_PENDING;
            context->priming_source++;
        }
        return PSP_MEDIA_HLS_OPEN_READY;
    }
    if (context->playlist_request_id == 0) {
        char start_error[160] = {0};
        psp_hls_destroy_playlist_stream(context);
        context->playlist_stream = media_hls_playlist_stream_create(
            context->budget,
            context->playlist_url[context->opening_source],
            start_error, sizeof(start_error));
        if (context->playlist_stream == NULL) {
            return psp_hls_open_fail(
                context, start_error[0] == '\0'
                    ? "HLS playlist parser budget refused" : start_error,
                error, error_size);
        }
        uint64_t id = psp_hls_start_sized(
            context, context->playlist_url[context->opening_source],
            MEDIA_HLS_MAXIMUM_PLAYLIST_DOWNLOAD_BYTES,
            PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES, true,
            start_error, sizeof(start_error));
        if (id == 0) {
            psp_hls_destroy_playlist_stream(context);
            if (start_error[0] != '\0') {
                return psp_hls_open_fail(
                    context, start_error, error, error_size);
            }
            return PSP_MEDIA_HLS_OPEN_PENDING;
        }
        context->playlist_request_id = id;
        context->playlist_request_source = context->opening_source;
        context->playlist_open_attempts++;
    }
    PspHlsRequest *request = psp_hls_request(
        context, context->playlist_request_id);
    if (request == NULL) {
        return psp_hls_open_fail(
            context, "HLS playlist request disappeared", error, error_size);
    }
    size_t length = 0;
    MediaHlsTransportPollResult result = psp_hls_poll_playlist(
        context, request->background_id,
        context->playlist_chunk, PSP_HLS_PLAYLIST_CHUNK_BYTES,
        &length, error, error_size);
    if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
        if (!media_hls_playlist_stream_feed(
                context->playlist_stream, context->playlist_chunk, length,
                error, error_size)) {
            psp_hls_cancel(context, context->playlist_request_id);
            context->playlist_request_id = 0;
            return psp_hls_open_fail(
                context, error[0] == '\0'
                    ? "HLS playlist parsing failed" : error,
                error, error_size);
        }
        return PSP_MEDIA_HLS_OPEN_PENDING;
    }
    if (result == MEDIA_HLS_TRANSPORT_WAIT) return PSP_MEDIA_HLS_OPEN_PENDING;
    context->playlist_request_id = 0;
    if (result == MEDIA_HLS_TRANSPORT_ERROR) {
        if (psp_hls_playlist_retry_allowed(
                context->playlist_open_attempts,
                context->last_transport_retryable)) {
            printf("tilefinch-media-hls: event=playlist-retry "
                   "attempt=%u transport=%ld\n",
                   context->playlist_open_attempts,
                   context->last_transport_code);
            psp_hls_destroy_playlist_stream(context);
            return PSP_MEDIA_HLS_OPEN_PENDING;
        }
        return psp_hls_open_fail(
            context, error[0] == '\0'
                ? "HLS playlist fetch failed" : error,
            error, error_size);
    }
    MediaHlsPlaylist *playlist = media_hls_playlist_stream_finish(
        context->playlist_stream, error, error_size);
    psp_hls_destroy_playlist_stream(context);
    if (playlist == NULL) {
        return psp_hls_open_fail(
            context, error[0] == '\0'
                ? "HLS playlist parsing failed" : error,
            error, error_size);
    }
    if (media_hls_playlist_kind(playlist) == MEDIA_HLS_PLAYLIST_MASTER) {
        char selected_video[NAVIGATION_URL_LIMIT];
        char selected_audio[NAVIGATION_URL_LIMIT];
        bool selected_ok = context->opening_source == 0u
            && media_hls_playlist_select_streams(
                playlist, 432u, 240u, 240u,
                selected_video, sizeof(selected_video),
                selected_audio, sizeof(selected_audio));
        if (!selected_ok) {
            char compatible_video[NAVIGATION_URL_LIMIT];
            bool has_video = context->opening_source == 0u
                && media_hls_playlist_select_variant(
                    playlist, 432u, 240u, 240u,
                    compatible_video, sizeof(compatible_video));
            media_hls_playlist_destroy(playlist);
            return psp_hls_open_fail(
                context, context->opening_source != 0u
                    ? "nested HLS audio master is unsupported"
                    : has_video
                        ? "HLS master is missing its selected audio rendition"
                        : "HLS master has no compatible 240p AVC rendition",
                error, error_size);
        }
        media_hls_playlist_destroy(playlist);
        if (++context->master_depth > PSP_HLS_MASTER_DEPTH_LIMIT) {
            return psp_hls_open_fail(
                context, "HLS master nesting exceeds its bound",
                error, error_size);
        }
        snprintf(context->playlist_url[0],
                 sizeof(context->playlist_url[0]), "%s", selected_video);
        if (selected_audio[0] != '\0') {
            context->source_count = 2u;
            context->track_selection[0] = MEDIA_HLS_TRACK_VIDEO;
            context->track_selection[1] = MEDIA_HLS_TRACK_AUDIO;
            snprintf(context->playlist_url[1],
                     sizeof(context->playlist_url[1]), "%s", selected_audio);
        } else {
            context->source_count = 1u;
            context->track_selection[0] = MEDIA_HLS_TRACK_MIXED;
            context->playlist_url[1][0] = '\0';
        }
        context->playlist_open_attempts = 0;
        return PSP_MEDIA_HLS_OPEN_PENDING;
    }
    MediaHlsTransport transport = {
        .opaque = context,
        .start = psp_hls_start,
        .poll = psp_hls_poll,
        .cancel = psp_hls_cancel
    };
    size_t source_index = context->opening_source;
    context->source[source_index] = media_hls_source_create_track(
        context->budget, playlist, &transport,
        context->track_selection[source_index], error, error_size);
    if (context->source[source_index] == NULL) {
        media_hls_playlist_destroy(playlist);
        return psp_hls_open_fail(
            context, error[0] == '\0'
                ? "HLS sample source creation failed" : error,
            error, error_size);
    }
    context->opening_source++;
    context->playlist_open_attempts = 0;
    return PSP_MEDIA_HLS_OPEN_PENDING;
}

void psp_media_hls_pump_delivery(
    PspMediaHlsContext *context, uint64_t now_us)
{
    if (context == NULL || context->failed
        || context->opening_source < context->source_count) return;
    for (size_t i = 0; i < context->source_count; i++)
        media_hls_source_pump(context->source[i], now_us);
    if (context->playlist_request_id == 0) {
        for (size_t i = 0; i < context->source_count; i++) {
            if (!media_hls_source_wants_playlist_refresh(
                    context->source[i], now_us)) continue;
            char error[160] = {0};
            psp_hls_destroy_playlist_stream(context);
            context->playlist_stream = media_hls_playlist_stream_create(
                context->budget, context->playlist_url[i],
                error, sizeof(error));
            if (context->playlist_stream == NULL) {
                media_hls_source_note_playlist_refresh_failure(
                    context->source[i], now_us);
                break;
            }
            context->playlist_request_id = psp_hls_start_sized(
                context, context->playlist_url[i],
                MEDIA_HLS_MAXIMUM_PLAYLIST_DOWNLOAD_BYTES,
                PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES, true,
                error, sizeof(error));
            if (context->playlist_request_id == 0) {
                psp_hls_destroy_playlist_stream(context);
                if (error[0] != '\0')
                    media_hls_source_note_playlist_refresh_failure(
                        context->source[i], now_us);
            } else {
                context->playlist_request_source = i;
            }
            break;
        }
    }
    if (context->playlist_request_id == 0) return;
    PspHlsRequest *request = psp_hls_request(
        context, context->playlist_request_id);
    if (request == NULL) {
        context->playlist_request_id = 0;
        media_hls_source_note_playlist_refresh_failure(
            context->source[context->playlist_request_source], now_us);
        psp_hls_destroy_playlist_stream(context);
        return;
    }
    size_t length = 0;
    char error[160] = {0};
    MediaHlsTransportPollResult result = psp_hls_poll_playlist(
        context, context->playlist_request_id,
        context->playlist_chunk, PSP_HLS_PLAYLIST_CHUNK_BYTES,
        &length, error, sizeof(error));
    if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
        if (!media_hls_playlist_stream_feed(
                context->playlist_stream, context->playlist_chunk, length,
                error, sizeof(error))) {
            psp_hls_cancel(context, context->playlist_request_id);
            context->playlist_request_id = 0;
            media_hls_source_note_playlist_refresh_failure(
                context->source[context->playlist_request_source], now_us);
            psp_hls_destroy_playlist_stream(context);
        }
        return;
    }
    if (result == MEDIA_HLS_TRANSPORT_WAIT) return;
    context->playlist_request_id = 0;
    if (result == MEDIA_HLS_TRANSPORT_ERROR) {
        media_hls_source_note_playlist_refresh_failure(
            context->source[context->playlist_request_source], now_us);
        psp_hls_destroy_playlist_stream(context);
        return;
    }
    MediaHlsPlaylist *replacement = media_hls_playlist_stream_finish(
        context->playlist_stream, error, sizeof(error));
    psp_hls_destroy_playlist_stream(context);
    if (replacement == NULL
        || media_hls_playlist_kind(replacement)
             != MEDIA_HLS_PLAYLIST_MEDIA) {
        media_hls_playlist_destroy(replacement);
        media_hls_source_note_playlist_refresh_failure(
            context->source[context->playlist_request_source], now_us);
        return;
    }
    (void) media_hls_source_update_playlist(
        context->source[context->playlist_request_source], replacement,
        now_us, error, sizeof(error));
}

bool psp_media_hls_is_live(const PspMediaHlsContext *context)
{
    if (context == NULL) return false;
    for (size_t i = 0; i < context->source_count; i++) {
        MediaHlsStats stats = {0};
        media_hls_source_stats(context->source[i], &stats);
        if (stats.live) return true;
    }
    return false;
}

bool psp_media_hls_failed(const PspMediaHlsContext *context)
{
    if (context == NULL) return false;
    if (context->failed) return true;
    for (size_t i = 0; i < context->source_count; i++)
        if (media_hls_source_failed(context->source[i])) return true;
    return false;
}

bool psp_media_hls_sample_source(
    PspMediaHlsContext *context, MediaSampleSource *source)
{
    return context != NULL && context->source[0] != NULL
        && media_hls_source_sample_source(context->source[0], source);
}

bool psp_media_hls_sample_sources(
    PspMediaHlsContext *context, MediaSampleSource *video_source,
    MediaSampleSource *audio_source, bool *has_separate_audio)
{
    if (has_separate_audio != NULL) *has_separate_audio = false;
    if (context == NULL || video_source == NULL
        || context->source[0] == NULL
        || !media_hls_source_sample_source(
            context->source[0], video_source)) return false;
    bool separate = context->source_count == 2u
        && context->source[1] != NULL;
    if (separate && (audio_source == NULL
        || !media_hls_source_sample_source(
            context->source[1], audio_source))) return false;
    if (audio_source != NULL && !separate)
        *audio_source = (MediaSampleSource) {0};
    if (has_separate_audio != NULL) *has_separate_audio = separate;
    return true;
}

bool psp_media_hls_stream_info(
    PspMediaHlsContext *context, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio)
{
    if (context == NULL || context->source[0] == NULL
        || !media_hls_source_stream_info(
            context->source[0], video,
            context->source_count == 1u ? audio : NULL)) return false;
    return context->source_count != 2u || audio == NULL
        || media_hls_source_stream_info(context->source[1], NULL, audio);
}

static size_t psp_hls_size_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

bool psp_media_hls_stats(
    PspMediaHlsContext *context, MediaHlsStats *stats)
{
    if (context == NULL || stats == NULL) return false;
    memset(stats, 0, sizeof(*stats));
    stats->playlist_bytes_received = context->playlist_bytes_received;
    if (context->playlist_stream != NULL) {
        size_t current = media_hls_playlist_stream_bytes_seen(
            context->playlist_stream);
        stats->playlist_bytes_received = current
                > SIZE_MAX - stats->playlist_bytes_received
            ? SIZE_MAX : stats->playlist_bytes_received + current;
    }
    stats->ended = true;
    for (size_t i = 0; i < context->source_count; i++) {
        if (context->source[i] == NULL) {
            stats->ended = false;
            continue;
        }
        MediaHlsStats one = {0};
        media_hls_source_stats(context->source[i], &one);
#define PSP_HLS_SUM(field) stats->field = psp_hls_size_add(                 \
        stats->field, one.field)
        PSP_HLS_SUM(segments_started);
        PSP_HLS_SUM(segments_completed);
        PSP_HLS_SUM(bytes_received);
        PSP_HLS_SUM(queued_samples);
        PSP_HLS_SUM(queued_bytes);
        PSP_HLS_SUM(queue_overflows);
        PSP_HLS_SUM(malformed_segments);
        PSP_HLS_SUM(playlist_refreshes);
        PSP_HLS_SUM(playlist_refresh_failures);
        PSP_HLS_SUM(skipped_live_segments);
#undef PSP_HLS_SUM
        stats->ts_sync_losses += one.ts_sync_losses;
        stats->ts_malformed_packets += one.ts_malformed_packets;
        stats->ts_malformed_psi += one.ts_malformed_psi;
        stats->active_requests += one.active_requests;
        if (one.buffered_until_us > stats->buffered_until_us)
            stats->buffered_until_us = one.buffered_until_us;
        stats->live = stats->live || one.live;
        stats->ended = stats->ended && one.ended;
    }
    if (context->playlist_request_id != 0) stats->active_requests++;
    return true;
}

void psp_media_hls_destroy(PspMediaHlsContext *context)
{
    if (context == NULL) return;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].background_id != 0)
            psp_hls_cancel(context, context->requests[i].background_id);
    for (size_t i = 0; i < PSP_HLS_SOURCE_LIMIT; i++)
        media_hls_source_destroy(context->source[i]);
    psp_hls_destroy_playlist_stream(context);
    budget_free(context->budget, context->playlist_wire_chunk);
    budget_free(context->budget, context->playlist_chunk);
    budget_free(context->budget, context);
}
