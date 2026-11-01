#include "tilefinch/psp_media_hls.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/media_worker_request.h"
#include "tilefinch/navigation.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/url.h"
#include "tilefinch/psp_log.h"

#include "psp_media_hls_policy.h"
#include "psp_hls_gzip.h"

#define PSP_HLS_REQUEST_LIMIT 4u
#define PSP_HLS_SOURCE_LIMIT 2u
#define PSP_HLS_TIMEOUT_MS 30000
#define PSP_HLS_MASTER_DEPTH_LIMIT 4u
#define PSP_HLS_PLAYLIST_WIRE_CHUNK_BYTES MEDIA_HLS_TRANSPORT_CHUNK_BYTES

typedef struct {
    /* Stable for the logical request; the HLS core and the playlist pump
       hold it. Each hop of a page request is a new worker request. */
    uint64_t handle;
    /* The current hop's worker request; zero while a followed redirect
       waits for a worker descriptor. */
    uint64_t background_id;
    size_t consumed;
    size_t maximum_bytes;
    size_t publication_bytes;
    bool headers_admitted;
    bool raw_gzip;
    MediaWorkerHop hop;
} PspHlsRequest;

typedef enum {
    PSP_HLS_PLAYLIST_ENCODING_UNDECIDED = 0,
    PSP_HLS_PLAYLIST_ENCODING_IDENTITY,
    PSP_HLS_PLAYLIST_ENCODING_GZIP
} PspHlsPlaylistEncoding;

struct PspMediaHlsContext {
    Budget *budget;
    BrowserSession *session;
    void *memory_context;
    bool (*make_room)(void *, size_t, unsigned *);
    unsigned memory_stage;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    /* The page's CSP (NULL when it sent none or for provider streams). */
    const TilefinchContentSecurityPolicy *content_security_policy;
    /* Page media follows redirects across origins one admitted hop at a
       time; provider streams keep the worker's fixed-origin rule. */
    bool page_redirects;
    uint64_t next_handle;
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
    /* Variants that declare no AVC profile keep the software bound. */
    PspMediaHlsSelectionBound selection_bound;
    /* The chosen variant exceeds the software bound on an undeclared
       profile; its SPS decides. */
    bool selection_unverified;
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
    PspMediaHlsContext *context, uint64_t handle)
{
    if (context == NULL || handle == 0) return NULL;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].handle == handle)
            return &context->requests[i];
    return NULL;
}

static MediaWorkerAuthority psp_hls_authority(
    const PspMediaHlsContext *context)
{
    return (MediaWorkerAuthority) {
        .session = context->session,
        .document_url = context->document_url,
        .mode = context->mode,
        .credentials = context->credentials,
        .content_security_policy = context->content_security_policy,
        .page_redirects = context->page_redirects
    };
}

typedef enum {
    PSP_HLS_ISSUE_STARTED = 0,
    /* Worker backpressure: nothing failed, ask again on a later pump. */
    PSP_HLS_ISSUE_DEFERRED,
    PSP_HLS_ISSUE_FAILED
} PspHlsIssueStatus;

/* Issues the worker request for the request's current hop. */
static PspHlsIssueStatus psp_hls_issue(
    PspMediaHlsContext *context, PspHlsRequest *request,
    char *error, size_t error_size)
{
    FetchPreparedPageRequest *prepared = budget_malloc_category(
        context->budget, BUDGET_CATEGORY_NAVIGATION, sizeof(*prepared));
    if (prepared == NULL) {
        snprintf(error, error_size, "%s", "HLS request budget refused");
        return PSP_HLS_ISSUE_FAILED;
    }
    FetchRequest transport = {
        .allow_http_errors = true,
        .accept = "application/vnd.apple.mpegurl,video/mp2t,video/*;q=0.9,*/*;q=0.5",
        /* A long live DVR playlist can expand far beyond one publication
           quantum. Let the browser-owned playlist pump inflate gzip in
           bounded slices; libcurl must never retain a decoded tail while its
           worker is paused. Segments call this with raw_gzip=false. */
        .raw_gzip_encoding = request->raw_gzip,
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .connect_timeout_ms = PSP_HLS_CONNECT_TIMEOUT_MS
    };
    MediaWorkerAuthority authority = psp_hls_authority(context);
    FetchRequest stream_request;
    bool prepared_ok = media_worker_request_prepare(
        &authority, &request->hop, &transport, prepared, &stream_request);
    FetchBackgroundEnqueueStatus enqueue_status =
        FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    /* A page hop returns its redirect unfollowed so the browser thread can
       admit the next target (psp_hls_admit_headers). */
    uint64_t id = !prepared_ok ? 0 : context->page_redirects
        ? fetch_background_transport_enqueue_media_hop_stream_sized_diagnosed(
              request->hop.url, &stream_request, request->maximum_bytes,
              PSP_HLS_TIMEOUT_MS, request->publication_bytes,
              &enqueue_status)
        : fetch_background_transport_enqueue_media_stream_sized_diagnosed(
              request->hop.url, &stream_request, request->maximum_bytes,
              PSP_HLS_TIMEOUT_MS, request->publication_bytes,
              &enqueue_status);
    budget_free(context->budget, prepared);
    request->background_id = id;
    request->consumed = 0;
    request->headers_admitted = false;
    if (id != 0) return PSP_HLS_ISSUE_STARTED;
    if (prepared_ok
        && (enqueue_status == FETCH_BACKGROUND_ENQUEUE_SATURATED
            || enqueue_status == FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED))
        return PSP_HLS_ISSUE_DEFERRED;
    snprintf(error, error_size, "%s",
             !prepared_ok ? "HLS request authority refused"
                          : "HLS transport cannot admit request");
    return PSP_HLS_ISSUE_FAILED;
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
        if (context->requests[i].handle == 0) { slot = i; break; }
    if (slot == PSP_HLS_REQUEST_LIMIT) return 0;
    PspHlsRequest *request = &context->requests[slot];
    *request = (PspHlsRequest) {
        .maximum_bytes = maximum_bytes,
        .publication_bytes = publication_bytes,
        .raw_gzip = raw_gzip
    };
    if (!media_worker_hop_init(&request->hop, url)) {
        *request = (PspHlsRequest) {0};
        snprintf(error, error_size, "%s", "HLS request URL exceeds bound");
        return 0;
    }
    /* Playlists, variants and segments are admitted under the page's policy
       here, on the browser thread; the worker sees only the flat request. */
    MediaWorkerAuthority authority = psp_hls_authority(context);
    FetchPreparedPageRequest *scratch = !context->page_redirects ? NULL
        : budget_malloc_category(
              context->budget, BUDGET_CATEGORY_NAVIGATION, sizeof(*scratch));
    MediaWorkerHopAdmission admission = media_worker_hop_admit(
        &authority, &request->hop, scratch);
    budget_free(context->budget, scratch);
    if (admission != MEDIA_WORKER_HOP_ADMITTED) {
        *request = (PspHlsRequest) {0};
        snprintf(error, error_size, "%s",
                 admission == MEDIA_WORKER_HOP_REFUSED
                     ? "HLS request refused by page policy"
                     : "HLS request budget refused");
        return 0;
    }
    if (psp_hls_issue(context, request, error, error_size)
            != PSP_HLS_ISSUE_STARTED) {
        *request = (PspHlsRequest) {0};
        return 0;
    }
    if (++context->next_handle == 0) context->next_handle = 1;
    request->handle = context->next_handle;
    return request->handle;
}

static void psp_hls_request_cancel(PspHlsRequest *request,
                                   const char *reason)
{
    if (request == NULL) return;
    if (request->background_id != 0)
        (void) fetch_background_transport_cancel(
            request->background_id, reason);
    *request = (PspHlsRequest) {0};
}

static uint64_t psp_hls_start(void *opaque, const char *url,
                              size_t maximum_bytes,
                              char *error, size_t error_size)
{
    return psp_hls_start_sized(
        opaque, url, maximum_bytes, MEDIA_HLS_TRANSPORT_CHUNK_BYTES,
        false, error, error_size);
}

typedef enum {
    PSP_HLS_HEADERS_PENDING = 0,
    PSP_HLS_HEADERS_ADMITTED,
    /* A redirect was admitted and its next hop issued (or deferred). */
    PSP_HLS_HEADERS_REDIRECTED,
    PSP_HLS_HEADERS_REFUSED
} PspHlsHeadersStatus;

/* Follows an admitted redirect: the old hop is cancelled before its body is
   read, and a redirected playlist resolves its URIs against the new URL. */
static PspHlsHeadersStatus psp_hls_follow_redirect(
    PspMediaHlsContext *context, PspHlsRequest *request,
    char *error, size_t error_size)
{
    (void) fetch_background_transport_cancel(
        request->background_id, "HLS redirect followed");
    request->background_id = 0;
    if (request->handle == context->playlist_request_id
        && context->playlist_stream != NULL
        && !media_hls_playlist_stream_rebase(
               context->playlist_stream, request->hop.url)) {
        snprintf(error, error_size, "%s",
                 "HLS playlist redirect could not be rebased");
        return PSP_HLS_HEADERS_REFUSED;
    }
    return psp_hls_issue(context, request, error, error_size)
            == PSP_HLS_ISSUE_FAILED
        ? PSP_HLS_HEADERS_REFUSED : PSP_HLS_HEADERS_REDIRECTED;
}

static PspHlsHeadersStatus psp_hls_admit_headers(
    PspMediaHlsContext *context, PspHlsRequest *request,
    char *error, size_t error_size)
{
    FetchResult *metadata = fetch_result_create(context->budget);
    if (metadata == NULL) {
        snprintf(error, error_size, "%s", "HLS response metadata budget");
        return PSP_HLS_HEADERS_REFUSED;
    }
    bool available = fetch_background_transport_take_headers(
        request->background_id, metadata);
    if (!available) {
        fetch_result_free(metadata);
        return PSP_HLS_HEADERS_PENDING;
    }
    if (context->page_redirects && metadata->status_code >= 300
        && metadata->status_code < 400) {
        MediaWorkerAuthority authority = psp_hls_authority(context);
        FetchPreparedPageRequest *scratch = budget_malloc_category(
            context->budget, BUDGET_CATEGORY_NAVIGATION, sizeof(*scratch));
        MediaWorkerRedirectStatus redirect = media_worker_redirect(
            &authority, &request->hop, metadata, scratch);
        budget_free(context->budget, scratch);
        if (redirect != MEDIA_WORKER_RESPONSE) {
            long status = metadata->status_code;
            fetch_result_free(metadata);
            if (redirect == MEDIA_WORKER_REDIRECT_FOLLOW)
                return psp_hls_follow_redirect(
                    context, request, error, error_size);
            if (redirect == MEDIA_WORKER_REDIRECT_NO_MEMORY)
                snprintf(error, error_size, "%s",
                         "HLS redirect budget refused");
            else
                snprintf(error, error_size,
                         "HLS redirect refused (HTTP %ld)", status);
            return PSP_HLS_HEADERS_REFUSED;
        }
    }
    TilefinchRequestContext authority = {
        .target_url = request->hop.url,
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
    return admitted ? PSP_HLS_HEADERS_ADMITTED : PSP_HLS_HEADERS_REFUSED;
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
    if (request->background_id == 0) {
        /* A followed redirect found the worker queue full. */
        PspHlsIssueStatus issued = psp_hls_issue(
            context, request, error, error_size);
        if (issued == PSP_HLS_ISSUE_FAILED) {
            context->last_transport_retryable = false;
            *request = (PspHlsRequest) {0};
            return MEDIA_HLS_TRANSPORT_ERROR;
        }
        return MEDIA_HLS_TRANSPORT_WAIT;
    }
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(
            request->background_id, &progress)) {
        /* Network quiescence retires generation-bearing worker IDs. Retry
           the entire playlist (or rebuild the failed segment's pipeline),
           never append a restarted response to already-consumed bytes. */
        context->last_transport_retryable = true;
        context->last_transport_code = 0;
        *request = (PspHlsRequest) {0};
        snprintf(error, error_size, "%s", "HLS worker lost request");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    if (!request->headers_admitted) {
        PspHlsHeadersStatus headers = psp_hls_admit_headers(
            context, request, error, error_size);
        if (headers == PSP_HLS_HEADERS_REFUSED) {
            context->last_transport_retryable = false;
            psp_hls_request_cancel(request, "HLS response authority refused");
            return MEDIA_HLS_TRANSPORT_ERROR;
        }
        /* A hop that failed before any status line (resolver or connect
           timeout, refused connection) completes without ever publishing
           headers. Its terminal result is the answer; waiting for headers
           would hold the open until the phase timeout and skip the retry. */
        if (headers == PSP_HLS_HEADERS_PENDING && !progress.complete)
            return MEDIA_HLS_TRANSPORT_WAIT;
        if (headers == PSP_HLS_HEADERS_REDIRECTED)
            return MEDIA_HLS_TRANSPORT_WAIT;
    }
    if (fetch_background_transport_take_chunk(
            request->background_id, destination, capacity, length)) {
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
        request->background_id, context->budget, result, request->consumed);
    /* Only a response whose headers passed admission may deliver a body. */
    if (!request->headers_admitted && success) {
        success = false;
        snprintf(result->error, sizeof(result->error), "%s",
                 "response completed without admitted headers");
    }
    context->last_transport_code = result->transport_code;
    context->last_transport_retryable = !success
        && (result->transport_code != 0 || result->timed_out
            || (result->status_code >= 200 && result->status_code < 300
                && result->received_body_bytes != request->consumed));
    if (success) {
        TilefinchRequestContext cookie_context = {
            .target_url = request->hop.url,
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
    if (request == NULL) request = psp_hls_request(context, handle);
    psp_hls_request_cancel(request, message);
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
    psp_hls_request_cancel(
        psp_hls_request(context, handle), "HLS route closed");
}

PspMediaHlsContext *psp_media_hls_create(
    Budget *budget, BrowserSession *session,
    const char *playlist_url, const char *document_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    PspMediaHlsSelectionBound selection_bound,
    const TilefinchContentSecurityPolicy *content_security_policy,
    bool page_redirects, char *error, size_t error_size)
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
    context->selection_bound = selection_bound;
    context->content_security_policy = content_security_policy;
    context->page_redirects = page_redirects;
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

void psp_media_hls_set_memory_recovery(
    PspMediaHlsContext *context, void *opaque,
    bool (*make_room)(void *, size_t, unsigned *))
{
    if (context == NULL) return;
    context->memory_context = opaque;
    context->make_room = make_room;
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
        context, request->handle,
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
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
            printf("tilefinch-media-hls: event=playlist-retry "
                   "attempt=%u transport=%ld\n",
                   context->playlist_open_attempts,
                   context->last_transport_code);
#endif
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
        /* High profile goes to the software decoder (432x240); Baseline and
           Main go to the firmware decoder, up to its geometry. A variant
           without a declared profile is tried at the firmware bound first;
           the open path checks its SPS and reselects at the software bound
           when it turns out to be High. */
        MediaHlsDecoderLimits limits = {
            .software_width = 432u,
            .software_height = 240u,
            .target_height = 240u,
            .admit_undeclared_firmware =
                context->selection_bound == PSP_MEDIA_HLS_BOUND_FIRMWARE,
            .firmware_refused =
                context->selection_bound == PSP_MEDIA_HLS_BOUND_SOFTWARE
        };
        MediaHlsSelection selection = {0};
        bool selected_ok = context->opening_source == 0u
            && media_hls_playlist_select_decodable_streams(
                playlist, &limits,
                selected_video, sizeof(selected_video),
                selected_audio, sizeof(selected_audio), &selection);
        if (!selected_ok) {
            media_hls_playlist_destroy(playlist);
            return psp_hls_open_fail(
                context, context->opening_source != 0u
                    ? "nested HLS audio master is unsupported"
                    : selection.video_selected
                        ? "HLS master is missing its selected audio rendition"
                        : "HLS master has no compatible AVC rendition",
                error, error_size);
        }
        context->selection_unverified = selection.unverified;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* Page-video device pass: the variant the decoder-aware selection
           chose. A second record after a reselect carries software-only=1. */
        psp_log_printf("tilefinch-page-media: event=hls-variant video=%ux%u "
               "unverified=%d software-only=%d page=%d separate-audio=%d "
               "url=\"%.200s\"\n",
               (unsigned) selection.width, (unsigned) selection.height,
               selection.unverified ? 1 : 0,
               context->selection_bound != PSP_MEDIA_HLS_BOUND_FIRMWARE ? 1 : 0,
               context->page_redirects ? 1 : 0,
               selected_audio[0] != '\0' ? 1 : 0, selected_video);
#endif
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
    for (;;) {
        context->source[source_index] = media_hls_source_create_track(
            context->budget, playlist, &transport,
            context->track_selection[source_index], error, error_size);
        if (context->source[source_index] != NULL
            || context->make_room == NULL || context->memory_stage >= 2u
            || error == NULL
            || strcmp(error, "HLS source exceeds memory budget") != 0)
            break;
        /* The factory has freed its partial buffers and still leaves the
           playlist with us. Use the same two recovery stages as decoder
           opening, including native-heap fragmentation (needed=0), before
           retrying. No refetch, limit increase, or change to page authority. */
        unsigned stage = context->memory_stage;
        if (!context->make_room(context->memory_context, 0,
                                &context->memory_stage)
            || context->memory_stage <= stage)
            break;
    }
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

bool psp_media_hls_failed(const PspMediaHlsContext *context)
{
    if (context == NULL) return false;
    if (context->failed) return true;
    for (size_t i = 0; i < context->source_count; i++)
        if (media_hls_source_failed(context->source[i])) return true;
    return false;
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
    stats->holding = context->source_count != 0;
    for (size_t i = 0; i < context->source_count; i++) {
        if (context->source[i] == NULL) {
            stats->ended = false;
            stats->holding = false;
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
        stats->holding = stats->holding && (one.holding || one.ended);
    }
    if (context->playlist_request_id != 0) stats->active_requests++;
    return true;
}

bool psp_media_hls_selection_unverified(const PspMediaHlsContext *context)
{
    return context != NULL && context->selection_unverified;
}

void psp_media_hls_destroy(PspMediaHlsContext *context)
{
    if (context == NULL) return;
    for (size_t i = 0; i < PSP_HLS_REQUEST_LIMIT; i++)
        if (context->requests[i].handle != 0)
            psp_hls_cancel(context, context->requests[i].handle);
    for (size_t i = 0; i < PSP_HLS_SOURCE_LIMIT; i++)
        media_hls_source_destroy(context->source[i]);
    psp_hls_destroy_playlist_stream(context);
    budget_free(context->budget, context->playlist_wire_chunk);
    budget_free(context->budget, context->playlist_chunk);
    budget_free(context->budget, context);
}
