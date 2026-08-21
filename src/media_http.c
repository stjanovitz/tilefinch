#include "tilefinch/media_http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/user_agent.h"
#include "tilefinch_compiler.h"

#define MEDIA_HTTP_DEFAULT_CACHE_BYTES (64u * 1024u)
#define MEDIA_HTTP_MINIMUM_CACHE_BYTES (4u * 1024u)
#define MEDIA_HTTP_MAXIMUM_CACHE_BYTES (256u * 1024u)
#define MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS 2u
#define MEDIA_HTTP_USER_AGENT TILEFINCH_BROWSER_USER_AGENT
/*
 * Start the next window after one quarter of the current one has been
 * consumed. The original half-window policy assumed a 0.3-1.6 s refill, but a
 * refusal-heavy 120 s hardware soak measured a 4.405 s maximum. Half of the
 * 240p video window can be consumed in less time than that; retaining three
 * quarters gives the same single-buffer design about six seconds of cover.
 * This moves the same request earlier -- it does not add a request or retain a
 * second cache window.
 */
#define MEDIA_HTTP_READAHEAD_CONSUMED_DIVISOR 4u
#define MEDIA_HTTP_AGGRESSIVE_READAHEAD_CONSUMED_DIVISOR 16u
/* One bounded scheduler pump per wait step: the longest interval in which a
   blocking (open-time) range read cannot see a cancellation request. */
#define MEDIA_HTTP_WAIT_STEP_MS 10u

typedef enum {
    RANGE_FILL_ISSUED = 0,
    RANGE_FILL_DEFERRED,
    RANGE_FILL_FAILED
} RangeFillIssueStatus;

typedef enum {
    RANGE_WINDOW_IDLE = 0,
    RANGE_WINDOW_PENDING,
    RANGE_WINDOW_FAILED
} RangeWindowState;

struct MediaHttpRange {
    Budget *budget;
    BrowserSession *session;
    char *url;
    char *range_url;
    size_t range_url_capacity;
    char *referer;
    char range_header[64];
    bool standard_range_header;
    FetchPreparedPageRequest *prepared_request;
    unsigned char *cache;
    size_t cache_capacity;
    uint64_t cache_offset;
    size_t cache_length;
    /* High-water mark of consumption inside the active window. What is left
       unconsumed is what the readahead policy decides against. */
    size_t cache_consumed;
    /* Optional completed sequential successor. The active and lookahead
       allocations exchange roles on promotion, so crossing a boundary is
       pointer-only and never copies 256 KiB on the browser thread. */
    unsigned char *lookahead[MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS];
    uint64_t lookahead_offset[MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS];
    size_t lookahead_length[MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS];
    unsigned lookahead_capacity;
    unsigned lookahead_slots;
    unsigned lookahead_count;
    unsigned lookahead_fetch_limit;
    size_t stream_publication_bytes;
    uint64_t content_length;
    long timeout_ms;
    long connect_timeout_ms;
    MediaHttpRangeTransport transport;
    void *transport_opaque;
    MediaHttpUrlValidator url_validator;
    FetchCancelCallback cancel;
    void *cancel_opaque;
    /* PSP live networking keeps curl's DNS/TCP/TLS progress on the bounded
       process worker. Host replay and substituted transports retain their
       existing deterministic paths. */
    bool use_background_transport;
    bool aggressive_readahead;
    /*
     * The window being fetched. Its bytes live in the scheduler's own bounded
     * response buffer from the moment the request is issued until the active
     * window is replaced, which is what lets a refill overlap decoding instead
     * of stopping it. Nothing new is retained for that: a blocking range read
     * already held a response buffer of the same bound for the duration of the
     * call, so the peak is one window plus one response, exactly as before.
     */
    FetchScheduler *scheduler;
    uint64_t fill_request;
    uint64_t fill_offset;
    size_t fill_length;
    bool fill_admission_deferred;
    /* Sequential video refills stream into the optional owned successor.
       Header admission happens before the first byte is exposed; the browser
       thread alone copies published chunks, so the decoder never aliases the
       transport worker's buffer. */
    bool fill_streaming;
    unsigned fill_stream_slot;
    bool fill_stream_headers_received;
    bool fill_stream_headers_admitted;
    size_t fill_stream_received_bytes;
    size_t fill_streamed_bytes;
    unsigned fill_attempts;
    /* Byte-progress watch and the number of fresh physical requests already
       spent on this logical window. */
    MediaHttpWindowTracker window_tracker;
    bool fill_stall_exhausted;
    /* One logical demanded window, distinct from the physical request which
       may be replaced onto a fresh connection. A terminal window is never
       also pending; selecting another offset receives a fresh bounded retry
       scope. */
    RangeWindowState window_state;
    uint64_t window_offset;
    size_t window_length;
    size_t minimum_sustained_bytes_per_second;
    /* When this window was issued, and when a read first had to answer
       would-block against it. Zero for "not armed"; the second is re-armed
       per window so a window nobody waited on charges no starvation. */
    uint64_t fill_issued_us;
    uint64_t fill_starved_us;
    /*
     * Absolute monotonic deadline shared by every blocking read, or zero when
     * only the per-window `timeout_ms` applies. See
     * media_http_range_set_wait_budget_us: the per-window deadline bounds one
     * read and composes to nothing across the many an MP4 open performs.
     * `wait_budget_armed` distinguishes "no budget was set" from "the budget
     * is spent", which are opposite answers.
     */
    uint64_t wait_deadline_us;
    bool wait_budget_armed;
    uint64_t last_read_offset;
    size_t last_read_length;
    bool successor_prime_complete;
    bool successor_prime_failed;
    MediaHttpRangeStats stats;
    char last_error[256];
};

static void media_http_error(char *error, size_t error_size,
                             const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

bool media_http_parse_content_range(
    const char *value, uint64_t expected_first,
    uint64_t expected_last, size_t body_length,
    uint64_t *complete_length)
{
    if (value == NULL || strncmp(value, "bytes ", 6) != 0) return false;
    unsigned long long first = 0, last = 0, complete = 0;
    char tail = '\0';
    if (sscanf(value + 6, "%llu-%llu/%llu%c",
               &first, &last, &complete, &tail) != 3
        || first != expected_first || last < first
        || last > expected_last || complete == 0
        || last >= complete
        || last - first + 1u != body_length) {
        return false;
    }
    *complete_length = (uint64_t) complete;
    return true;
}

static bool append_bytes(char *output, size_t output_size, size_t *length,
                         const char *bytes, size_t bytes_length)
{
    if (output == NULL || length == NULL || bytes == NULL
        || *length >= output_size
        || bytes_length > output_size - *length - 1u) return false;
    memcpy(output + *length, bytes, bytes_length);
    *length += bytes_length;
    output[*length] = '\0';
    return true;
}

bool media_http_build_range_url(
    const char *url, uint64_t first_byte, uint64_t last_byte,
    char *output, size_t output_size)
{
    if (url == NULL || url[0] == '\0' || last_byte < first_byte
        || output == NULL || output_size == 0) return false;
    output[0] = '\0';
    const char *url_end = url + strlen(url);
    const char *fragment = strchr(url, '#');
    const char *query_end = fragment == NULL ? url_end : fragment;
    const char *query = memchr(url, '?', (size_t) (query_end - url));
    const char *base_end = query == NULL ? query_end : query;
    size_t length = 0;
    if (!append_bytes(
            output, output_size, &length, url,
            (size_t) (base_end - url))) return false;

    bool have_query = false;
    if (query != NULL) {
        const char *at = query + 1;
        while (at < query_end) {
            const char *token_end = memchr(
                at, '&', (size_t) (query_end - at));
            if (token_end == NULL) token_end = query_end;
            size_t token_length = (size_t) (token_end - at);
            const char *equals = memchr(at, '=', token_length);
            size_t key_length = equals == NULL
                ? token_length : (size_t) (equals - at);
            bool is_range = key_length == 5u
                && memcmp(at, "range", 5u) == 0;
            if (token_length != 0 && !is_range) {
                const char separator = have_query ? '&' : '?';
                if (!append_bytes(
                        output, output_size, &length, &separator, 1u)
                    || !append_bytes(
                        output, output_size, &length, at,
                        token_length)) return false;
                have_query = true;
            }
            at = token_end < query_end ? token_end + 1 : query_end;
        }
    }
    char range_query[64];
    int range_length = snprintf(
        range_query, sizeof(range_query), "%crange=%llu-%llu",
        have_query ? '&' : '?',
        (unsigned long long) first_byte,
        (unsigned long long) last_byte);
    if (range_length < 0 || (size_t) range_length >= sizeof(range_query)
        || !append_bytes(
            output, output_size, &length, range_query,
            (size_t) range_length)) return false;
    if (fragment != NULL
        && !append_bytes(
            output, output_size, &length, fragment,
            (size_t) (url_end - fragment))) return false;
    return true;
}

bool media_http_build_range_header(
    uint64_t first_byte, uint64_t last_byte,
    char *output, size_t output_size)
{
    if (output == NULL || output_size == 0 || last_byte < first_byte)
        return false;
    int written = snprintf(
        output, output_size, "Range: bytes=%llu-%llu",
        (unsigned long long) first_byte,
        (unsigned long long) last_byte);
    return written >= 0 && (size_t) written < output_size;
}

/* One request shape for both the blocking and the issue/poll paths, so a
   header, a timeout or the redirect validator can only be set in one place. */
static bool range_prepare_request(
    MediaHttpRange *range, uint64_t first_byte, uint64_t last_byte,
    FetchRequest *request, char *error, size_t error_size)
{
    bool url_ready = false;
    if (range->standard_range_header) {
        int written = snprintf(
            range->range_url, range->range_url_capacity, "%s", range->url);
        url_ready = written >= 0
            && (size_t) written < range->range_url_capacity;
    } else {
        url_ready = media_http_build_range_url(
            range->url, first_byte, last_byte,
            range->range_url, range->range_url_capacity);
    }
    if (!url_ready) {
        media_http_error(error, error_size, "media range URL full");
        return false;
    }
    if (range->standard_range_header
        && !media_http_build_range_header(
               first_byte, last_byte, range->range_header,
               sizeof(range->range_header))) {
        media_http_error(error, error_size, "media Range header full");
        return false;
    }
    const FetchRequest *authorized = range->prepared_request == NULL
        ? NULL : fetch_prepared_page_request(range->prepared_request);
    *request = authorized == NULL ? (FetchRequest) {
        .method = "GET",
        .allow_http_errors = true,
        .referer = range->referer[0] == '\0' ? NULL : range->referer,
        .accept = "video/mp4,video/*;q=0.9,*/*;q=0.5",
        .sec_fetch_dest = "video",
        .sec_fetch_mode = "no-cors",
        .sec_fetch_site = "cross-site",
        .user_agent = MEDIA_HTTP_USER_AGENT,
        .credentials = FETCH_CREDENTIALS_OMIT,
        .initiator_url = range->referer[0] == '\0' ? NULL : range->referer,
        .referrer_source = range->referer[0] == '\0' ? NULL : range->referer,
        .connect_timeout_ms = range->connect_timeout_ms,
        /* A trickle reconnect must not multiplex back onto the HTTP/2
           connection whose measured rate caused the reconnect. Device
           evidence showed cancellation alone retained that connection. */
        .force_fresh_connection = range->window_tracker.reconnects != 0u,
        .redirect_url_validator = range->url_validator,
        /* The native streaming worker can follow redirects only when their
           origin remains fixed; the final media-host validator still checks
           the effective URL independently. Direct googlevideo range URLs do
           not need a redirect in the normal case. Keeping this request shape
           explicit also lets host replay enforce the native admission
           contract instead of accepting a stream the PSP will refuse. */
        .redirect_same_origin_only = true
    } : *authorized;
    request->extra_headers = range->standard_range_header
        ? range->range_header : request->extra_headers;
    request->connect_timeout_ms = range->connect_timeout_ms;
    request->force_fresh_connection =
        range->window_tracker.reconnects != 0u;
    request->redirect_url_validator = range->url_validator;
    request->redirect_same_origin_only = true;
    return true;
}

/*
 * Admit one bounded range response. A partial must carry the Content-Range the
 * request asked for; a provider `range=` query answers 200 with exactly the
 * requested bytes. Either way the final effective URL is re-checked against
 * the media host policy before a byte enters the window.
 */
static bool range_admit_values(
    MediaHttpRange *range, uint64_t first_byte, uint64_t last_byte,
    long status_code, size_t body_length, const char *content_range,
    const char *effective_url, const char *transport_error, bool fetched,
    uint64_t *complete_length, char *error, size_t error_size)
{
    range->stats.last_http_status = status_code;
    range->stats.last_first_byte = first_byte;
    range->stats.last_last_byte = last_byte;
    size_t expected_length = (size_t) (last_byte - first_byte + 1u);
    bool exact_query_body = !range->standard_range_header
        && fetched && status_code == 200
        && body_length == expected_length;
    bool valid_partial = fetched && status_code == 206
        && body_length == expected_length && content_range != NULL
        && content_range[0] != '\0'
        && media_http_parse_content_range(
               content_range, first_byte, last_byte, body_length,
               complete_length);
    const char *admitted_url = effective_url == NULL || effective_url[0] == '\0'
        ? range->range_url : effective_url;
    bool effective_url_admitted =
        range->url_validator == NULL
        || range->url_validator(admitted_url);
    if (exact_query_body) *complete_length = range->content_length;
    if (effective_url_admitted && (exact_query_body || valid_partial))
        return true;
    media_http_error(
        error, error_size,
        "range %llu-%llu failed: HTTP %ld, %zuB, %s",
        (unsigned long long) first_byte,
        (unsigned long long) last_byte,
        status_code, body_length,
        !effective_url_admitted
            ? "redirected outside admitted media hosts"
            : transport_error == NULL || transport_error[0] == '\0'
            ? "invalid bounded range response" : transport_error);
    return false;
}

static bool range_admit_response(
    MediaHttpRange *range, uint64_t first_byte, uint64_t last_byte,
    FetchResult *result, bool fetched, uint64_t *complete_length,
    char *error, size_t error_size)
{
    char content_range[128] = {0};
    (void) fetch_response_header_value(
        result, "content-range", content_range, sizeof(content_range));
    return range_admit_values(
        range, first_byte, last_byte, result->status_code, result->length,
        content_range, result->effective_url, result->error, fetched,
        complete_length, error, error_size);
}

/*
 * The issue/poll window machine.
 *
 * This replaced a synchronous fetch_request_cancelable() per window. What it
 * buys is that a *playing* stream never waits: the request for the next window
 * is on the wire while the decoder works through the current one, and a read
 * whose bytes have not landed answers WOULD_BLOCK in microseconds instead of
 * holding the interactive thread for a whole HTTP transaction. Device cycle
 * truth2 measured that transaction at 0.3 s, 0.7 s and 1.64 s inside single
 * feed units.
 *
 * A substituted transport (the host fixtures, and any future non-HTTP source)
 * is synchronous by contract and reaches no network, so it keeps the
 * straight-line fill in range_fill_window() below and never answers
 * WOULD_BLOCK.
 */
static bool range_uses_scheduler(const MediaHttpRange *range)
{
    return range->transport == NULL && !range->use_background_transport;
}

static bool range_uses_background(const MediaHttpRange *range)
{
    return range->transport == NULL && range->use_background_transport;
}

static bool range_uses_async_transport(const MediaHttpRange *range)
{
    return range_uses_scheduler(range) || range_uses_background(range);
}

static bool range_scheduler_ready(MediaHttpRange *range)
{
    if (range_uses_background(range))
        return fetch_background_transport_initialize(range->budget);
    if (range->scheduler != NULL) return true;
    /* One request, one window's worth of reservation. Created with the first
       fill so a session that never opens a video pays nothing. */
    range->scheduler = fetch_scheduler_create(
        range->budget, 1u, range->cache_capacity);
    return range->scheduler != NULL;
}

static void range_fill_abandon(MediaHttpRange *range, const char *reason)
{
    if (range == NULL) return;
    if (range->fill_request != 0) {
        if (range_uses_background(range)) {
            (void) fetch_background_transport_cancel(
                range->fill_request, reason);
        } else {
            (void) fetch_scheduler_cancel(
                range->scheduler, range->fill_request, reason);
            (void) fetch_scheduler_discard(
                range->scheduler, range->fill_request);
        }
    }
    range->fill_request = 0;
    range->fill_length = 0;
    range->fill_admission_deferred = false;
    range->fill_streaming = false;
    range->fill_stream_slot = 0;
    range->fill_stream_headers_received = false;
    range->fill_stream_headers_admitted = false;
    range->fill_stream_received_bytes = 0;
    range->fill_streamed_bytes = 0;
    range->fill_attempts = 0;
    if (range->window_state == RANGE_WINDOW_PENDING)
        range->window_state = RANGE_WINDOW_IDLE;
}

static bool range_window_same(
    const MediaHttpRange *range, uint64_t offset, size_t length)
{
    return range != NULL && range->window_offset == offset
        && range->window_length == length;
}

static void range_window_select(
    MediaHttpRange *range, uint64_t offset, size_t length)
{
    if (range == NULL || range_window_same(range, offset, length)) return;
    if (range->fill_request != 0 || range->fill_admission_deferred)
        range_fill_abandon(range, "media range window superseded");
    range->window_offset = offset;
    range->window_length = length;
    range->window_state = RANGE_WINDOW_IDLE;
    media_http_window_tracker_reset(&range->window_tracker);
    range->fill_attempts = 0;
    range->fill_stall_exhausted = false;
    range->fill_starved_us = 0;
}

static void range_window_mark_demanded(MediaHttpRange *range)
{
    if (range == NULL) return;
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    if (range->fill_starved_us == 0) range->fill_starved_us = now_us;
    media_http_window_tracker_demand(&range->window_tracker, now_us);
}

static void range_window_fail(MediaHttpRange *range, const char *reason)
{
    if (range == NULL || range->window_state == RANGE_WINDOW_FAILED) return;
    uint64_t offset = range->window_offset;
    size_t length = range->window_length;
    range_fill_abandon(range, reason);
    range->window_state = RANGE_WINDOW_FAILED;
    range->fill_stall_exhausted = true;
    if (range->stats.stalled_reconnect_exhaustions != SIZE_MAX)
        range->stats.stalled_reconnect_exhaustions++;
    if (range->stats.failures != SIZE_MAX) range->stats.failures++;
    uint64_t last = length == 0 || length - 1u > UINT64_MAX - offset
        ? offset : offset + length - 1u;
    snprintf(range->last_error, sizeof(range->last_error),
             "range %llu-%llu delivery failed: %s",
             (unsigned long long) offset,
             (unsigned long long) last,
             reason == NULL ? "no progress" : reason);
}

/* Bytes of the outstanding window the transport has accepted so far. This is
   the only progress a refill shows before it completes, and the first-frame
   watchdog needs it to tell a slow link from a stalled one. */
static size_t range_fill_progress(const MediaHttpRange *range)
{
    if (range->fill_request == 0) return 0;
    if (range_uses_background(range)) {
        FetchBackgroundProgress progress = {0};
        return fetch_background_transport_progress(
                   range->fill_request, &progress)
            ? progress.received_body_bytes : 0;
    }
    FetchRequestProgress progress = {0};
    return fetch_scheduler_request_progress(
               range->scheduler, range->fill_request, &progress)
        ? progress.received_body_bytes : 0;
}

static bool range_fill_complete(const MediaHttpRange *range);

static bool range_stream_header_values(
    MediaHttpRange *range, long status_code, const char *effective_url,
    const char *content_length, const char *content_range,
    const char *transport_error)
{
    if (range == NULL || !range->fill_streaming
        || range->fill_length == 0) return false;
    const char *admitted_url = effective_url == NULL
        || effective_url[0] == '\0' ? range->range_url : effective_url;
    /* Preserve the status even when the streaming header gate rejects before
       terminal admission. The session's bounded 403 re-resolution policy
       depends on this classification; losing it turns an expired signed URL
       into a generic playback failure. */
    range->stats.last_http_status = status_code;
    range->stats.last_first_byte = range->fill_offset;
    range->stats.last_last_byte =
        range->fill_offset + range->fill_length - 1u;
    if ((range->url_validator != NULL
            && !range->url_validator(admitted_url))
        || (status_code != 200 && status_code != 206)) return false;
    range->fill_stream_headers_received = true;
    bool have_declared_length = content_length != NULL
        && content_length[0] != '\0';
    if (have_declared_length) {
        char *tail = NULL;
        unsigned long long declared = strtoull(content_length, &tail, 10);
        if (tail == content_length || *tail != '\0'
            || declared != range->fill_length) return false;
    }
    uint64_t complete_length = 0;
    char error[192] = {0};
    /* A 206 Content-Range proves the exact response span by itself. A 200
       query response needs Content-Length for safe pre-EOF exposure; without
       it we still stream into owned memory but wait for terminal admission. */
    if (status_code == 200 && !have_declared_length) return true;
    bool admitted = range_admit_values(
        range, range->fill_offset,
        range->fill_offset + range->fill_length - 1u,
        status_code, range->fill_length,
        content_range == NULL ? "" : content_range,
        effective_url == NULL ? "" : effective_url,
        transport_error == NULL ? "" : transport_error, true,
        &complete_length, error, sizeof(error));
    if (!admitted || complete_length != range->content_length) {
        snprintf(range->last_error, sizeof(range->last_error), "%.220s",
                 error[0] == '\0'
                     ? "streamed media range headers were not admitted"
                     : error);
        return false;
    }
    range->fill_stream_headers_admitted = true;
    return true;
}

static bool range_stream_headers(void *opaque, const FetchResult *metadata)
{
    MediaHttpRange *range = opaque;
    if (metadata == NULL) return false;
    char content_length[64] = {0};
    char content_range[128] = {0};
    (void) fetch_response_header_value(
        metadata, "content-length", content_length,
        sizeof(content_length));
    (void) fetch_response_header_value(
        metadata, "content-range", content_range, sizeof(content_range));
    return range_stream_header_values(
        range, metadata->status_code, metadata->effective_url,
        content_length, content_range, metadata->error);
}

static bool range_stream_body(
    void *opaque, const unsigned char *data, size_t length)
{
    MediaHttpRange *range = opaque;
    if (range == NULL || data == NULL || !range->fill_streaming
        || !range->fill_stream_headers_received
        || range->lookahead_slots == 0
        || range->fill_stream_received_bytes > range->fill_length
        || length > range->fill_length - range->fill_stream_received_bytes) {
        return false;
    }
    memcpy(range->lookahead[range->fill_stream_slot]
               + range->fill_stream_received_bytes,
           data, length);
    range->fill_stream_received_bytes += length;
    /* The native worker publishes bounded chunks rather than every curl
       write. Hold host visibility to the configured quantum as well so a
       deterministic replay uses the same granularity as its PSP policy. The
       terminal tail is safe to expose once the exact promised body arrived. */
    if (range->fill_stream_received_bytes == range->fill_length
        || range->fill_stream_received_bytes - range->fill_streamed_bytes
               >= range->stream_publication_bytes) {
        range->fill_streamed_bytes = range->fill_stream_received_bytes;
    }
    return true;
}

static void range_stream_drain_background(MediaHttpRange *range)
{
    if (range == NULL || !range_uses_background(range)
        || !range->fill_streaming || range->fill_request == 0) return;
    if (!range->fill_stream_headers_received) {
        FetchBackgroundMediaResponse metadata = {0};
        if (fetch_background_transport_take_media_headers(
                range->fill_request, &metadata)) {
            bool admitted = range_stream_header_values(
                range, metadata.status_code, metadata.effective_url,
                metadata.content_length, metadata.content_range,
                metadata.error);
            if (!admitted) {
                (void) fetch_background_transport_cancel(
                    range->fill_request,
                    "streamed media range headers rejected");
                return;
            }
        }
    }
    if (!range->fill_stream_headers_received) return;
    size_t available = range->fill_length - range->fill_streamed_bytes;
    if (available == 0) return;
    size_t length = 0;
    (void) fetch_background_transport_take_chunk(
        range->fill_request,
        range->lookahead[range->fill_stream_slot]
            + range->fill_streamed_bytes,
        available, &length);
    range->fill_streamed_bytes += length;
}

static void range_record_superseded(MediaHttpRange *range)
{
    if (range == NULL || range->fill_request == 0) return;
    size_t bytes = range_fill_progress(range);
    bool complete = range_fill_complete(range);
    if (range->stats.readahead_superseded != SIZE_MAX)
        range->stats.readahead_superseded++;
    if (complete
        && range->stats.completed_readahead_superseded != SIZE_MAX) {
        range->stats.completed_readahead_superseded++;
    }
    range->stats.superseded_bytes =
        bytes > SIZE_MAX - range->stats.superseded_bytes
            ? SIZE_MAX : range->stats.superseded_bytes + bytes;
}

static bool range_fill_complete(const MediaHttpRange *range)
{
    if (range_uses_background(range)) {
        FetchBackgroundProgress progress = {0};
        return range->fill_request != 0
            && fetch_background_transport_progress(
                   range->fill_request, &progress)
            && progress.complete;
    }
    FetchRequestProgress progress = {0};
    return range->fill_request != 0
        && fetch_scheduler_request_progress(
               range->scheduler, range->fill_request, &progress)
        && progress.complete;
}

static bool range_has_immediate_predecessor(const MediaHttpRange *range)
{
    if (range == NULL || range->cache_length == 0) return false;
    for (unsigned at = 0; at < range->lookahead_count; at++) {
        uint64_t offset = range->lookahead_offset[at];
        size_t length = range->lookahead_length[at];
        if (length <= UINT64_MAX - offset
            && offset + length == range->cache_offset) return true;
    }
    return false;
}

static int range_lookahead_at_offset(
    const MediaHttpRange *range, uint64_t offset)
{
    if (range == NULL) return -1;
    for (unsigned at = 0; at < range->lookahead_count; at++) {
        if (range->lookahead_length[at] != 0
            && range->lookahead_offset[at] == offset) return (int) at;
    }
    return -1;
}

/* Slot allocations exchange roles with the active cache on promotion, so
   array order is not media order. Follow the bounded offset chain instead. */
static uint64_t range_contiguous_lookahead_tail(
    const MediaHttpRange *range, unsigned *windows,
    bool members[MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS])
{
    if (windows != NULL) *windows = 0;
    if (members != NULL) {
        for (unsigned at = 0; at < MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS; at++)
            members[at] = false;
    }
    if (range == NULL || range->cache_length == 0
        || range->cache_length > UINT64_MAX - range->cache_offset) return 0;
    uint64_t cursor = range->cache_offset + range->cache_length;
    unsigned count = 0;
    while (count < range->lookahead_count) {
        int slot = range_lookahead_at_offset(range, cursor);
        if (slot < 0 || (members != NULL && members[(unsigned) slot])) break;
        size_t length = range->lookahead_length[(unsigned) slot];
        if (length > UINT64_MAX - cursor) break;
        if (members != NULL) members[(unsigned) slot] = true;
        cursor += length;
        count++;
    }
    if (windows != NULL) *windows = count;
    return cursor;
}

static RangeFillIssueStatus range_fill_issue(
    MediaHttpRange *range, uint64_t aligned, size_t wanted)
{
    range_window_select(range, aligned, wanted);
    if (range->window_state == RANGE_WINDOW_FAILED) {
        return RANGE_FILL_FAILED;
    }
    if (wanted == 0 || !range_scheduler_ready(range)) {
        snprintf(range->last_error, sizeof(range->last_error),
                 "media range transport unavailable");
        return RANGE_FILL_FAILED;
    }
    FetchRequest request;
    char error[256] = {0};
    if (!range_prepare_request(
            range, aligned, aligned + wanted - 1u, &request,
            error, sizeof(error))) {
        snprintf(range->last_error, sizeof(range->last_error),
                 "%.200s", error);
        return RANGE_FILL_FAILED;
    }
    uint64_t sequential_tail = range_contiguous_lookahead_tail(
        range, NULL, NULL);
    /* Streaming is safe because this is an unused admitted slot; retained
       successors and a lazy-demux predecessor are not overwritten. */
    bool stream = range->lookahead_slots != 0
        && range->lookahead_count < range->lookahead_slots
        && range->cache_length != 0 && aligned == sequential_tail
        && wanted <= range->cache_capacity;
    range->fill_streaming = stream;
    range->fill_stream_slot = stream ? range->lookahead_count : 0u;
    range->fill_stream_headers_received = false;
    range->fill_stream_headers_admitted = false;
    range->fill_stream_received_bytes = 0;
    range->fill_streamed_bytes = 0;
    FetchStreamOptions stream_options = {
        .on_headers = range_stream_headers,
        .on_body = range_stream_body,
        .opaque = range,
        .chunk_bytes = range->stream_publication_bytes
    };
    bool stream_shape_supported = !stream
        || fetch_background_transport_stream_shape_supported(
               range->range_url, &request, range->cache_capacity,
               range->timeout_ms, false);
    FetchBackgroundEnqueueStatus background_status =
        FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    uint64_t id = range_uses_background(range)
        ? (!stream_shape_supported ? 0 : stream
            ? fetch_background_transport_enqueue_media_stream_sized_diagnosed(
                  range->range_url, &request,
                  range->cache_capacity, range->timeout_ms,
                  range->stream_publication_bytes, &background_status)
            : fetch_background_transport_enqueue_media_diagnosed(
                  range->range_url, &request,
                  range->cache_capacity, range->timeout_ms,
                  &background_status))
        : (!stream_shape_supported ? 0 : stream
            ? fetch_scheduler_enqueue_stream(
                  range->scheduler, range->range_url, &request,
                  range->cache_capacity, range->timeout_ms,
                  &stream_options)
            : fetch_scheduler_enqueue(
                  range->scheduler, range->range_url, &request,
                  range->cache_capacity, range->timeout_ms));
    if (id == 0) {
        range->fill_streaming = false;
        range->fill_stream_headers_received = false;
        bool deferred = range_uses_background(range)
            ? background_status == FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED
                || background_status == FETCH_BACKGROUND_ENQUEUE_SATURATED
                || background_status == FETCH_BACKGROUND_ENQUEUE_MEMORY
            : stream_shape_supported
                && fetch_scheduler_enqueue_would_block(
                       range->scheduler, range->cache_capacity);
        if (deferred) {
            range->fill_admission_deferred = true;
            range->fill_offset = aligned;
            range->fill_length = wanted;
            range->window_state = RANGE_WINDOW_PENDING;
            if (range->stats.admission_deferrals != SIZE_MAX)
                range->stats.admission_deferrals++;
            return RANGE_FILL_DEFERRED;
        }
        range->fill_admission_deferred = false;
        snprintf(range->last_error, sizeof(range->last_error),
                 "range %llu-%llu was not admitted: %.140s",
                 (unsigned long long) aligned,
                 (unsigned long long) (aligned + wanted - 1u),
                 range_uses_background(range)
                     ? "background transport queue full or unavailable"
                     : fetch_scheduler_last_error(range->scheduler));
        return RANGE_FILL_FAILED;
    }
    range->fill_admission_deferred = false;
    range->fill_request = id;
    range->fill_offset = aligned;
    range->fill_length = wanted;
    range->window_state = RANGE_WINDOW_PENDING;
    range->fill_issued_us = tilefinch_platform_monotonic_time_us();
    range->fill_starved_us = 0;
    media_http_window_tracker_request_started(
        &range->window_tracker, range->fill_issued_us);
    range->stats.requests++;
    /* The second attempt at the same window is the retry the synchronous
       transport performed inside one call, counted the same way. */
    if (range->fill_attempts != 0) range->stats.retry_attempts++;
    range->fill_attempts++;
    return RANGE_FILL_ISSUED;
}

typedef struct {
    uint64_t complete_length;
    char error[256];
    size_t received;
    long new_connections;
    bool handshake_measured;
    uint64_t handshake_us;
    bool admitted;
} RangeFillTransportResult;

static TILEFINCH_OUT_OF_LINE void range_take_background_stream(
    MediaHttpRange *range, uint64_t id, uint64_t aligned, size_t wanted,
    RangeFillTransportResult *taken)
{
    FetchBackgroundMediaResponse result = {0};
    bool success = fetch_background_transport_take_media_result_consumed(
        id, &result, range->fill_streamed_bytes);
    taken->received = range->fill_streamed_bytes;
    taken->new_connections = result.new_connections;
    taken->handshake_measured = result.tls_handshake_measured;
    taken->handshake_us = result.tls_handshake_us;
    bool values_admitted = range_admit_values(
        range, aligned, aligned + wanted - 1u,
        result.status_code, taken->received, result.content_range,
        result.effective_url, result.error, success,
        &taken->complete_length, taken->error, sizeof(taken->error));
    taken->admitted = success && range->fill_stream_headers_received
        && values_admitted;
}

static TILEFINCH_OUT_OF_LINE bool range_take_scheduler_stream(
    MediaHttpRange *range, uint64_t id, uint64_t aligned, size_t wanted,
    RangeFillTransportResult *taken)
{
    bool success = false;
    FetchResult result = {.budget = range->budget};
    FetchStreamMetrics metrics = {0};
    if (!fetch_scheduler_take_stream(
            range->scheduler, id, &success, &metrics, &result)) {
        return false;
    }
    taken->received = range->fill_stream_received_bytes;
    taken->new_connections = result.new_connections;
    taken->handshake_measured = result.tls_handshake_measured;
    taken->handshake_us = result.tls_handshake_us;
    char content_range[128] = {0};
    (void) fetch_response_header_value(
        &result, "content-range", content_range, sizeof(content_range));
    bool values_admitted = range_admit_values(
        range, aligned, aligned + wanted - 1u,
        result.status_code, taken->received, content_range,
        result.effective_url, result.error, success,
        &taken->complete_length, taken->error, sizeof(taken->error));
    taken->admitted = success && range->fill_stream_headers_received
        && values_admitted;
    fetch_result_destroy(&result);
    return true;
}

static TILEFINCH_OUT_OF_LINE bool range_take_background_fixed(
    MediaHttpRange *range, uint64_t id, uint64_t aligned, size_t wanted,
    unsigned char *destination, size_t destination_capacity,
    RangeFillTransportResult *taken)
{
    FetchBackgroundMediaResponse result = {0};
    if (!fetch_background_transport_take_media(
            id, destination, destination_capacity, &result)) return false;
    taken->received = result.length;
    taken->new_connections = result.new_connections;
    taken->handshake_measured = result.tls_handshake_measured;
    taken->handshake_us = result.tls_handshake_us;
    taken->admitted = wanted != 0 && range_admit_values(
        range, aligned, aligned + wanted - 1u,
        result.status_code, result.length, result.content_range,
        result.effective_url, result.error, result.success,
        &taken->complete_length, taken->error, sizeof(taken->error));
    return true;
}

static TILEFINCH_OUT_OF_LINE bool range_take_scheduler_fixed(
    MediaHttpRange *range, uint64_t id, uint64_t aligned, size_t wanted,
    unsigned char *destination, RangeFillTransportResult *taken)
{
    bool success = false;
    FetchResult result = {.budget = range->budget};
    if (!fetch_scheduler_take(
            range->scheduler, id, &success, &result)) return false;
    taken->admitted = wanted != 0 && range_admit_response(
        range, aligned, aligned + wanted - 1u, &result, success,
        &taken->complete_length, taken->error, sizeof(taken->error));
    taken->received = result.length;
    taken->new_connections = result.new_connections;
    taken->handshake_measured = result.tls_handshake_measured;
    taken->handshake_us = result.tls_handshake_us;
    if (taken->admitted) memcpy(destination, result.data, taken->received);
    fetch_result_destroy(&result);
    return true;
}

/*
 * Move a completed window into the cache. Called only once the active window
 * has nothing the caller still needs, which is what makes overlapping the
 * refill with decoding safe. Large generic response objects remain isolated
 * in the host/custom-transport helpers; the native PSP range path carries the
 * compact media response summary on its stack.
 */
static bool range_fill_install_into(
    MediaHttpRange *range, unsigned char *destination,
    size_t destination_capacity, bool make_active, unsigned lookahead_slot)
{
    uint64_t install_started_us = tilefinch_platform_monotonic_time_us();
    uint64_t aligned = range->fill_offset;
    size_t wanted = range->fill_length;
    uint64_t id = range->fill_request;
    if (id == 0) return false;
    RangeFillTransportResult taken = {0};
    bool result_ready = true;

    if (range->fill_streaming) {
        if (range_uses_background(range)) {
            range_take_background_stream(
                range, id, aligned, wanted, &taken);
        } else {
            result_ready = range_take_scheduler_stream(
                range, id, aligned, wanted, &taken);
        }
        if (!result_ready) return false;
        range->fill_request = 0;
        range->fill_length = 0;
        range->fill_streaming = false;
        range->fill_stream_slot = 0;
        range->fill_stream_headers_received = false;
        range->fill_stream_headers_admitted = false;
        range->fill_stream_received_bytes = 0;
        range->fill_streamed_bytes = 0;
    } else if (range_uses_background(range)) {
        result_ready = range_take_background_fixed(
            range, id, aligned, wanted, destination,
            destination_capacity, &taken);
    } else {
        result_ready = range_take_scheduler_fixed(
            range, id, aligned, wanted, destination, &taken);
    }
    if (!result_ready) return false;
    range->fill_request = 0;
    range->fill_length = 0;
    if (range->window_state == RANGE_WINDOW_PENDING)
        range->window_state = RANGE_WINDOW_IDLE;
    /* A zero-length window would make the last-byte arithmetic below wrap and
       admit a zero-byte body as if it were the whole window. range_fill_issue()
       refuses to ask for one; refuse to install one too, so the wrap cannot be
       reached from any future caller. */
    if (wanted == 0) {
        media_http_error(
            taken.error, sizeof(taken.error), "empty window at %llu",
            (unsigned long long) aligned);
    }
    if (taken.admitted && (taken.received != wanted
            || taken.complete_length != range->content_length)) {
        taken.admitted = false;
        media_http_error(
            taken.error, sizeof(taken.error),
            "range %llu-%llu returned %zuB of %lluB",
            (unsigned long long) aligned,
            (unsigned long long) (aligned + wanted - 1u),
            taken.received, (unsigned long long) range->content_length);
    }
    if (!taken.admitted) {
        snprintf(range->last_error, sizeof(range->last_error),
                 "range %llu-%llu failed: %.180s",
                 (unsigned long long) aligned,
                 (unsigned long long) (aligned + wanted - 1u), taken.error);
        return false;
    }
    /* Whether this window reused the session's connection or paid a fresh
       handshake, taken from curl before the result is released. */
    if (taken.new_connections > 0)
        range->stats.window_new_connections +=
            (size_t) taken.new_connections;
    if (taken.handshake_measured) {
        range->stats.window_handshakes++;
        if (taken.handshake_us > range->stats.handshake_max_us)
            range->stats.handshake_max_us = taken.handshake_us;
    }
    range->last_error[0] = '\0';
    if (make_active) {
        range->cache_offset = aligned;
        range->cache_length = taken.received;
        range->cache_consumed = 0;
    } else {
        unsigned slot = lookahead_slot;
        if (slot >= range->lookahead_slots) return false;
        size_t replaced = slot < range->lookahead_count
            ? range->lookahead_length[slot] : 0;
        range->lookahead_offset[slot] = aligned;
        range->lookahead_length[slot] = taken.received;
        if (slot == range->lookahead_count) range->lookahead_count++;
        range->stats.lookahead_installs++;
        range->stats.lookahead_retained_bytes =
            replaced > range->stats.lookahead_retained_bytes
                ? taken.received
                : range->stats.lookahead_retained_bytes - replaced
                    + taken.received;
    }
    range->fill_attempts = 0;
    /* A window that landed clears the reconnect budget: the allowance is per
       window, so a session that trickles once does not spend the rest of its
       transfers unable to recover. */
    range->fill_stall_exhausted = false;
    media_http_window_tracker_reset(&range->window_tracker);
    range->stats.bytes_received += taken.received;
    range->stats.window_installs++;
    uint64_t install_us =
        tilefinch_platform_monotonic_time_us() - install_started_us;
    if (install_us > range->stats.install_max_us)
        range->stats.install_max_us = install_us;
    /* The window is here; close both intervals it opened. Guarded on the arm
       because the synchronous transport installs windows it never issued. */
    if (range->fill_issued_us != 0) {
        uint64_t landed_us = tilefinch_platform_monotonic_time_us();
        uint64_t pending_us = landed_us - range->fill_issued_us;
        range->stats.window_pending_samples++;
        range->stats.window_pending_total_us += pending_us;
        if (pending_us > range->stats.window_pending_max_us)
            range->stats.window_pending_max_us = pending_us;
        if (range->fill_starved_us != 0) {
            uint64_t starved_us = landed_us - range->fill_starved_us;
            range->stats.window_starved_total_us += starved_us;
            if (starved_us > range->stats.window_starved_max_us)
                range->stats.window_starved_max_us = starved_us;
        }
        range->fill_issued_us = 0;
        range->fill_starved_us = 0;
    }
    return true;
}

static bool range_fill_install(MediaHttpRange *range)
{
    return range_fill_install_into(
        range, range->cache, range->cache_capacity, true, 0);
}

static bool range_fill_install_lookahead(
    MediaHttpRange *range, bool preserve_predecessor)
{
    if (range->lookahead_slots == 0
        || range->fill_length > range->cache_capacity) return false;
    unsigned slot = range->lookahead_count;
    if (slot >= range->lookahead_slots) {
        bool successor[MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS] = {false};
        (void) range_contiguous_lookahead_tail(
            range, NULL, successor);
        for (slot = 0; slot < range->lookahead_count; slot++) {
            uint64_t offset = range->lookahead_offset[slot];
            size_t length = range->lookahead_length[slot];
            bool predecessor = preserve_predecessor
                && length <= UINT64_MAX - offset
                && offset + length == range->cache_offset;
            if (!successor[slot] && !predecessor) break;
        }
        if (slot >= range->lookahead_count) return false;
    }
    return range_fill_install_into(
        range, range->lookahead[slot], range->cache_capacity, false, slot);
}

static bool range_promote_lookahead(MediaHttpRange *range, uint64_t offset)
{
    for (unsigned slot = 0; slot < range->lookahead_count; slot++) {
        uint64_t front = range->lookahead_offset[slot];
        size_t length = range->lookahead_length[slot];
        if (offset < front) continue;
        uint64_t into = offset - front;
        if (into >= length) continue;
        unsigned char *old_cache = range->cache;
        uint64_t old_offset = range->cache_offset;
        size_t old_length = range->cache_length;
        range->cache = range->lookahead[slot];
        range->cache_offset = front;
        range->cache_length = length;
        range->cache_consumed = 0;
        range->lookahead[slot] = old_cache;
        range->lookahead_offset[slot] = old_offset;
        range->lookahead_length[slot] = old_length;
        range->stats.lookahead_promotions++;
        range->stats.lookahead_retained_bytes =
            length > range->stats.lookahead_retained_bytes
                ? old_length
                : range->stats.lookahead_retained_bytes
                    - length + old_length;
        return true;
    }
    return false;
}

/*
 * Make a completed sequential fill active without overwriting the window the
 * demux just left. With one owned auxiliary allocation and one bounded
 * transport response, the useful rotation is:
 *
 *     previous(aux), current(active), next(transport)
 *       -> current(aux), next(active)
 *
 * Installing next into the auxiliary and then swapping pointers performs that
 * rotation without copying a cache window on the browser thread. It also
 * fixes the streaming case, whose bytes already live in the auxiliary rather
 * than in `cache`.
 */
static bool range_fill_install_active(
    MediaHttpRange *range, bool *active_preserved)
{
    if (active_preserved != NULL) *active_preserved = false;
    if (range == NULL || range->lookahead_slots == 0
        || range->cache_length == 0) return range_fill_install(range);

    uint64_t cache_end = range->cache_offset + range->cache_length;
    if (cache_end < range->cache_offset) return range_fill_install(range);

    /* A low-bitrate stream can have current + completed successor while the
       following response is already complete. Advance through that owned
       successor first, so the following response can rotate into its slot. */
    int successor_slot = range_lookahead_at_offset(range, cache_end);
    if (range->fill_offset != cache_end && successor_slot >= 0
        && range->lookahead_length[(unsigned) successor_slot]
               <= UINT64_MAX - cache_end
        && range->fill_offset == cache_end
             + range->lookahead_length[(unsigned) successor_slot]) {
        if (!range_promote_lookahead(range, cache_end)) return false;
        cache_end = range->cache_offset + range->cache_length;
    }
    if (range->fill_offset != cache_end) return range_fill_install(range);

    uint64_t installed_offset = range->fill_offset;
    if (active_preserved != NULL) *active_preserved = true;
    if (!range_fill_install_lookahead(range, false)) {
        /* A native fixed-result take copies before this layer admits the
           headers. On rejection the old auxiliary bytes may therefore have
           been overwritten; never leave their old offset live. */
        range->lookahead_count = 0;
        range->stats.lookahead_retained_bytes = 0;
        return false;
    }
    if (range_promote_lookahead(range, installed_offset)) return true;
    snprintf(range->last_error, sizeof(range->last_error),
             "installed sequential media window could not be promoted");
    return false;
}

static size_t range_window_bytes(const MediaHttpRange *range, uint64_t aligned)
{
    uint64_t remaining = range->content_length - aligned;
    return remaining < range->cache_capacity
        ? (size_t) remaining : range->cache_capacity;
}

static uint64_t range_window_start(
    const MediaHttpRange *range, uint64_t offset, size_t span_length)
{
    uint64_t aligned =
        offset - offset % (uint64_t) range->cache_capacity;
    /* A non-blocking caller restarts the same logical read after WOULD_BLOCK.
       If that read straddles our arbitrary cache boundary, copying its first
       half and fetching its second creates a permanent A/B cache ping-pong:
       the retry starts at A again and evicts B. When one bounded window can
       contain the whole logical object, shift the window to the object's
       start so the retry becomes atomic. This is especially important for a
       lazy MP4 moof, whose scratch buffer is intentionally released on a
       yielded read. */
    if (span_length != 0 && span_length <= range->cache_capacity
        && offset - aligned
               > (uint64_t) (range->cache_capacity - span_length)) {
        return offset;
    }
    return aligned;
}

/*
 * Ask for the window after the active one while there is still content to
 * decode out of it. Nothing here can fail the read in progress: a refused
 * issue simply leaves the fill unstarted, and the read that reaches the end of
 * the window asks again.
 */
static void range_prefetch(MediaHttpRange *range)
{
    if (!range_uses_async_transport(range) || range->cache_length == 0) return;
    range->stats.readahead_checks++;
    /* Retire a completed sequential response into the optional owned
       lookahead, then immediately give the worker the following window. */
    unsigned contiguous_windows = 0;
    uint64_t tail_end = range_contiguous_lookahead_tail(
        range, &contiguous_windows, NULL);
    bool have_contiguous_lookahead = contiguous_windows != 0;
    bool have_contiguous_predecessor =
        range_has_immediate_predecessor(range);
    if (range->fill_request != 0 && range->fill_offset == tail_end
        && range_fill_complete(range)) {
        bool preserve_predecessor = range->lookahead_fetch_limit <= 1u;
        if (!range_fill_install_lookahead(
                range, preserve_predecessor)) return;
        tail_end = range_contiguous_lookahead_tail(
            range, &contiguous_windows, NULL);
        have_contiguous_lookahead = contiguous_windows != 0;
    }
    if (range->fill_request != 0) return;
    if (range->cache_consumed > range->cache_length) return;
    if (range->lookahead_slots != 0
        && contiguous_windows >= range->lookahead_fetch_limit) return;
    unsigned divisor = range->aggressive_readahead
        ? MEDIA_HTTP_AGGRESSIVE_READAHEAD_CONSUMED_DIVISOR
        : MEDIA_HTTP_READAHEAD_CONSUMED_DIVISOR;
    size_t consumed_threshold = range->cache_length / divisor;
    if (consumed_threshold == 0) consumed_threshold = 1;
    /* Once a sequential pipeline exists, keep its bounded slots full. A
       promotion resets cache_consumed for the new active window; applying the
       first-request threshold again there would collapse two-window depth
       back to one after every boundary. */
    if (!have_contiguous_lookahead && !have_contiguous_predecessor
        && range->cache_consumed < consumed_threshold) {
        range->stats.readahead_waiting_for_consumption++;
        return;
    }
    uint64_t next = tail_end;
    if (next >= range->content_length) return;
    if (range->cancel != NULL && range->cancel(range->cancel_opaque)) return;
    /* Preserve the attempt count when a completed prefetch response was
       rejected. Resetting it here made each replacement look like a brand-new
       window: telemetry reported zero retries and a permanently truncated CDN
       response could churn forever before the play head reached it. A true
       next window follows a successful install, which already reset this
       count; a seek to another offset starts a new retry scope. */
    if (range->fill_attempts != 0 && range->fill_offset != next)
        range->fill_attempts = 0;
    if (range->fill_attempts >= 2u) return;
    if (range_fill_issue(
            range, next, range_window_bytes(range, next))
            == RANGE_FILL_ISSUED)
        range->stats.readahead_requests++;
    else
        range->stats.readahead_issue_refusals++;
}

/*
 * Abandon a window that has stopped arriving, so the next issue can make a
 * new connection.
 *
 * Cancelling a transfer with an unfinished body is not sufficient on PSP
 * libcurl's HTTP/2 path: device telemetry showed every retry multiplexed back
 * onto the original connection. range_prepare_request therefore marks the
 * replacement CURLOPT_FRESH_CONNECT so it gets its own burst allowance rather
 * than inheriting the spent one.
 * The bytes already received are lost -- at these rates that is a few
 * kilobytes against a window that was sixteen seconds away.
 *
 * Only ever after demanded bytes stop moving, and only a bounded number of
 * times per logical window. Useful slow bytes are never discarded.
 */
static bool range_restart_stalled_fill(
    MediaHttpRange *range, const char *reason, bool starved)
{
    if (range == NULL || range->fill_request == 0) {
        return false;
    }
    if (range->window_tracker.reconnects
        >= MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS) {
        range_window_fail(range, reason);
        return false;
    }
    uint64_t offset = range->fill_offset;
    size_t length = range->fill_length;
    range_fill_abandon(range, reason);
    media_http_window_tracker_reconnected(&range->window_tracker);
    range->stats.reconnects++;
    if (starved) range->stats.starved_reconnects++;
    /* Re-issue immediately: the window is still the one the reader needs, and
       waiting adds to a delay that is already the problem. A refused issue
       leaves the fill unstarted and the ordinary paths ask again. */
    if (length == 0) {
        range_window_fail(range, "replacement range was empty");
        return false;
    }
    RangeFillIssueStatus issue = range_fill_issue(range, offset, length);
    if (issue == RANGE_FILL_FAILED) {
        char issue_error[192] = {0};
        snprintf(issue_error, sizeof(issue_error), "%.180s",
                 range->last_error[0] == '\0'
                     ? "replacement range was not admitted"
                     : range->last_error);
        range_window_fail(range, issue_error);
        return false;
    }
    return true;
}

static void range_watch_window_liveness(MediaHttpRange *range)
{
    if (range == NULL || range->window_state != RANGE_WINDOW_PENDING
        || (!range_uses_background(range) && range->scheduler == NULL)) return;
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    size_t received = range_fill_progress(range);
    bool have_request = range->fill_request != 0;
    MediaHttpWindowLivenessAction action =
        media_http_window_tracker_observe(
            &range->window_tracker, now_us, received,
            have_request && range_fill_complete(range), have_request);
    if (action == MEDIA_HTTP_WINDOW_FAIL) {
        uint64_t demanded_us = range->window_tracker.demanded
            && now_us > range->window_tracker.demanded_started_us
            ? now_us - range->window_tracker.demanded_started_us : 0;
        range_window_fail(
            range, demanded_us >= MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US
                ? "demanded window deadline expired"
                : "fresh connections made no progress");
        return;
    }
    if (action == MEDIA_HTTP_WINDOW_RECONNECT) {
        (void) range_restart_stalled_fill(
            range, "starved media range stopped making progress", true);
        return;
    }
}

/* One bounded, non-blocking step of transport progress -- measured, because
   the layer underneath it can block in ways this layer cannot see. */
static void range_pump(MediaHttpRange *range)
{
    if (range == NULL
        || (!range_uses_background(range) && range->scheduler == NULL)) return;
    if (range->fill_request == 0) {
        range_watch_window_liveness(range);
        return;
    }
    range_stream_drain_background(range);
    range_watch_window_liveness(range);
    if (range->fill_request == 0) return;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    if (!range_uses_background(range))
        (void) fetch_scheduler_pump(range->scheduler, 1u, 0u);
    else
        range_stream_drain_background(range);
    uint64_t elapsed_us =
        tilefinch_platform_monotonic_time_us() - started_us;
    range->stats.pump_calls++;
    range->stats.pump_total_us += elapsed_us;
    if (elapsed_us > range->stats.pump_max_us)
        range->stats.pump_max_us = elapsed_us;
}

static bool range_cancelled(MediaHttpRange *range, uint64_t offset)
{
    if (range->cancel == NULL || !range->cancel(range->cancel_opaque))
        return false;
    range_fill_abandon(range, "media range read cancelled");
    snprintf(range->last_error, sizeof(range->last_error),
             "range read cancelled at offset %llu",
             (unsigned long long) offset);
    range->stats.failures++;
    return true;
}

/*
 * Fill the window that holds `offset`. `may_wait` is true only for the
 * metadata reads a stream performs while it is being opened, which run inside
 * an open job that owns its own deadline; the wait is a bounded scheduler pump
 * so cancellation is still observed every ten milliseconds -- an improvement on
 * the synchronous transport, which could only reach the cancel callback from
 * libcurl's progress callback and never during connect.
 */
static MediaRangeReadStatus range_fill_window(
    MediaHttpRange *range, uint64_t offset, size_t span_length,
    bool may_wait)
{
    uint64_t aligned = range_window_start(range, offset, span_length);
    size_t wanted = range_window_bytes(range, aligned);
    if (range_promote_lookahead(range, offset)) {
        uint64_t into = offset - range->cache_offset;
        if (into < range->cache_length
            && span_length <= range->cache_length - (size_t) into) {
            range_prefetch(range);
            return MEDIA_RANGE_READ_COMPLETE;
        }
    }
    range_window_select(range, aligned, wanted);
    if (range->window_state == RANGE_WINDOW_FAILED)
        return MEDIA_RANGE_READ_FAILED;
    /* Preserve an auxiliary playback window across an out-of-order metadata
       read. It swaps back into active service when the demux returns. */
    if (!range_uses_async_transport(range)) {
        /* Substituted transports are synchronous by contract. */
        size_t received = 0;
        uint64_t complete_length = 0;
        char error[256] = {0};
        for (unsigned attempt = 0; attempt != 2u; ++attempt) {
            if (range_cancelled(range, aligned)) {
                return MEDIA_RANGE_READ_FAILED;
            }
            received = 0;
            complete_length = 0;
            error[0] = '\0';
            range->stats.requests++;
            if (attempt != 0) range->stats.retry_attempts++;
            bool transported = wanted != 0
                && range->transport(
                    range->transport_opaque, aligned, aligned + wanted - 1u,
                    range->cache, range->cache_capacity, &received,
                    &complete_length, error, sizeof(error));
            if (transported && received == wanted
                && complete_length == range->content_length) {
                range->last_error[0] = '\0';
                range->cache_offset = aligned;
                range->cache_length = received;
                range->cache_consumed = 0;
                range->stats.bytes_received += received;
                range->stats.window_installs++;
                return MEDIA_RANGE_READ_COMPLETE;
            }
            if (range->cancel != NULL
                && range->cancel(range->cancel_opaque)) break;
        }
        range->stats.failures++;
        snprintf(
            range->last_error, sizeof(range->last_error),
            "range %llu-%llu failed after retry: %.180s",
            (unsigned long long) aligned,
            (unsigned long long) (aligned + wanted - 1u),
            error[0] == '\0' ? "invalid bounded response" : error);
        return MEDIA_RANGE_READ_FAILED;
    }

    /* A fill for some other window is a mispredicted readahead -- a seek, or
       metadata read behind the play head. Retire it and ask for this one. */
    if (range->fill_request != 0 && range->fill_offset != aligned) {
        range_record_superseded(range);
        range_fill_abandon(range, "media readahead superseded");
    }
    uint64_t deadline_us = 0;
    if (may_wait) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        deadline_us = now_us
            + (uint64_t) (range->timeout_ms > 0 ? range->timeout_ms : 15000)
                * 1000u;
        /* The transaction's remaining budget outranks this read's own window
           timeout whenever it is the tighter of the two. An expired budget
           fails here rather than after one more full window wait. */
        if (range->wait_budget_armed && range->wait_deadline_us < deadline_us)
            deadline_us = range->wait_deadline_us;
    }
    for (;;) {
        if (range_cancelled(range, offset)) return MEDIA_RANGE_READ_FAILED;
        if (range->fill_request == 0) {
            if (range->fill_attempts >= 2u) {
                range->fill_attempts = 0;
                range->stats.failures++;
                return MEDIA_RANGE_READ_FAILED;
            }
            RangeFillIssueStatus issue =
                range_fill_issue(range, aligned, wanted);
            if (issue == RANGE_FILL_FAILED) {
                range->fill_attempts = 0;
                range->stats.failures++;
                return MEDIA_RANGE_READ_FAILED;
            }
        }
        range_pump(range);
        if (range_fill_complete(range)) {
            if (range_fill_install_active(range, NULL))
                return MEDIA_RANGE_READ_COMPLETE;
            /* A failed window is retried once, exactly as the synchronous
               transport did, and only then reported. */
            if (range->fill_attempts >= 2u) {
                range->fill_attempts = 0;
                range->stats.failures++;
                return MEDIA_RANGE_READ_FAILED;
            }
            continue;
        }
        if (!may_wait) {
            range->stats.would_block_reads++;
            /* The first refusal against this window opens the starved
               interval; later ones are inside it and must not restart it. */
            range_window_mark_demanded(range);
            return MEDIA_RANGE_READ_WOULD_BLOCK;
        }
        if (tilefinch_platform_monotonic_time_us() >= deadline_us) {
            bool budgeted = range->wait_budget_armed
                && range->wait_deadline_us <= deadline_us;
            range_fill_abandon(range, "media range read deadline");
            range->stats.failures++;
            if (budgeted) {
                /* Name the bound that actually fired. A caller that reads many
                   windows needs to know its whole transaction ran out, not
                   that one window did. */
                snprintf(range->last_error, sizeof(range->last_error),
                         "range %llu-%llu exceeded the transaction budget",
                         (unsigned long long) aligned,
                         (unsigned long long) (aligned + wanted - 1u));
            } else {
                snprintf(range->last_error, sizeof(range->last_error),
                         "range %llu-%llu timed out after %ldms",
                         (unsigned long long) aligned,
                         (unsigned long long) (aligned + wanted - 1u),
                         range->timeout_ms);
            }
            return MEDIA_RANGE_READ_FAILED;
        }
        if (range_uses_background(range)) {
            fetch_background_transport_wait(MEDIA_HTTP_WAIT_STEP_MS);
        } else {
            (void) fetch_scheduler_pump(
                range->scheduler, 1u, MEDIA_HTTP_WAIT_STEP_MS);
        }
        /* The same cooperative checkpoint the scheduler's own synchronous
           helper takes: the frontend's heartbeat, watchdog and cancel scope
           run here, once per bounded wait step. */
        if (!tilefinch_platform_cooperate(
                "media-range", range_fill_progress(range))) {
            range_fill_abandon(range, "media range cancelled by platform");
            range->stats.failures++;
            snprintf(range->last_error, sizeof(range->last_error),
                     "range read cancelled at offset %llu",
                     (unsigned long long) offset);
            return MEDIA_RANGE_READ_FAILED;
        }
    }
}

static MediaRangeReadStatus range_read_bounded(
    MediaHttpRange *range, uint64_t offset, void *destination, size_t length,
    bool may_wait)
{
    if (range == NULL) return MEDIA_RANGE_READ_FAILED;
    range->last_read_offset = offset;
    range->last_read_length = length;
    if (destination == NULL || offset > range->content_length
        || length > range->content_length - offset) {
        snprintf(
            range->last_error, sizeof(range->last_error),
            "invalid range read at offset %llu (%zuB/%lluB)",
            (unsigned long long) offset, length,
            (unsigned long long) range->content_length);
        range->stats.failures++;
        return MEDIA_RANGE_READ_FAILED;
    }
    unsigned char *output = destination;
    while (length != 0) {
        if (range_cancelled(range, offset)) return MEDIA_RANGE_READ_FAILED;
        (void) range_promote_lookahead(range, offset);
        bool streamed = !may_wait && range->fill_streaming
            && range->fill_stream_headers_admitted
            && range->lookahead_slots != 0
            && offset >= range->fill_offset;
        size_t stream_at = streamed
            ? (size_t) (offset - range->fill_offset) : 0;
        size_t stream_available = streamed
            && stream_at <= range->fill_streamed_bytes
            ? range->fill_streamed_bytes - stream_at : 0;
        if (streamed && length <= stream_available) {
            memcpy(output, range->lookahead[range->fill_stream_slot]
                       + stream_at, length);
            range->stats.cache_hits++;
            if (range->stats.streaming_partial_reads != SIZE_MAX)
                range->stats.streaming_partial_reads++;
            range->stats.streaming_partial_bytes =
                length > SIZE_MAX - range->stats.streaming_partial_bytes
                    ? SIZE_MAX
                    : range->stats.streaming_partial_bytes + length;
            output += length;
            offset += length;
            length = 0;
            continue;
        }
        bool cached = range->cache_length != 0
            && offset >= range->cache_offset
            && offset - range->cache_offset < range->cache_length;
        size_t cache_at = cached
            ? (size_t) (offset - range->cache_offset) : 0;
        size_t available = cached && cache_at <= range->cache_length
            ? range->cache_length - cache_at : 0;
        bool split_streaming = !may_wait && cached && length > available
            && range->fill_streaming
            && range->fill_stream_headers_admitted
            && range->cache_offset <= UINT64_MAX - range->cache_length
            && range->fill_offset
                 == range->cache_offset + range->cache_length
            && length - available <= range->fill_streamed_bytes;
        if (split_streaming) {
            size_t prefix = available;
            memcpy(output, range->cache + cache_at, prefix);
            memcpy(output + prefix,
                   range->lookahead[range->fill_stream_slot],
                   length - prefix);
            range->stats.cache_hits++;
            if (range->stats.streaming_partial_reads != SIZE_MAX)
                range->stats.streaming_partial_reads++;
            size_t streamed_bytes = length - prefix;
            range->stats.streaming_partial_bytes =
                streamed_bytes
                    > SIZE_MAX - range->stats.streaming_partial_bytes
                    ? SIZE_MAX
                    : range->stats.streaming_partial_bytes + streamed_bytes;
            output += length;
            offset += length;
            length = 0;
            continue;
        }
        /* The optional owned successor is already stable memory, so bridge a
           boundary-spanning logical sample before considering the on-wire
           successor. This preserves the poll contract: one call either
           copies the complete sample or no bytes at all. */
        uint64_t successor_offset = range->cache_offset
               <= UINT64_MAX - range->cache_length
            ? range->cache_offset + range->cache_length : 0;
        int successor_slot = range_lookahead_at_offset(
            range, successor_offset);
        bool split_lookahead = !may_wait && cached
            && length > available && range->lookahead_count != 0
            && range->cache_offset <= UINT64_MAX - range->cache_length
            && successor_slot >= 0
            && length - available
                 <= range->lookahead_length[(unsigned) successor_slot];
        if (split_lookahead) {
            size_t prefix = available;
            memcpy(output, range->cache + cache_at, prefix);
            if (!range_promote_lookahead(range, successor_offset)) {
                snprintf(range->last_error, sizeof(range->last_error),
                         "owned sequential window could not be promoted");
                range->stats.failures++;
                return MEDIA_RANGE_READ_FAILED;
            }
            size_t suffix = length - prefix;
            memcpy(output + prefix, range->cache, suffix);
            range->cache_consumed = suffix;
            if (range->stats.split_window_bridges != SIZE_MAX)
                range->stats.split_window_bridges++;
            range->stats.cache_hits++;
            output += length;
            offset += length;
            length = 0;
            continue;
        }
        /*
         * Preserve a completed sequential prefetch when one logical MP4
         * sample or moof straddles the arbitrary cache boundary. The old path
         * rejected the useful prefix, shifted the requested window to
         * `offset`, and discarded an already-complete successor before
         * downloading almost the same bytes again.
         *
         * Do not copy the prefix while the successor is incomplete: a
         * non-blocking caller retries the whole logical read and has no way to
         * retain partial progress. Once it is complete, copying the prefix,
         * installing the successor, and copying the suffix is one successful
         * call and needs no third buffer.
         */
        bool split_sequential = !may_wait && cached
            && length <= range->cache_capacity && length > available
            && range->fill_request != 0
            && range->cache_offset <= UINT64_MAX - range->cache_length
            && range->fill_offset
                 == range->cache_offset + range->cache_length;
        if (split_sequential) {
            range_pump(range);
            if (!range_fill_complete(range)) {
                range->stats.would_block_reads++;
                range_window_mark_demanded(range);
                return MEDIA_RANGE_READ_WOULD_BLOCK;
            }
            size_t prefix = available;
            memcpy(output, range->cache + cache_at, prefix);
            uint64_t failed_offset = range->fill_offset;
            size_t failed_length = range->fill_length;
            bool active_preserved = false;
            if (!range_fill_install_active(range, &active_preserved)) {
                /* The background take may already have overwritten the
                   active bytes before response admission rejected it. The
                   caller ignores its destination on WOULD_BLOCK, so retire
                   that cache and preserve the ordinary one-retry contract
                   with the shifted window which contains the whole span. */
                if (!active_preserved) {
                    range->cache_length = 0;
                    range->cache_consumed = 0;
                }
                uint64_t retry_offset =
                    range_window_start(range, offset, length);
                size_t retry_length =
                    range_window_bytes(range, retry_offset);
                /* The shifted request is not another attempt at the failed
                   aligned window. Give this exact byte span its own bounded
                   two-attempt scope. Otherwise a first-attempt connection
                   loss on both distinct URLs exhausts the inherited count
                   before the shifted request can retry. */
                if (retry_offset != failed_offset
                    || retry_length != failed_length) {
                    range->fill_attempts = 0;
                }
                RangeFillIssueStatus issue = range->fill_attempts >= 2u
                    ? RANGE_FILL_FAILED
                    : range_fill_issue(
                          range, retry_offset, retry_length);
                if (issue == RANGE_FILL_FAILED) {
                    range->fill_attempts = 0;
                    range->stats.failures++;
                    return MEDIA_RANGE_READ_FAILED;
                }
                range->stats.would_block_reads++;
                range_window_mark_demanded(range);
                return MEDIA_RANGE_READ_WOULD_BLOCK;
            }
            size_t suffix = length - prefix;
            if (suffix > range->cache_length) {
                snprintf(range->last_error, sizeof(range->last_error),
                         "sequential window bridge is short: %zu/%zuB",
                         range->cache_length, suffix);
                range->stats.failures++;
                return MEDIA_RANGE_READ_FAILED;
            }
            memcpy(output + prefix, range->cache, suffix);
            range->cache_consumed = suffix;
            if (range->stats.split_window_bridges != SIZE_MAX)
                range->stats.split_window_bridges++;
            range->stats.cache_hits++;
            output += length;
            offset += length;
            length = 0;
            continue;
        }
        /* For a logical read which fits one cache window, partial residency
           is not useful: returning WOULD_BLOCK after copying a prefix loses
           that progress when the caller retries. Force one shifted window
           that contains the complete span instead. Larger reads retain the
           historical bounded multi-window blocking path. */
        if (cached && length <= range->cache_capacity) {
            uint64_t cache_at = offset - range->cache_offset;
            cached = cache_at <= range->cache_length
                && length <= range->cache_length - (size_t) cache_at;
        }
        if (!cached) {
            MediaRangeReadStatus status =
                range_fill_window(range, offset, length, may_wait);
            if (status != MEDIA_RANGE_READ_COMPLETE) return status;
        } else {
            range->stats.cache_hits++;
        }
        cache_at = (size_t) (offset - range->cache_offset);
        if (cache_at >= range->cache_length) {
            /* The window that was just installed does not contain the byte it
               was fetched for. Nothing outside this file can diagnose that, and
               a bare false told the demuxer only "read failed at 0 (8B)" with
               no detail and no failure counted -- which is how a zeroed
               cache_length cost a device cycle. Say it. */
            snprintf(
                range->last_error, sizeof(range->last_error),
                "window %llu+%zuB does not hold offset %llu",
                (unsigned long long) range->cache_offset,
                range->cache_length, (unsigned long long) offset);
            range->stats.failures++;
            return MEDIA_RANGE_READ_FAILED;
        }
        available = range->cache_length - cache_at;
        size_t copied = length < available ? length : available;
        memcpy(output, range->cache + cache_at, copied);
        if (cache_at + copied > range->cache_consumed)
            range->cache_consumed = cache_at + copied;
        output += copied;
        offset += copied;
        length -= copied;
    }
    /* Decide the refill from what the window has left, not from having run out
       of it: the request has to be on the wire before the decoder arrives. */
    range_prefetch(range);
    return MEDIA_RANGE_READ_COMPLETE;
}

static bool range_read(void *opaque, uint64_t offset,
                       void *destination, size_t length)
{
    return range_read_bounded(opaque, offset, destination, length, true)
        == MEDIA_RANGE_READ_COMPLETE;
}

static MediaRangeReadStatus range_read_poll(
    void *opaque, uint64_t offset, void *destination, size_t length)
{
    return range_read_bounded(opaque, offset, destination, length, false);
}

static bool range_resident(void *opaque, uint64_t offset, size_t length)
{
    return media_http_range_resident(opaque, offset, length);
}

static bool range_describe_failure(void *opaque, char *error,
                                   size_t error_size)
{
    MediaHttpRange *range = opaque;
    if (range == NULL || error == NULL || error_size == 0
        || range->last_error[0] == '\0') {
        return false;
    }
    snprintf(error, error_size, "%s", range->last_error);
    return true;
}

MediaHttpRange *media_http_range_create(
    Budget *budget, BrowserSession *session, const char *url,
    uint64_t content_length, const MediaHttpRangeOptions *options,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || url == NULL || url[0] == '\0'
        || content_length == 0
        || (options != NULL && options->url_validator != NULL
            && !options->url_validator(url))) {
        media_http_error(error, error_size, "media: invalid HTTP");
        return NULL;
    }
    size_t cache_bytes = options == NULL || options->cache_bytes == 0
        ? MEDIA_HTTP_DEFAULT_CACHE_BYTES : options->cache_bytes;
    if (cache_bytes < MEDIA_HTTP_MINIMUM_CACHE_BYTES
        || cache_bytes > MEDIA_HTTP_MAXIMUM_CACHE_BYTES) {
        media_http_error(error, error_size,
                         "media cache outside 4-256 KiB");
        return NULL;
    }
    MediaHttpRange *range = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*range));
    if (range == NULL) {
        media_http_error(error, error_size,
                         "media HTTP state budget");
        return NULL;
    }
    range->budget = budget;
    size_t url_length = strlen(url) + 1u;
    if (url_length > SIZE_MAX - 64u) {
        media_http_range_destroy(range);
        media_http_error(error, error_size, "media URL exceeded bound");
        return NULL;
    }
    size_t range_url_capacity = url_length + 64u;
    const char *referer = options == NULL || options->referer == NULL
        ? "" : options->referer;
    size_t referer_length = strlen(referer) + 1u;
    range->url = budget_malloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, url_length);
    range->range_url = budget_malloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, range_url_capacity);
    range->referer = budget_malloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, referer_length);
    range->cache = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, cache_bytes);
    if (range->url == NULL || range->range_url == NULL
        || range->referer == NULL
        || range->cache == NULL) {
        media_http_range_destroy(range);
        media_http_error(error, error_size,
                         "media HTTP cache budget");
        return NULL;
    }
    memcpy(range->url, url, url_length);
    range->range_url_capacity = range_url_capacity;
    memcpy(range->referer, referer, referer_length);
    range->standard_range_header = options != NULL
        && options->standard_range_header;
    if (options != NULL && options->page_request_context != NULL) {
        range->prepared_request = budget_malloc_category(
            budget, BUDGET_CATEGORY_NAVIGATION,
            sizeof(*range->prepared_request));
        FetchRequest transport = {
            .allow_http_errors = true,
            .accept = "video/mp4,video/*;q=0.9,*/*;q=0.5",
            .user_agent = MEDIA_HTTP_USER_AGENT,
            .connect_timeout_ms = range->connect_timeout_ms,
            .redirect_same_origin_only = true
        };
        FetchRequestValidationError request_error;
        const char *referrer_source = referer[0] == '\0'
            ? options->page_request_context->initiator_url : referer;
        if (range->prepared_request == NULL
            || !fetch_prepare_page_request_context(
                   options->page_request_context, referrer_source,
                   options->referrer_policy, session,
                   options->content_security_policy,
                   session == NULL ? NULL : session->content_blocker,
                   &transport, range->prepared_request, &request_error)) {
            media_http_range_destroy(range);
            media_http_error(error, error_size,
                             "media request authority refused");
            return NULL;
        }
    }
    range->session = session;
    range->cache_capacity = cache_bytes;
    range->stream_publication_bytes = options == NULL
        || options->stream_publication_bytes == 0
        ? 16u * 1024u : options->stream_publication_bytes;
    if (range->stream_publication_bytes > cache_bytes)
        range->stream_publication_bytes = cache_bytes;
    unsigned lookahead_windows = options == NULL
        ? 0 : options->lookahead_windows;
    if (lookahead_windows > MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS)
        lookahead_windows = MEDIA_HTTP_MAXIMUM_LOOKAHEAD_WINDOWS;
    range->lookahead_capacity = lookahead_windows;
    unsigned initial_lookahead = options == NULL
        ? 0 : options->initial_lookahead_windows;
    unsigned initial_allocations = initial_lookahead == 0
        ? lookahead_windows : initial_lookahead;
    if (initial_allocations > lookahead_windows)
        initial_allocations = lookahead_windows;
    /* Optional by design: stop at the first refused buffer and retain the
       functional active+in-flight path with however many were admitted. */
    for (unsigned at = 0; at < initial_allocations; at++) {
        range->lookahead[at] = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, cache_bytes);
        if (range->lookahead[at] == NULL) break;
        range->lookahead_slots++;
    }
    range->lookahead_fetch_limit = initial_lookahead == 0
        ? range->lookahead_slots : initial_lookahead;
    if (range->lookahead_fetch_limit > range->lookahead_slots)
        range->lookahead_fetch_limit = range->lookahead_slots;
    range->content_length = content_length;
    range->timeout_ms = options == NULL || options->timeout_ms <= 0
        ? 15000 : options->timeout_ms;
    range->connect_timeout_ms = options == NULL
        ? 0 : options->connect_timeout_ms;
    range->minimum_sustained_bytes_per_second = options == NULL
        || options->minimum_sustained_bytes_per_second
               < MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND
        ? MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND
        : options->minimum_sustained_bytes_per_second;
    range->stats.minimum_sustained_bytes_per_second =
        range->minimum_sustained_bytes_per_second;
    range->transport = options == NULL ? NULL : options->transport;
    range->transport_opaque = options == NULL
        ? NULL : options->transport_opaque;
    range->url_validator = options == NULL
        ? NULL : options->url_validator;
    range->cancel = options == NULL ? NULL : options->cancel;
    range->cancel_opaque = options == NULL
        ? NULL : options->cancel_opaque;
    range->use_background_transport = range->transport == NULL
        && fetch_background_transport_available();
    range->stats.retained_bytes =
        sizeof(*range) + url_length + range_url_capacity
        + referer_length + cache_bytes;
    if (range->prepared_request != NULL)
        range->stats.retained_bytes += sizeof(*range->prepared_request);
    range->stats.retained_bytes +=
        (size_t) range->lookahead_slots * cache_bytes;
    return range;
}

MediaRangeReader media_http_range_reader(MediaHttpRange *range)
{
    return (MediaRangeReader) {
        .opaque = range,
        .length = range == NULL ? 0 : range->content_length,
        .read = range_read,
        .poll = range_read_poll,
        .resident = range_resident,
        .describe_failure = range_describe_failure
    };
}

/*
 * The whole span, in the window that is loaded right now.
 *
 * The first byte is not the question. Windows are aligned to the cache
 * capacity and range_read_bounded refills per window, so a sample straddling a
 * boundary blocks even though its first byte is cached -- and a predicate that
 * checked only the first byte would report exactly the false positives it
 * exists to rule out.
 *
 * A transport-substituted range fills synchronously inside the read and can
 * never answer WOULD_BLOCK, so for that shape the honest answer is yes
 * whatever the window happens to hold. Host fixtures are all that shape; the
 * device is the other one.
 */
bool media_http_range_resident(const MediaHttpRange *range,
                               uint64_t offset, size_t length)
{
    if (range == NULL) return false;
    if (length == 0) return true;
    if (!range_uses_async_transport(range)) return true;
    if (range->cache_length != 0 && offset >= range->cache_offset) {
        uint64_t into = offset - range->cache_offset;
        if (into < range->cache_length
            && length <= range->cache_length - (size_t) into) return true;
        uint64_t cache_end = range->cache_offset + range->cache_length;
        if (into < range->cache_length && length > 0) {
            uint64_t cursor = cache_end;
            size_t remaining = length - (length < range->cache_length - (size_t) into
                ? length : range->cache_length - (size_t) into);
            for (unsigned at = 0; remaining != 0
                 && at < range->lookahead_count; at++) {
                int slot = range_lookahead_at_offset(range, cursor);
                if (slot < 0) break;
                size_t held = range->lookahead_length[(unsigned) slot];
                if (remaining <= held) return true;
                remaining -= held;
                cursor += held;
            }
        }
    }
    for (unsigned at = 0; at < range->lookahead_count; at++) {
        if (offset < range->lookahead_offset[at]) continue;
        uint64_t into = offset - range->lookahead_offset[at];
        if (into < range->lookahead_length[at]
            && length <= range->lookahead_length[at] - (size_t) into)
            return true;
    }
    if (range->fill_streaming && range->fill_stream_headers_admitted
        && range->lookahead_slots != 0 && offset >= range->fill_offset) {
        uint64_t into = offset - range->fill_offset;
        if (into < range->fill_streamed_bytes
            && length <= range->fill_streamed_bytes - (size_t) into)
            return true;
    }
    return false;
}

bool media_http_range_pump(MediaHttpRange *range)
{
    if (range == NULL || !range_uses_async_transport(range)) return false;
    /* Start the next window even in frames where nothing was read: a decoder
       working through buffered content is exactly when the refill should run. */
    range_prefetch(range);
    range_pump(range);
    return range->window_state == RANGE_WINDOW_PENDING;
}

MediaHttpRangePrimeStatus media_http_range_prime_successor(
    MediaHttpRange *range)
{
    if (range == NULL || range->successor_prime_failed)
        return MEDIA_HTTP_RANGE_PRIME_FAILED;
    if (range->successor_prime_complete)
        return MEDIA_HTTP_RANGE_PRIME_READY;
    if (range->content_length <= range->cache_capacity) {
        range->successor_prime_complete = true;
        return MEDIA_HTTP_RANGE_PRIME_READY;
    }

    /* A metadata read beyond the prefix has already proved the property. */
    if (range->cache_length != 0 && range->cache_offset != 0) {
        range->successor_prime_complete = true;
        return MEDIA_HTTP_RANGE_PRIME_READY;
    }
    for (unsigned at = 0; at < range->lookahead_count; at++) {
        if (range->lookahead_length[at] != 0
            && range->lookahead_offset[at] != 0) {
            range->successor_prime_complete = true;
            return MEDIA_HTTP_RANGE_PRIME_READY;
        }
    }
    if (range->cache_length == 0) return MEDIA_HTTP_RANGE_PRIME_PENDING;

    /* Substituted host transports are synchronous by contract. The shipping
       PSP path below remains one bounded worker pump per call. */
    uint64_t successor = range->cache_capacity;
    if (!range_uses_async_transport(range)) {
        unsigned char byte = 0;
        MediaRangeReadStatus status = range_read_bounded(
            range, successor, &byte, 1u, true);
        range->successor_prime_complete =
            status == MEDIA_RANGE_READ_COMPLETE;
        range->successor_prime_failed =
            status == MEDIA_RANGE_READ_FAILED;
        return range->successor_prime_complete
            ? MEDIA_HTTP_RANGE_PRIME_READY
            : MEDIA_HTTP_RANGE_PRIME_FAILED;
    }

    size_t wanted = range_window_bytes(range, successor);
    if (range->fill_request != 0 && range->fill_offset != successor) {
        range_pump(range);
        return MEDIA_HTTP_RANGE_PRIME_PENDING;
    }
    if (range->fill_request == 0) {
        /* Two completed but rejected responses prove that this candidate
           cannot deliver its successor. Admission deferral is different: it
           increments no attempt count and receives the demanded-window
           deadline below. */
        if (range->fill_attempts >= 2u) {
            range->successor_prime_failed = true;
            range->stats.failures++;
            return MEDIA_HTTP_RANGE_PRIME_FAILED;
        }
        RangeFillIssueStatus issue =
            range_fill_issue(range, successor, wanted);
        if (issue == RANGE_FILL_FAILED) {
            range->successor_prime_failed = true;
            range->stats.failures++;
            return MEDIA_HTTP_RANGE_PRIME_FAILED;
        }
    }
    /* Unlike speculative read-ahead, startup prime is an explicit dependency
       of the open transaction. Arm the logical-window deadline even when no
       descriptor could be admitted, so a saturated/leaked transport cannot
       leave the player at a permanent loading spinner. */
    range_window_mark_demanded(range);
    range_pump(range);
    if (range->window_state == RANGE_WINDOW_FAILED) {
        range->successor_prime_failed = true;
        return MEDIA_HTTP_RANGE_PRIME_FAILED;
    }
    if (!range_fill_complete(range)) return MEDIA_HTTP_RANGE_PRIME_PENDING;
    if (range_fill_install_lookahead(range, true)) {
        range->successor_prime_complete = true;
        return MEDIA_HTTP_RANGE_PRIME_READY;
    }
    if (range->fill_attempts < 2u)
        return MEDIA_HTTP_RANGE_PRIME_PENDING;
    range->successor_prime_failed = true;
    range->stats.failures++;
    return MEDIA_HTTP_RANGE_PRIME_FAILED;
}

void media_http_range_set_aggressive_readahead(
    MediaHttpRange *range, bool enabled)
{
    if (range == NULL) return;
    range->aggressive_readahead = enabled;
    if (enabled) range_prefetch(range);
}

void media_http_range_set_lookahead_limit(
    MediaHttpRange *range, unsigned windows)
{
    if (range == NULL || range->lookahead_capacity == 0) return;
    if (windows == 0) windows = 1;
    if (windows > range->lookahead_capacity)
        windows = range->lookahead_capacity;
    while (range->lookahead_slots < windows) {
        unsigned slot = range->lookahead_slots;
        unsigned char *buffer = budget_malloc_category(
            range->budget, BUDGET_CATEGORY_RESOURCE,
            range->cache_capacity);
        if (buffer == NULL) break;
        range->lookahead[slot] = buffer;
        range->lookahead_slots++;
        range->stats.retained_bytes += range->cache_capacity;
    }
    if (windows > range->lookahead_slots) windows = range->lookahead_slots;
    bool increased = windows > range->lookahead_fetch_limit;
    range->lookahead_fetch_limit = windows;
    if (increased) range_prefetch(range);
}

void media_http_range_set_wait_budget_us(
    MediaHttpRange *range, uint64_t budget_us)
{
    if (range == NULL) return;
    range->wait_budget_armed = true;
    range->wait_deadline_us =
        tilefinch_platform_monotonic_time_us() + budget_us;
}

void media_http_range_clear_wait_budget(MediaHttpRange *range)
{
    if (range == NULL) return;
    range->wait_budget_armed = false;
    range->wait_deadline_us = 0;
}

bool media_http_range_stats(const MediaHttpRange *range,
                            MediaHttpRangeStats *stats)
{
    if (range == NULL || stats == NULL) return false;
    *stats = range->stats;
    stats->bytes_in_flight = range_fill_progress(range);
    stats->window_pending =
        range->window_state == RANGE_WINDOW_PENDING;
    stats->admission_deferred = range->fill_admission_deferred;
    stats->delivery_stalled = range->fill_stall_exhausted;
    stats->cache_offset = range->cache_offset;
    stats->cache_length = range->cache_length;
    stats->cache_consumed = range->cache_consumed;
    stats->lookahead_slots = range->lookahead_slots;
    stats->lookahead_fetch_limit = range->lookahead_fetch_limit;
    stats->aggressive_readahead = range->aggressive_readahead;
    stats->fill_offset = range->window_state == RANGE_WINDOW_FAILED
        ? range->window_offset : range->fill_offset;
    stats->fill_length = range->window_state == RANGE_WINDOW_FAILED
        ? range->window_length : range->fill_length;
    stats->last_read_offset = range->last_read_offset;
    stats->last_read_length = range->last_read_length;
    return true;
}

uint64_t media_http_range_buffered_ahead_us(
    const MediaHttpRange *range, uint64_t duration_us)
{
    if (range == NULL || duration_us == 0 || range->content_length == 0
        || range->cache_length == 0) return 0;
    uint64_t cache_end = range->cache_offset + range->cache_length;
    if (cache_end < range->cache_offset) return 0;
    uint64_t read_end = range->last_read_offset;
    if (range->last_read_length > UINT64_MAX - read_end)
        read_end = UINT64_MAX;
    else
        read_end += range->last_read_length;
    if (read_end < range->cache_offset) read_end = range->cache_offset;
    uint64_t unread = read_end < cache_end ? cache_end - read_end : 0;
    for (unsigned at = 0; at < range->lookahead_count; at++) {
        int slot = range_lookahead_at_offset(range, cache_end);
        if (slot < 0) break;
        size_t held = range->lookahead_length[(unsigned) slot];
        if (held > UINT64_MAX - unread) break;
        unread += held;
        cache_end += held;
    }
    if (range->fill_streaming && range->fill_stream_headers_admitted
        && range->fill_offset == cache_end
        && range->fill_streamed_bytes <= UINT64_MAX - unread) {
        unread += range->fill_streamed_bytes;
        cache_end += range->fill_streamed_bytes;
    }
    /* A completed sequential readahead still lives in the scheduler response
       buffer until the demux crosses the active-window boundary. Count it:
       it is already resident RAM and becomes the active cache without another
       network wait. Not counting it made a paused decoder unable to prove that
       its own refill had completed. Partial/on-wire data is deliberately not
       credited. */
    if (!range->fill_streaming && range->fill_request != 0
        && range->fill_offset == cache_end
        && range_fill_complete(range)) {
        size_t complete = range_fill_progress(range);
        if (complete >= range->fill_length
            && complete <= UINT64_MAX - unread)
            unread += complete;
    }
    if (unread == 0) return 0;
    /* Keep 20% in reserve for VBR bursts and MP4 box overhead, so a nominal
       two seconds is not resumed on optimistic average-rate arithmetic. */
    if (duration_us <= UINT32_MAX && range->content_length <= UINT32_MAX
        && unread <= UINT32_MAX) {
        /* Buffering policy needs a conservative duration, not sample-clock
           precision. Measuring bytes in KiB and duration in milliseconds
           keeps ordinary web video on the PSP's 32-bit divider. Flooring the
           numerator units and ceiling the content length can only delay a
           resume; it can never claim bytes which are not resident. */
        uint32_t unread_kib = (uint32_t) unread / 1024u;
        uint32_t content = (uint32_t) range->content_length;
        uint32_t content_kib = content / 1024u
            + (content % 1024u != 0);
        uint32_t duration_ms = (uint32_t) duration_us / 1000u;
        if (unread_kib != 0 && content_kib != 0 && duration_ms != 0
            && unread_kib <= UINT32_MAX / duration_ms) {
            uint32_t estimated_ms = unread_kib * duration_ms / content_kib;
            return (uint64_t) estimated_ms * 800u;
        }
    }
    if (duration_us > UINT64_MAX / unread) return 0;
    uint64_t estimated = unread * duration_us / range->content_length;
    return estimated - estimated / 5u;
}

void media_http_range_destroy(MediaHttpRange *range)
{
    if (range == NULL) return;
    Budget *budget = range->budget;
    range_fill_abandon(range, "media range closed");
    if (range->use_background_transport) {
        if (range->fill_request != 0)
            (void) fetch_background_transport_cancel(
                range->fill_request, "media range destroyed");
    } else {
        fetch_scheduler_destroy(range->scheduler);
    }
    budget_free(budget, range->cache);
    for (unsigned at = 0; at < range->lookahead_slots; at++)
        budget_free(budget, range->lookahead[at]);
    budget_free(budget, range->prepared_request);
    budget_free(budget, range->referer);
    budget_free(budget, range->range_url);
    budget_free(budget, range->url);
    memset(range, 0, sizeof(*range));
    budget_free(budget, range);
}
