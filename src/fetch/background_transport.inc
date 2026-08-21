/*
 * PSP transport worker.
 *
 * FetchScheduler deliberately remains a browser-thread object: its response
 * callbacks allocate from, and its completion path mutates, the intrusive
 * page Budget ledger.  Moving only curl_multi_perform to a thread would race
 * that ledger and the scheduler itself.  This lower lane instead deep-copies
 * the small immutable request shape into six fixed descriptors. Streaming
 * requests grow a pool of up to six 64 KiB buffers as concurrency requires;
 * two 256 KiB fixed-response media buffers are likewise allocated lazily on
 * the owning thread. The worker
 * touches curl, atomics and those buffers only -- never Budget, DOM, UI,
 * profile state, logging or the filesystem.
 */

#define FETCH_BACKGROUND_URL_CAPACITY 4160u
#define FETCH_BACKGROUND_REFERER_CAPACITY 2048u
#define FETCH_BACKGROUND_BODY_CAPACITY 8192u
#define FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY 2048u
#define FETCH_BACKGROUND_COOKIE_CAPACITY 4096u
#define FETCH_BACKGROUND_ORIGIN_CAPACITY 2048u
#define FETCH_BACKGROUND_ACCEPT_CAPACITY 512u
#define FETCH_BACKGROUND_USER_AGENT_CAPACITY 512u
#define FETCH_BACKGROUND_CONTENT_TYPE_CAPACITY 128u
#define FETCH_BACKGROUND_METHOD_CAPACITY 16u
#define FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY 64u

#include "background_slot_policy.h"

bool fetch_background_transport_stream_shape_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, bool single_hop)
{
    const char *method = request == NULL || request->method == NULL
        ? "GET" : request->method;
#define FETCH_BACKGROUND_FIELD_FITS(value, capacity) \
    ((value) == NULL || strlen(value) < (capacity))
    bool supported = url != NULL && request != NULL && maximum_bytes != 0
        && maximum_bytes <= UINT_MAX && timeout_ms > 0
        && fetch_request_validate(request, NULL)
        && strlen(method) < FETCH_BACKGROUND_METHOD_CAPACITY
        && (single_hop || strcasecmp(method, "GET") == 0
            || strcasecmp(method, "POST") == 0)
        && request->body_length < FETCH_BACKGROUND_BODY_CAPACITY
        && (request->body_length == 0 || request->body != NULL)
        && (single_hop
            || (request->redirect_url_validator != NULL
                && request->redirect_url_validator(url)
                && request->redirect_same_origin_only))
        && !request->enforce_cors
        && request->page_context == NULL
        && request->content_security_policy == NULL
        && request->content_blocker == NULL
        && request->cookie_session == NULL
        && request->cookie_context == NULL
        && FETCH_BACKGROUND_FIELD_FITS(
               url, FETCH_BACKGROUND_URL_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->content_type,
               FETCH_BACKGROUND_CONTENT_TYPE_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->extra_headers,
               FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->cookie, FETCH_BACKGROUND_COOKIE_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->referer, FETCH_BACKGROUND_REFERER_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->origin, FETCH_BACKGROUND_ORIGIN_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->accept, FETCH_BACKGROUND_ACCEPT_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->user_agent,
               FETCH_BACKGROUND_USER_AGENT_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(request->if_none_match, 192u)
        && FETCH_BACKGROUND_FIELD_FITS(request->if_modified_since, 128u)
        && FETCH_BACKGROUND_FIELD_FITS(request->client_hint_tokens, 1024u)
        && FETCH_BACKGROUND_FIELD_FITS(request->client_hint_origin, 2048u)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->sec_fetch_dest,
               FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->sec_fetch_mode,
               FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY)
        && FETCH_BACKGROUND_FIELD_FITS(
               request->sec_fetch_site,
               FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY);
#undef FETCH_BACKGROUND_FIELD_FITS
    return supported;
}

#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)

#define FETCH_BACKGROUND_EVENT_WAKE 0x1u
/* libcurl may deliver another CURL_MAX_WRITE_SIZE (normally 16 KiB) body
   callback before observing the published pause state. Hand the current
   chunk to the browser with one such callback of spare capacity. */
#define FETCH_BACKGROUND_STREAM_CHUNK_TARGET (48u * 1024u)
#define FETCH_BACKGROUND_THREAD_STACK_BYTES (256u * 1024u)
/*
 * One notch above the 0x20 browser thread. A lower-priority curl worker was
 * repeatedly preempted at every vblank and could accumulate only 220--370 KiB
 * over a ten-second media request. curl_multi_poll() blocks for up to 10 ms
 * whenever no CPU work is ready, so this priority grants prompt TLS/body
 * progress without turning the worker into a spin loop or moving browser
 * callbacks onto it.
 */
#define FETCH_BACKGROUND_POLL_MS 10u
#include "tilefinch/user_agent.h"
#define FETCH_BACKGROUND_USER_AGENT TILEFINCH_BROWSER_USER_AGENT

typedef enum {
    FETCH_BACKGROUND_SLOT_FREE = 0,
    FETCH_BACKGROUND_SLOT_PREPARING,
    FETCH_BACKGROUND_SLOT_QUEUED,
    FETCH_BACKGROUND_SLOT_RUNNING,
    FETCH_BACKGROUND_SLOT_COMPLETE,
    FETCH_BACKGROUND_SLOT_TAKING
} FetchBackgroundSlotState;

typedef enum {
    FETCH_BACKGROUND_CHUNK_EMPTY = 0,
    FETCH_BACKGROUND_CHUNK_READY,
    FETCH_BACKGROUND_CHUNK_TAKING
} FetchBackgroundChunkState;

typedef enum {
    FETCH_BACKGROUND_HEADERS_EMPTY = 0,
    FETCH_BACKGROUND_HEADERS_READY,
    FETCH_BACKGROUND_HEADERS_TAKEN
} FetchBackgroundHeadersState;

typedef enum {
    FETCH_BACKGROUND_PRECONNECT_FREE = 0,
    FETCH_BACKGROUND_PRECONNECT_PREPARING,
    FETCH_BACKGROUND_PRECONNECT_QUEUED,
    FETCH_BACKGROUND_PRECONNECT_RUNNING,
    FETCH_BACKGROUND_PRECONNECT_CONNECTED,
    FETCH_BACKGROUND_PRECONNECT_FAILED
} FetchBackgroundPreconnectState;

typedef struct {
    uint16_t value_offset;
    uint16_t value_length;
} FetchBackgroundCookie;

typedef struct {
    atomic_int state;
    atomic_int cancel_requested;
    atomic_int discard_requested;
    atomic_uint received_body_bytes;
    atomic_uint generation;
    atomic_int chunk_state;
    atomic_uint chunk_bytes;
    atomic_int headers_state;
    int buffer_index;
    CURL *easy;
    struct curl_slist *headers;
    FetchRedirectUrlValidator validator;
    size_t maximum_bytes;
    size_t stream_chunk_target;
    size_t length;
    long timeout_ms;
    long connect_timeout_ms;
    uint64_t started_us;
    uint64_t deadline_us;
    uint64_t pause_started_us;
    uint64_t last_progress_us;
    unsigned redirects;
    uint32_t redirect_transition_hashes[FETCH_REDIRECT_LIMIT];
    uint8_t redirect_transition_count;
    bool redirect_loop_detected;
    bool streaming;
    bool single_hop;
    bool transfer_done;
    bool curl_paused;
    bool allow_http_errors;
    bool redirect_same_origin_only;
    bool body_enabled;
    bool identity_encoding;
    bool force_fresh_connection;
    bool send_low_client_hints;
    bool send_client_hints;
    bool suppress_high_client_hints;
    bool tls12_retry_attempted;
    size_t total_length;
    size_t response_cookie_bytes;
    size_t response_cookie_count;
    long header_status;
    char initial_url[FETCH_BACKGROUND_URL_CAPACITY];
    char url[FETCH_BACKGROUND_URL_CAPACITY];
    char referer[FETCH_BACKGROUND_REFERER_CAPACITY];
    char method[FETCH_BACKGROUND_METHOD_CAPACITY];
    char body[FETCH_BACKGROUND_BODY_CAPACITY];
    size_t body_length;
    char content_type[FETCH_BACKGROUND_CONTENT_TYPE_CAPACITY];
    char extra_headers[FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY];
    char cookie[FETCH_BACKGROUND_COOKIE_CAPACITY];
    char origin[FETCH_BACKGROUND_ORIGIN_CAPACITY];
    char accept[FETCH_BACKGROUND_ACCEPT_CAPACITY];
    char user_agent[FETCH_BACKGROUND_USER_AGENT_CAPACITY];
    char if_none_match[192];
    char if_modified_since[128];
    char client_hint_tokens[1024];
    char client_hint_origin[2048];
    char sec_fetch_dest[FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY];
    char sec_fetch_mode[FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY];
    char sec_fetch_site[FETCH_BACKGROUND_FETCH_TOKEN_CAPACITY];
    bool sec_fetch_user;
    bool upgrade_insecure_requests;
    FetchPrivateNetworkGuard private_network;
    char location[FETCH_BACKGROUND_URL_CAPACITY];
    char content_range[128];
    char content_type_response[128];
    char response_cookie_storage[FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES];
    FetchBackgroundCookie response_cookies[FETCH_RESPONSE_COOKIE_CAPACITY];
    char curl_error[CURL_ERROR_SIZE];
    FetchBackgroundResult result;
} FetchBackgroundSlot;

typedef struct {
    atomic_int state;
    atomic_int cancel_requested;
    CURL *easy;
    CURLcode result;
    long timeout_ms;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
} FetchBackgroundPreconnect;

typedef struct {
    Budget *budget;
    BudgetReservation stack_reservation;
    unsigned char *fixed_buffers[FETCH_BACKGROUND_ACTIVE_LIMIT];
    unsigned char *stream_buffers[FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT];
    CURLM *multi;
    SceUID event;
    SceUID thread;
    atomic_int stop_requested;
    atomic_uint streaming_started;
    atomic_uint fixed_started;
    atomic_uint peak_streaming_active;
    atomic_uint peak_fixed_active;
    atomic_uint worker_performs;
    atomic_uint worker_polls;
    atomic_uint header_callbacks;
    atomic_uint body_callbacks;
    atomic_uint completions;
    atomic_uint worker_perform_max_us;
    atomic_uint worker_perform_over_33ms;
    atomic_uint worker_perform_over_100ms;
    atomic_uint worker_setup_performs;
    atomic_uint worker_setup_perform_max_us;
    atomic_uint worker_steady_perform_max_us;
    atomic_uint worker_priority_failures;
    atomic_uint worker_service_max_us;
    atomic_uint worker_poll_max_us;
    atomic_uint worker_loop_max_us;
    atomic_int last_multi_code;
    atomic_int last_running;
    atomic_int network_regression_hint;
    FetchBackgroundPreconnect preconnect;
    bool initialized;
    FetchBackgroundSlot slots[FETCH_BACKGROUND_REQUEST_LIMIT];
} FetchBackgroundTransport;

static FetchBackgroundTransport fetch_background;
static atomic_int fetch_background_admission_open;
static atomic_int fetch_background_media_priority_active;

static uint64_t fetch_background_now_us(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void fetch_background_atomic_max(
    atomic_uint *maximum, uint64_t elapsed_us)
{
    unsigned candidate = elapsed_us > UINT_MAX
        ? UINT_MAX : (unsigned) elapsed_us;
    unsigned observed = atomic_load_explicit(maximum, memory_order_relaxed);
    while (candidate > observed
           && !atomic_compare_exchange_weak_explicit(
               maximum, &observed, candidate,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}
#endif

static unsigned fetch_background_id_slot(uint64_t request_id)
{
    return fetch_background_request_id_slot(request_id);
}

static uint32_t fetch_background_id_generation(uint64_t request_id)
{
    return fetch_background_request_id_generation(request_id);
}

static uint64_t fetch_background_make_id(unsigned slot, uint32_t generation)
{
    return fetch_background_request_id_make(slot, generation);
}

static bool fetch_background_slot_matches(
    const FetchBackgroundSlot *slot, uint64_t request_id)
{
    return slot != NULL
        && atomic_load_explicit(
               &slot->generation, memory_order_acquire)
               == fetch_background_id_generation(request_id);
}

static size_t fetch_background_buffer_limit(
    const FetchBackgroundSlot *slot)
{
    return slot != NULL && slot->streaming
        ? FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT
        : FETCH_BACKGROUND_ACTIVE_LIMIT;
}

static size_t fetch_background_buffer_capacity(
    const FetchBackgroundSlot *slot)
{
    return slot != NULL && slot->streaming
        ? FETCH_BACKGROUND_STREAM_BUFFER_BYTES
        : FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES;
}

static unsigned char *fetch_background_slot_buffer(
    const FetchBackgroundSlot *slot)
{
    if (slot == NULL || slot->buffer_index < 0
        || (size_t) slot->buffer_index
               >= fetch_background_buffer_limit(slot)) return NULL;
    return slot->streaming
        ? fetch_background.stream_buffers[slot->buffer_index]
        : fetch_background.fixed_buffers[slot->buffer_index];
}

static FetchBackgroundCookieCapture fetch_background_capture_cookie(
    FetchBackgroundSlot *slot, const char *value, size_t value_length)
{
    if (slot == NULL || value == NULL)
        return FETCH_BACKGROUND_COOKIE_REJECTED;
    FetchBackgroundCookieCapture policy =
        fetch_background_cookie_capture_policy(
            value_length, slot->response_cookie_count,
            slot->response_cookie_bytes);
    if (policy != FETCH_BACKGROUND_COOKIE_CAPTURED) return policy;
    size_t value_offset = slot->response_cookie_bytes;
    memcpy(slot->response_cookie_storage + value_offset, value, value_length);
    slot->response_cookie_storage[value_offset + value_length] = '\0';
    slot->response_cookie_bytes = value_offset + value_length + 1u;
    slot->response_cookies[slot->response_cookie_count++] =
        (FetchBackgroundCookie) {
            .value_offset = (uint16_t) value_offset,
            .value_length = (uint16_t) value_length
        };
    return FETCH_BACKGROUND_COOKIE_CAPTURED;
}

static void fetch_background_reset_response_headers(
    FetchBackgroundSlot *slot)
{
    if (slot == NULL) return;
    slot->header_status = 0;
    slot->location[0] = '\0';
    slot->content_range[0] = '\0';
    slot->content_type_response[0] = '\0';
    slot->response_cookie_bytes = 0;
    slot->response_cookie_count = 0;
    slot->result.status_code = 0;
    slot->result.effective_url[0] = '\0';
    slot->result.content_type[0] = '\0';
    slot->result.etag[0] = '\0';
    slot->result.last_modified[0] = '\0';
    slot->result.cf_mitigated[0] = '\0';
    slot->result.accept_ch[0] = '\0';
    slot->result.critical_ch[0] = '\0';
    slot->result.server[0] = '\0';
    slot->result.cf_ray[0] = '\0';
    slot->result.response_headers[0] = '\0';
    slot->result.response_headers_length = 0;
    fetch_response_security_metadata_reset(&slot->result.security);
    slot->result.response_referrer_policy[0] = '\0';
    slot->result.response_referrer_policy_header_present = false;
    slot->result.response_referrer_policy_metadata_valid = true;
    slot->result.response_security_headers_truncated = false;
    slot->result.set_cookies_truncated = false;
    atomic_store_explicit(
        &slot->headers_state, FETCH_BACKGROUND_HEADERS_EMPTY,
        memory_order_release);
}

static bool fetch_background_store_header(
    FetchBackgroundSlot *slot, const char *data, size_t length)
{
    const char *colon = memchr(data, ':', length);
    if (slot == NULL || colon == NULL || colon == data) return true;
    size_t name_length = (size_t) (colon - data);
    const char *value = colon + 1;
    size_t value_length = length - name_length - 1u;
    while (value_length != 0 && (*value == ' ' || *value == '\t')) {
        value++;
        value_length--;
    }
    while (value_length != 0
           && (value[value_length - 1u] == '\r'
               || value[value_length - 1u] == '\n'
               || isspace((unsigned char) value[value_length - 1u]))) {
        value_length--;
    }
    bool set_cookie = name_length == 10u
        && strncasecmp(data, "set-cookie", 10u) == 0;
    bool forbidden = set_cookie
        || (name_length == 11u
            && strncasecmp(data, "set-cookie2", 11u) == 0);
    size_t required = name_length + 2u + value_length + 1u;
    bool stored = false;
    if (!forbidden && required < sizeof(slot->result.response_headers)
                                     - slot->result.response_headers_length) {
        size_t at = slot->result.response_headers_length;
        for (size_t i = 0; i < name_length; i++) {
            slot->result.response_headers[at++] =
                (char) tolower((unsigned char) data[i]);
        }
        slot->result.response_headers[at++] = ':';
        slot->result.response_headers[at++] = ' ';
        memcpy(slot->result.response_headers + at, value, value_length);
        at += value_length;
        slot->result.response_headers[at++] = '\n';
        slot->result.response_headers[at] = '\0';
        slot->result.response_headers_length = at;
        stored = true;
    }
    (void) fetch_response_security_metadata_collect(
        &slot->result.security, data, name_length, value, value_length, stored);
    snprintf(slot->result.response_referrer_policy,
             sizeof(slot->result.response_referrer_policy), "%s",
             slot->result.security.referrer_policy);
    slot->result.response_referrer_policy_header_present =
        slot->result.security.referrer_policy_state
            != FETCH_SECURITY_FIELD_ABSENT;
    slot->result.response_security_headers_truncated =
        slot->result.security.snapshot_truncated;
    if (name_length == 4u
               && strncasecmp(data, "etag", 4u) == 0) {
        copy_header_value(slot->result.etag, sizeof(slot->result.etag),
                          value, value_length);
    } else if (name_length == 13u
               && strncasecmp(data, "last-modified", 13u) == 0) {
        copy_header_value(
            slot->result.last_modified, sizeof(slot->result.last_modified),
            value, value_length);
    } else if (name_length == 8u
               && strncasecmp(data, "location", 8u) == 0) {
        copy_header_value(slot->location, sizeof(slot->location),
                          value, value_length);
    } else if (set_cookie) {
        FetchBackgroundCookieCapture capture =
            fetch_background_capture_cookie(slot, value, value_length);
        if (capture == FETCH_BACKGROUND_COOKIE_TRUNCATED) {
            slot->result.set_cookies_truncated = true;
        } else if (capture == FETCH_BACKGROUND_COOKIE_REJECTED) {
            snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                     "background response cookies exceeded bound");
            return false;
        }
    } else if (name_length == 12u
               && strncasecmp(data, "content-type", 12u) == 0) {
        copy_header_value(
            slot->result.content_type, sizeof(slot->result.content_type),
            value, value_length);
        snprintf(slot->content_type_response,
                 sizeof(slot->content_type_response), "%s",
                 slot->result.content_type);
    } else if (name_length == 13u
               && strncasecmp(data, "content-range", 13u) == 0) {
        copy_header_value(slot->content_range, sizeof(slot->content_range),
                          value, value_length);
    } else if (name_length == 12u
               && strncasecmp(data, "cf-mitigated", 12u) == 0) {
        copy_header_value(
            slot->result.cf_mitigated, sizeof(slot->result.cf_mitigated),
            value, value_length);
    } else if (name_length == 9u
               && strncasecmp(data, "accept-ch", 9u) == 0) {
        if (!append_header_list_value(
                slot->result.accept_ch, sizeof(slot->result.accept_ch),
                value, value_length)) return false;
    } else if (name_length == 11u
               && strncasecmp(data, "critical-ch", 11u) == 0) {
        if (!append_header_list_value(
                slot->result.critical_ch, sizeof(slot->result.critical_ch),
                value, value_length)) return false;
    } else if (name_length == 6u
               && strncasecmp(data, "server", 6u) == 0) {
        copy_header_value(slot->result.server, sizeof(slot->result.server),
                          value, value_length);
    } else if (name_length == 6u
               && strncasecmp(data, "cf-ray", 6u) == 0) {
        copy_header_value(slot->result.cf_ray, sizeof(slot->result.cf_ray),
                          value, value_length);
    }
    return true;
}

static size_t fetch_background_receive_header(
    char *data, size_t size, size_t count, void *opaque)
{
    FetchBackgroundSlot *slot = opaque;
    if (slot == NULL || (size != 0 && count > SIZE_MAX / size)) return 0;
    (void) atomic_fetch_add_explicit(
        &fetch_background.header_callbacks, 1u, memory_order_relaxed);
    size_t length = size * count;
    slot->last_progress_us = fetch_background_now_us();
    if (length >= 5u && strncasecmp(data, "HTTP/", 5u) == 0) {
        long status = 0;
        char status_line[128];
        size_t copied = length < sizeof(status_line) - 1u
            ? length : sizeof(status_line) - 1u;
        memcpy(status_line, data, copied);
        status_line[copied] = '\0';
        if (sscanf(status_line, "HTTP/%*s %ld", &status) == 1) {
            fetch_background_reset_response_headers(slot);
            slot->header_status = status;
            slot->result.status_code = status;
        }
    } else if ((length == 2u && data[0] == '\r' && data[1] == '\n')
               || (length == 1u && data[0] == '\n')) {
        if (slot->header_status >= 100 && slot->header_status < 200) {
            fetch_background_reset_response_headers(slot);
        } else {
            if (slot->result.set_cookies_truncated
                && fetch_background_redirect_cookie_overflow_fatal(
                       slot->single_hop, slot->header_status,
                       slot->location[0] != '\0')) {
                snprintf(
                    slot->result.error, sizeof(slot->result.error),
                    "%s", "redirect response cookies exceeded transport bounds");
                return 0;
            }
            slot->result.status_code = slot->header_status;
            snprintf(slot->result.effective_url,
                     sizeof(slot->result.effective_url), "%s", slot->url);
            atomic_store_explicit(
                &slot->headers_state, FETCH_BACKGROUND_HEADERS_READY,
                memory_order_release);
        }
    } else {
        if (!fetch_background_store_header(slot, data, length)) return 0;
    }
    return length;
}

static size_t fetch_background_receive_body(
    char *data, size_t size, size_t count, void *opaque)
{
    FetchBackgroundSlot *slot = opaque;
    if (slot == NULL || (size != 0 && count > SIZE_MAX / size)) return 0;
    (void) atomic_fetch_add_explicit(
        &fetch_background.body_callbacks, 1u, memory_order_relaxed);
    size_t length = size * count;
    slot->last_progress_us = fetch_background_now_us();
    /* Redirect bodies are neither exposed nor charged to the final response. */
    if (slot->header_status >= 300 && slot->header_status < 400)
        return length;
    if (slot->streaming
        && atomic_load_explicit(
               &slot->chunk_state, memory_order_acquire)
               != FETCH_BACKGROUND_CHUNK_EMPTY) {
        slot->curl_paused = true;
        slot->pause_started_us = fetch_background_now_us();
        return CURL_WRITEFUNC_PAUSE;
    }
    size_t buffer_capacity = fetch_background_buffer_capacity(slot);
    if (slot->buffer_index < 0
        || (size_t) slot->buffer_index
               >= fetch_background_buffer_limit(slot)
        || slot->total_length > slot->maximum_bytes
        || length > slot->maximum_bytes - slot->total_length
        || slot->length > buffer_capacity
        || length > buffer_capacity - slot->length) {
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 "background response quota exceeded");
        return 0;
    }
    unsigned char *buffer = fetch_background_slot_buffer(slot);
    if (buffer == NULL) return 0;
    memcpy(buffer + slot->length, data, length);
    slot->length += length;
    slot->total_length += length;
    atomic_store_explicit(
        &slot->received_body_bytes, (unsigned) slot->total_length,
        memory_order_release);
    if (slot->streaming
        && slot->length >= slot->stream_chunk_target) {
        atomic_store_explicit(
            &slot->chunk_bytes, (unsigned) slot->length,
            memory_order_release);
        atomic_store_explicit(
            &slot->chunk_state, FETCH_BACKGROUND_CHUNK_READY,
            memory_order_release);
    }
    return length;
}

static int fetch_background_transfer_progress(
    void *opaque, curl_off_t download_total, curl_off_t downloaded,
    curl_off_t upload_total, curl_off_t uploaded)
{
    (void) download_total;
    (void) upload_total;
    (void) uploaded;
    FetchBackgroundSlot *slot = opaque;
    if (slot == NULL) return 1;
    if (!slot->streaming && downloaded > 0
        && (uint64_t) downloaded <= UINT_MAX) {
        atomic_store_explicit(
            &slot->received_body_bytes, (unsigned) downloaded,
            memory_order_release);
    }
    return atomic_load_explicit(
               &slot->cancel_requested, memory_order_acquire) != 0;
}

static void fetch_background_slot_release_curl(FetchBackgroundSlot *slot)
{
    if (slot == NULL) return;
    if (slot->easy != NULL) {
        (void) curl_multi_remove_handle(fetch_background.multi, slot->easy);
        curl_easy_cleanup(slot->easy);
        slot->easy = NULL;
    }
    curl_slist_free_all(slot->headers);
    slot->headers = NULL;
}

static bool fetch_background_append_header(
    FetchBackgroundSlot *slot, const char *value)
{
    struct curl_slist *next = curl_slist_append(slot->headers, value);
    if (next == NULL) return false;
    slot->headers = next;
    return true;
}

static bool fetch_background_append_named_header(
    FetchBackgroundSlot *slot, const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') return true;
    char line[FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY + 96u];
    int written = snprintf(line, sizeof(line), "%s: %s", name, value);
    return written >= 0 && (size_t) written < sizeof(line)
        && fetch_background_append_header(slot, line);
}

static bool fetch_background_append_extra_headers(
    FetchBackgroundSlot *slot, const char *headers)
{
    if (headers == NULL || headers[0] == '\0') return true;
    const char *at = headers;
    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        size_t length = (size_t) (end - at);
        if (length == 0 || length >= FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY)
            return false;
        char line[FETCH_BACKGROUND_EXTRA_HEADERS_CAPACITY];
        memcpy(line, at, length);
        line[length] = '\0';
        if (!fetch_background_append_header(slot, line)) return false;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

static bool fetch_background_configure_request_shape(
    FetchBackgroundSlot *slot)
{
    FetchRequest request = {
        .method = slot->method,
        .body = slot->body_enabled ? slot->body : NULL,
        .body_length = slot->body_enabled ? slot->body_length : 0,
        .content_type = slot->content_type
    };
    bool hints_ready = true;
    if (slot->send_low_client_hints
        && tilefinch_url_potentially_trustworthy(slot->url)) {
        for (size_t i = 0;
             hints_ready
             && i < sizeof(fetch_low_client_hint_headers)
                      / sizeof(fetch_low_client_hint_headers[0]); i++) {
            hints_ready = fetch_background_append_header(
                slot, fetch_low_client_hint_headers[i]);
        }
    }
    bool high_entropy_allowed = slot->send_client_hints
        && !slot->suppress_high_client_hints
        && slot->client_hint_origin[0] != '\0'
        && tilefinch_url_same_origin(slot->client_hint_origin, slot->url)
        && tilefinch_url_potentially_trustworthy(slot->url);
    if (high_entropy_allowed) {
        for (size_t i = 0;
             hints_ready
             && i < sizeof(fetch_high_client_hints)
                      / sizeof(fetch_high_client_hints[0]); i++) {
            if (client_hint_token_requested(
                    slot->client_hint_tokens,
                    fetch_high_client_hints[i].token)) {
                hints_ready = fetch_background_append_header(
                    slot, fetch_high_client_hints[i].header);
            }
        }
    }
    return hints_ready && configure_easy_method(
               slot->easy, slot->method, &request, slot->body_enabled)
        && fetch_background_append_header(
               slot, "Accept-Language: en-US,en;q=0.9")
        && fetch_background_append_named_header(
               slot, "Origin", slot->origin)
        && fetch_background_append_named_header(
               slot, "Cookie", slot->cookie)
        && fetch_background_append_named_header(
               slot, "Accept", slot->accept)
        && fetch_background_append_named_header(
               slot, "Sec-Fetch-Dest", slot->sec_fetch_dest)
        && fetch_background_append_named_header(
               slot, "Sec-Fetch-Mode", slot->sec_fetch_mode)
        && fetch_background_append_named_header(
               slot, "Sec-Fetch-Site", slot->sec_fetch_site)
        && (!slot->sec_fetch_user
            || fetch_background_append_header(
                   slot, "Sec-Fetch-User: ?1"))
        && (!slot->upgrade_insecure_requests
            || fetch_background_append_header(
                   slot, "Upgrade-Insecure-Requests: 1"))
        && fetch_background_append_named_header(
               slot, "Content-Type",
               slot->body_enabled ? slot->content_type : NULL)
        && fetch_background_append_named_header(
               slot, "If-None-Match", slot->if_none_match)
        && fetch_background_append_named_header(
               slot, "If-Modified-Since", slot->if_modified_since)
        && fetch_background_append_extra_headers(
               slot, slot->extra_headers);
}

static long fetch_background_remaining_ms(const FetchBackgroundSlot *slot)
{
    uint64_t now_us = fetch_background_now_us();
    if (now_us >= slot->deadline_us) return 0;
    uint64_t remaining_us = slot->deadline_us - now_us;
    uint64_t remaining_ms = (remaining_us + 999u) / 1000u;
    return remaining_ms > (uint64_t) LONG_MAX
        ? LONG_MAX : (long) remaining_ms;
}

static bool fetch_background_configure_easy(FetchBackgroundSlot *slot)
{
    if (slot == NULL || slot->url[0] == '\0') return false;
    fetch_background_slot_release_curl(slot);
    slot->easy = curl_easy_init();
    if (slot->easy == NULL) return false;
    long remaining_ms = fetch_background_remaining_ms(slot);
    if (remaining_ms <= 0) {
        slot->result.timed_out = true;
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 "background request deadline expired");
        return false;
    }
    long connect_ms = remaining_ms / 2;
    if (connect_ms <= 0) connect_ms = 1;
    if (slot->connect_timeout_ms > 0
        && slot->connect_timeout_ms < connect_ms) {
        connect_ms = slot->connect_timeout_ms;
    }
    bool request_ready = slot->streaming
        ? fetch_background_configure_request_shape(slot)
        : fetch_background_append_header(
              slot, "Accept: video/mp4,video/*;q=0.9,*/*;q=0.5")
          && fetch_background_append_header(
                 slot, "Sec-Fetch-Dest: video")
          && fetch_background_append_header(
                 slot, "Sec-Fetch-Mode: no-cors")
          && fetch_background_append_header(
                 slot, "Sec-Fetch-Site: cross-site");
    if (!request_ready
        || !curl_easy_attach_shared_state(slot->easy)
        || !fetch_configure_tls(slot->easy)
        || !fetch_configure_http_version(slot->easy)
        || !fetch_configure_private_network(
               slot->easy, &slot->private_network)
        || curl_easy_setopt(slot->easy, CURLOPT_URL, slot->url) != CURLE_OK
        || curl_easy_setopt(slot->easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, TILEFINCH_CURL_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, TILEFINCH_CURL_REDIR_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_ACCEPT_ENCODING,
               slot->identity_encoding ? "identity" : "") != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_USERAGENT,
               slot->user_agent[0] == '\0'
                   ? FETCH_BACKGROUND_USER_AGENT : slot->user_agent)
               != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_CONNECTTIMEOUT_MS, connect_ms) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_FRESH_CONNECT,
               slot->force_fresh_connection ? 1L : 0L) != CURLE_OK
        /* The worker enforces an active-transport deadline itself. libcurl's
           wall timeout continues while a write callback is paused for the
           browser to parse a prior chunk, incorrectly charging layout/script
           work as network time. */
        || curl_easy_setopt(slot->easy, CURLOPT_TIMEOUT_MS, 0L) != CURLE_OK
        || curl_easy_setopt(slot->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(slot->easy, CURLOPT_FAILONERROR, 0L) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_ERRORBUFFER, slot->curl_error) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_WRITEFUNCTION,
               fetch_background_receive_body) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_WRITEDATA, slot) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_HEADERFUNCTION,
               fetch_background_receive_header) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_HEADERDATA, slot) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_XFERINFOFUNCTION,
               fetch_background_transfer_progress) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_XFERINFODATA, slot) != CURLE_OK
        || curl_easy_setopt(slot->easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK
        || curl_easy_setopt(
               slot->easy, CURLOPT_HTTPHEADER, slot->headers) != CURLE_OK
        || curl_easy_setopt(slot->easy, CURLOPT_PRIVATE, slot) != CURLE_OK
        || (slot->referer[0] != '\0'
            && curl_easy_setopt(
                   slot->easy, CURLOPT_REFERER, slot->referer) != CURLE_OK)) {
        return false;
    }
    return curl_multi_add_handle(fetch_background.multi, slot->easy)
        == CURLM_OK;
}

static int fetch_background_find_buffer(bool streaming)
{
    bool used[FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT] = {false};
    size_t limit = streaming ? FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT
                             : FETCH_BACKGROUND_ACTIVE_LIMIT;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if ((state == FETCH_BACKGROUND_SLOT_RUNNING
             || state == FETCH_BACKGROUND_SLOT_COMPLETE
             || state == FETCH_BACKGROUND_SLOT_TAKING)
            && slot->streaming == streaming
            && slot->buffer_index >= 0
            && (size_t) slot->buffer_index < limit) {
            used[slot->buffer_index] = true;
        }
    }
    for (size_t i = 0; i < limit; i++) {
        unsigned char *buffer = streaming
            ? fetch_background.stream_buffers[i]
            : fetch_background.fixed_buffers[i];
        if (!used[i] && buffer != NULL) return (int) i;
    }
    return -1;
}

/* The transport can be created by HOME's connect-only speculation, which
   needs no response storage and runs at the tightest point in the native
   boot footprint. Grow streaming storage on the browser thread only when a
   real response is admitted. The count includes queued descriptors so the
   worker can never observe more runnable streams than allocated buffers. */
static FetchBackgroundEnqueueStatus
fetch_background_ensure_stream_capacity(void)
{
    size_t required = 1u;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state != FETCH_BACKGROUND_SLOT_FREE && slot->streaming)
            required++;
    }
    if (required > FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT)
        return FETCH_BACKGROUND_ENQUEUE_SATURATED;

    size_t allocated = 0;
    for (unsigned i = 0; i < FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT; i++)
        if (fetch_background.stream_buffers[i] != NULL) allocated++;
    if (allocated >= required) return FETCH_BACKGROUND_ENQUEUE_ADMITTED;

    bool allocated_now[FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT] = {false};
    for (unsigned i = 0;
         i < FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT && allocated < required;
         i++) {
        if (fetch_background.stream_buffers[i] != NULL) continue;
        fetch_background.stream_buffers[i] = budget_malloc_category(
            fetch_background.budget, BUDGET_CATEGORY_RESOURCE,
            FETCH_BACKGROUND_STREAM_BUFFER_BYTES);
        if (fetch_background.stream_buffers[i] == NULL) {
            for (unsigned j = 0; j < FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT;
                 j++) {
                if (!allocated_now[j]) continue;
                budget_free(fetch_background.budget,
                            fetch_background.stream_buffers[j]);
                fetch_background.stream_buffers[j] = NULL;
            }
            return FETCH_BACKGROUND_ENQUEUE_MEMORY;
        }
        allocated_now[i] = true;
        allocated++;
    }
    return allocated >= required
        ? FETCH_BACKGROUND_ENQUEUE_ADMITTED
        : FETCH_BACKGROUND_ENQUEUE_MEMORY;
}

static void fetch_background_record_start(bool streaming)
{
    atomic_uint *started = streaming
        ? &fetch_background.streaming_started
        : &fetch_background.fixed_started;
    atomic_uint *peak = streaming
        ? &fetch_background.peak_streaming_active
        : &fetch_background.peak_fixed_active;
    (void) atomic_fetch_add_explicit(started, 1u, memory_order_relaxed);
    unsigned active = 0;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        if (slot->streaming == streaming
            && atomic_load_explicit(
                   &slot->state, memory_order_acquire)
                   == FETCH_BACKGROUND_SLOT_RUNNING) active++;
    }
    unsigned observed = atomic_load_explicit(peak, memory_order_relaxed);
    while (active > observed
           && !atomic_compare_exchange_weak_explicit(
                  peak, &observed, active,
                  memory_order_relaxed, memory_order_relaxed)) {}
}

static void fetch_background_record_network_regression(CURLcode code)
{
    if (code != CURLE_COULDNT_RESOLVE_HOST
        && code != CURLE_COULDNT_CONNECT
        && code != CURLE_SEND_ERROR
        && code != CURLE_RECV_ERROR) return;
    atomic_store_explicit(
        &fetch_background.network_regression_hint, 1,
        memory_order_release);
}

static void fetch_background_publish_complete(
    FetchBackgroundSlot *slot, bool success, CURLcode code)
{
    slot->result.transport_code = (long) code;
    if (slot->easy != NULL) {
        (void) curl_easy_getinfo(
            slot->easy, CURLINFO_SSL_VERIFYRESULT,
            &slot->result.tls_verify_result);
    }
    slot->result.success = success;
    slot->result.length = slot->streaming
        ? slot->total_length : slot->length;
    slot->result.received_body_bytes = atomic_load_explicit(
        &slot->received_body_bytes, memory_order_acquire);
    slot->result.timed_out = code == CURLE_OPERATION_TIMEDOUT
        || slot->result.timed_out;
    slot->result.status_code = slot->header_status;
    slot->result.tls12_compatibility_retry = slot->tls12_retry_attempted;
    if (!success && slot->result.error[0] == '\0') {
        const char *message = atomic_load_explicit(
                &slot->cancel_requested, memory_order_acquire)
            ? "background request cancelled"
            : (slot->curl_error[0] != '\0'
                  ? slot->curl_error : curl_easy_strerror(code));
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 message);
    }
    if (atomic_load_explicit(
            &slot->discard_requested, memory_order_acquire)) {
        atomic_store_explicit(
            &slot->state, FETCH_BACKGROUND_SLOT_FREE,
            memory_order_release);
    } else {
        atomic_store_explicit(
            &slot->state, FETCH_BACKGROUND_SLOT_COMPLETE,
            memory_order_release);
    }
}

static void fetch_background_publish_stream_chunk(
    FetchBackgroundSlot *slot)
{
    if (slot == NULL || !slot->streaming || slot->length == 0) return;
    atomic_store_explicit(
        &slot->chunk_bytes, (unsigned) slot->length,
        memory_order_release);
    atomic_store_explicit(
        &slot->chunk_state, FETCH_BACKGROUND_CHUNK_READY,
        memory_order_release);
}

static void fetch_background_finish_stream_if_drained(
    FetchBackgroundSlot *slot)
{
    if (slot == NULL || !slot->streaming || !slot->transfer_done
        || atomic_load_explicit(
               &slot->chunk_state, memory_order_acquire)
               != FETCH_BACKGROUND_CHUNK_EMPTY) return;
    fetch_background_publish_complete(
        slot, slot->result.success,
        slot->result.timed_out ? CURLE_OPERATION_TIMEDOUT : CURLE_OK);
}

static bool fetch_background_follow_redirect(FetchBackgroundSlot *slot)
{
    if (slot->header_status < 300 || slot->header_status >= 400
        || slot->location[0] == '\0'
        || slot->redirects >= FETCH_REDIRECT_LIMIT) return false;
    char resolved[FETCH_BACKGROUND_URL_CAPACITY];
    if (!tilefinch_url_resolve(
            slot->url, slot->location, resolved, sizeof(resolved))
        || (slot->validator != NULL && !slot->validator(resolved))
        || (slot->redirect_same_origin_only
            && !tilefinch_url_same_origin(slot->initial_url, resolved))) {
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 "background redirect left the admitted media hosts");
        return false;
    }
    uint32_t transition_hash =
        fetch_redirect_transition_hash(slot->url, resolved);
    for (uint8_t i = 0; i < slot->redirect_transition_count; i++) {
        if (slot->redirect_transition_hashes[i] == transition_hash) {
            slot->redirect_loop_detected = true;
            snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                     "response redirect loop detected");
            return false;
        }
    }
    size_t length = strlen(resolved);
    if (length >= sizeof(slot->url)) return false;
    memcpy(slot->url, resolved, length + 1u);
    if (slot->redirect_transition_count < FETCH_REDIRECT_LIMIT) {
        slot->redirect_transition_hashes[
            slot->redirect_transition_count++] = transition_hash;
    }
    if (slot->streaming
        && (slot->header_status == 301 || slot->header_status == 302
            || slot->header_status == 303)
        && strcasecmp(slot->method, "GET") != 0
        && strcasecmp(slot->method, "HEAD") != 0) {
        snprintf(slot->method, sizeof(slot->method), "%s", "GET");
        slot->body_enabled = false;
    }
    slot->redirects++;
    slot->length = 0;
    slot->total_length = 0;
    atomic_store_explicit(
        &slot->received_body_bytes, 0u, memory_order_release);
    slot->header_status = 0;
    slot->location[0] = '\0';
    slot->content_range[0] = '\0';
    slot->curl_error[0] = '\0';
    return fetch_background_configure_easy(slot);
}

static void fetch_background_finish_easy(
    FetchBackgroundSlot *slot, CURLcode code)
{
    if (slot == NULL || slot->easy == NULL) return;
    long status = 0;
    char *effective_url = NULL;
    (void) curl_easy_getinfo(slot->easy, CURLINFO_RESPONSE_CODE, &status);
    (void) curl_easy_getinfo(
        slot->easy, CURLINFO_EFFECTIVE_URL, &effective_url);
    slot->header_status = status;
    if (!slot->single_hop && code == CURLE_OK
        && status >= 300 && status < 400
        && fetch_background_follow_redirect(slot)) return;

    if (!slot->private_network.blocked && code != CURLE_OK
        && slot->length == 0 && slot->header_status == 0) {
        FetchRequest request = {
            .connect_timeout_ms = slot->connect_timeout_ms
        };
        long remaining_ms = fetch_background_remaining_ms(slot);
        if (fetch_try_tls12_compatibility(
                slot->easy, code, &slot->tls12_retry_attempted,
                remaining_ms, &request)) {
            slot->curl_error[0] = '\0';
            if (curl_multi_remove_handle(
                    fetch_background.multi, slot->easy) == CURLM_OK
                && curl_multi_add_handle(
                       fetch_background.multi, slot->easy) == CURLM_OK) {
                return;
            }
        }
    }

    /* Record after the TLS-compatibility retry decision, but before a
       streaming tail can remain pending on the browser thread. The
       supervisor still corroborates this hint with APCTL/WLAN state. */
    if (!slot->private_network.blocked)
        fetch_background_record_network_regression(code);
    slot->result.status_code = status;
    if (slot->private_network.blocked) {
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 "blocked private network target");
    }
    if (effective_url != NULL) {
        snprintf(slot->result.effective_url,
                 sizeof(slot->result.effective_url), "%s", effective_url);
    }
    snprintf(slot->result.content_range,
             sizeof(slot->result.content_range), "%s", slot->content_range);
    (void) curl_easy_getinfo(
        slot->easy, CURLINFO_HTTP_VERSION,
        &slot->result.negotiated_http_version);
#if LIBCURL_VERSION_NUM >= 0x073200
    (void) curl_easy_getinfo(
        slot->easy, CURLINFO_NUM_CONNECTS, &slot->result.new_connections);
#endif
    FetchHandshakeSample handshake;
    fetch_handshake_sample(slot->easy, &handshake);
    slot->result.tls_connection_reuse_known = handshake.classified;
    slot->result.tls_connection_reused = handshake.reused;
    slot->result.tls_handshake_measured = handshake.measured;
    slot->result.tls_handshake_us = handshake.handshake_us;
    bool admitted = !slot->private_network.blocked && code == CURLE_OK
        && slot->result.error[0] == '\0'
        && ((status >= 200 && status < 300)
            /* The browser thread decides whether a 3xx is a redirect, a
               cache-validation response (304), or a terminal response. The
               worker must not pre-empt that policy decision. */
            || (slot->single_hop && status >= 300 && status < 400)
            || (slot->streaming && slot->allow_http_errors
                && status >= 400 && status < 600))
        && (slot->validator == NULL
            || (effective_url != NULL && slot->validator(effective_url)));
    if (code == CURLE_OK && !admitted && slot->result.error[0] == '\0') {
        snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                 status >= 300 && status < 400
                    ? "background redirect was invalid or exceeded eight hops"
                    : (slot->single_hop
                           ? "background HTTP hop failed transport policy"
                           : "background response failed media URL policy"));
    }
    fetch_background_slot_release_curl(slot);
    if (slot->streaming) {
        slot->result.success = admitted;
        slot->result.timed_out = code == CURLE_OPERATION_TIMEDOUT;
        slot->transfer_done = true;
        if (slot->length != 0) {
            fetch_background_publish_stream_chunk(slot);
        } else {
            fetch_background_publish_complete(slot, admitted, code);
        }
    } else {
        fetch_background_publish_complete(slot, admitted, code);
    }
}

static void fetch_background_preconnect_release(bool publish_free)
{
    FetchBackgroundPreconnect *preconnect = &fetch_background.preconnect;
    if (preconnect->easy != NULL) {
        (void) curl_multi_remove_handle(
            fetch_background.multi, preconnect->easy);
        curl_easy_cleanup(preconnect->easy);
        preconnect->easy = NULL;
    }
    if (publish_free) {
        preconnect->origin[0] = '\0';
        atomic_store_explicit(
            &preconnect->cancel_requested, 0, memory_order_release);
        atomic_store_explicit(
            &preconnect->state, FETCH_BACKGROUND_PRECONNECT_FREE,
            memory_order_release);
    }
}

static bool fetch_background_preconnect_configure(void)
{
    FetchBackgroundPreconnect *preconnect = &fetch_background.preconnect;
    preconnect->easy = curl_easy_init();
    if (preconnect->easy == NULL) return false;
    CURL *easy = preconnect->easy;
    if (!curl_easy_attach_shared_state(easy)
        || !fetch_configure_tls(easy)
        || !fetch_configure_http_version(easy)
        || curl_easy_setopt(easy, CURLOPT_URL, preconnect->origin)
               != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_CONNECTTIMEOUT_MS, preconnect->timeout_ms)
               != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, preconnect->timeout_ms)
               != CURLE_OK
        || curl_easy_setopt(
               easy, TILEFINCH_CURL_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_multi_add_handle(fetch_background.multi, easy) != CURLM_OK) {
        fetch_background_preconnect_release(false);
        return false;
    }
    return true;
}

/* The speculative connection has its own compact descriptor. It neither
   occupies nor reduces the six authoritative/speculative response lanes. */
static void fetch_background_service_preconnect(void)
{
    FetchBackgroundPreconnect *preconnect = &fetch_background.preconnect;
    int state = atomic_load_explicit(
        &preconnect->state, memory_order_acquire);
    bool cancelled = atomic_load_explicit(
        &preconnect->cancel_requested, memory_order_acquire) != 0;
    if (cancelled) {
        if (state != FETCH_BACKGROUND_PRECONNECT_FREE
            && state != FETCH_BACKGROUND_PRECONNECT_PREPARING) {
            fetch_background_preconnect_release(true);
        }
        return;
    }
    if (state != FETCH_BACKGROUND_PRECONNECT_QUEUED) return;
    int expected = FETCH_BACKGROUND_PRECONNECT_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(
            &preconnect->state, &expected,
            FETCH_BACKGROUND_PRECONNECT_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) return;
    if (!fetch_background_preconnect_configure()) {
        preconnect->result = CURLE_FAILED_INIT;
        atomic_store_explicit(
            &preconnect->state, FETCH_BACKGROUND_PRECONNECT_FAILED,
            memory_order_release);
    }
}

static bool fetch_background_finish_preconnect(
    CURL *easy, CURLcode code)
{
    FetchBackgroundPreconnect *preconnect = &fetch_background.preconnect;
    if (easy == NULL || easy != preconnect->easy) return false;
    fetch_background_record_network_regression(code);
    (void) curl_multi_remove_handle(fetch_background.multi, easy);
    preconnect->result = code;
    if (atomic_load_explicit(
            &preconnect->cancel_requested, memory_order_acquire)) {
        fetch_background_preconnect_release(true);
    } else if (code == CURLE_OK) {
        /* Retain the CONNECT_ONLY easy/socket exactly as the old homepage
           path did. Navigation cancellation releases it after the shared DNS
           and TLS-session state has been warmed. */
        atomic_store_explicit(
            &preconnect->state, FETCH_BACKGROUND_PRECONNECT_CONNECTED,
            memory_order_release);
    } else {
        fetch_background_preconnect_release(false);
        atomic_store_explicit(
            &preconnect->state, FETCH_BACKGROUND_PRECONNECT_FAILED,
            memory_order_release);
    }
    return true;
}

static void fetch_background_start_queued(void)
{
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int expected = FETCH_BACKGROUND_SLOT_QUEUED;
        if (atomic_load_explicit(
                &slot->cancel_requested, memory_order_acquire)) {
            if (atomic_compare_exchange_strong_explicit(
                    &slot->state, &expected,
                    FETCH_BACKGROUND_SLOT_PREPARING,
                    memory_order_acq_rel, memory_order_acquire)) {
                fetch_background_publish_complete(
                    slot, false, CURLE_ABORTED_BY_CALLBACK);
            }
            continue;
        }
        int buffer = fetch_background_find_buffer(slot->streaming);
        /* Exhaustion in one pool must not head-of-line block the other pool.
           For example, two fixed media requests can occupy both large
           buffers while stylesheet/script streams still have free lanes. */
        if (buffer < 0) continue;
        expected = FETCH_BACKGROUND_SLOT_QUEUED;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, FETCH_BACKGROUND_SLOT_RUNNING,
                memory_order_acq_rel, memory_order_acquire)) continue;
        slot->buffer_index = buffer;
        slot->length = 0;
        slot->total_length = 0;
        slot->transfer_done = false;
        slot->curl_paused = false;
        slot->response_cookie_bytes = 0;
        slot->response_cookie_count = 0;
        memset(&slot->result, 0, sizeof(slot->result));
        atomic_store_explicit(
            &slot->headers_state, FETCH_BACKGROUND_HEADERS_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &slot->chunk_state, FETCH_BACKGROUND_CHUNK_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &slot->chunk_bytes, 0u, memory_order_release);
        atomic_store_explicit(
            &slot->received_body_bytes, 0u, memory_order_release);
        fetch_background_record_start(slot->streaming);
        if (!fetch_background_configure_easy(slot)) {
            fetch_background_slot_release_curl(slot);
            if (slot->result.error[0] == '\0') {
                snprintf(slot->result.error, sizeof(slot->result.error), "%s",
                         "background curl request setup failed");
            }
            fetch_background_publish_complete(
                slot, false, CURLE_FAILED_INIT);
        }
    }
}

static void fetch_background_collect_messages(void)
{
    int remaining = 0;
    CURLMsg *message = NULL;
    while ((message = curl_multi_info_read(
                fetch_background.multi, &remaining)) != NULL) {
        if (message->msg != CURLMSG_DONE) continue;
        if (fetch_background_finish_preconnect(
                message->easy_handle, message->data.result)) continue;
        (void) atomic_fetch_add_explicit(
            &fetch_background.completions, 1u, memory_order_relaxed);
        FetchBackgroundSlot *slot = NULL;
        (void) curl_easy_getinfo(
            message->easy_handle, CURLINFO_PRIVATE, &slot);
        fetch_background_finish_easy(slot, message->data.result);
    }
}

static void fetch_background_resume_streams(void)
{
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        if (!slot->streaming
            || atomic_load_explicit(
                   &slot->state, memory_order_acquire)
                   != FETCH_BACKGROUND_SLOT_RUNNING
            || atomic_load_explicit(
                   &slot->chunk_state, memory_order_acquire)
                   != FETCH_BACKGROUND_CHUNK_EMPTY) continue;
        if (slot->transfer_done) {
            fetch_background_finish_stream_if_drained(slot);
            continue;
        }
        if (slot->curl_paused && slot->easy != NULL) {
            uint64_t now_us = fetch_background_now_us();
            uint64_t paused_us = now_us >= slot->pause_started_us
                ? now_us - slot->pause_started_us : 0;
            slot->deadline_us = paused_us > UINT64_MAX - slot->deadline_us
                ? UINT64_MAX : slot->deadline_us + paused_us;
            slot->last_progress_us = now_us;
            slot->pause_started_us = 0;
            /* curl_easy_pause(CONT) may synchronously replay buffered body
               data and that callback may pause the handle again. Clear the
               old pause before the call so a synchronous re-pause wins. */
            slot->curl_paused = false;
            CURLcode code = curl_easy_pause(slot->easy, CURLPAUSE_CONT);
            if (code != CURLE_OK) {
                snprintf(slot->result.error, sizeof(slot->result.error),
                         "background curl resume failed (%d)", (int) code);
                fetch_background_slot_release_curl(slot);
                slot->result.success = false;
                slot->transfer_done = true;
                fetch_background_publish_complete(
                    slot, false, code);
            }
        }
    }
}

static void fetch_background_expire_active_deadlines(void)
{
    uint64_t now_us = fetch_background_now_us();
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        if (atomic_load_explicit(
                &slot->state, memory_order_acquire)
                != FETCH_BACKGROUND_SLOT_RUNNING
            || slot->curl_paused) continue;
        bool deadline_expired = now_us >= slot->deadline_us;
        bool response_stalled = slot->header_status != 0
            && now_us >= slot->last_progress_us
            && now_us - slot->last_progress_us
                   >= (uint64_t) FETCH_STALL_LOW_SPEED_SECONDS
                        * UINT64_C(1000000);
        if (!deadline_expired && !response_stalled) continue;
        snprintf(
            slot->result.error, sizeof(slot->result.error), "%s",
            deadline_expired
                ? "background active transport deadline expired"
                : "background response made no progress for three seconds");
        fetch_background_slot_release_curl(slot);
        slot->result.success = false;
        slot->result.timed_out = true;
        slot->transfer_done = true;
        if (slot->streaming && slot->length != 0)
            fetch_background_publish_stream_chunk(slot);
        else
            fetch_background_publish_complete(
                slot, false, CURLE_OPERATION_TIMEDOUT);
    }
}

static bool fetch_background_has_worker_work(void)
{
    int preconnect_state = atomic_load_explicit(
        &fetch_background.preconnect.state, memory_order_acquire);
    if (preconnect_state == FETCH_BACKGROUND_PRECONNECT_QUEUED
        || preconnect_state == FETCH_BACKGROUND_PRECONNECT_RUNNING) {
        return true;
    }
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        int state = atomic_load_explicit(
            &fetch_background.slots[i].state, memory_order_acquire);
        if (state == FETCH_BACKGROUND_SLOT_QUEUED
            || state == FETCH_BACKGROUND_SLOT_RUNNING) return true;
    }
    return false;
}

/*
 * PSP libcurl can spend hundreds of milliseconds inside one multi_perform
 * while resolving a name or establishing TCP/TLS. Keep queue service and the
 * bounded poll at the transport priority (the browser previously starved
 * those when the whole worker ran below it), but make only that irreducible
 * setup call preemptible. Once any response header has arrived the transfer
 * is steady-state body work and retains the ordinary transport priority.
 * All fields inspected here are worker-owned; the atomic byte load is a
 * conservative backstop for an unusual headerless response.
 */
static bool fetch_background_perform_has_setup_work(void)
{
    if (atomic_load_explicit(
            &fetch_background.preconnect.state, memory_order_acquire)
        == FETCH_BACKGROUND_PRECONNECT_RUNNING) return true;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire)
                != FETCH_BACKGROUND_SLOT_RUNNING) continue;
        if (slot->header_status == 0
            && atomic_load_explicit(
                   &slot->received_body_bytes, memory_order_acquire) == 0) {
            return true;
        }
    }
    return false;
}

static int fetch_background_worker_main(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    for (;;) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t loop_started_us = fetch_background_now_us();
        uint64_t service_started_us = loop_started_us;
#endif
        fetch_background_service_preconnect();
        fetch_background_resume_streams();
        fetch_background_start_queued();
        fetch_background_expire_active_deadlines();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t service_us =
            fetch_background_now_us() - service_started_us;
        fetch_background_atomic_max(
            &fetch_background.worker_service_max_us, service_us);
#endif
        int running = 0;
        bool setup_perform = fetch_background_perform_has_setup_work();
        SceUID worker_id = -1;
        bool setup_priority_active = false;
        if (setup_perform) {
            worker_id = sceKernelGetThreadId();
            int priority_result = sceKernelChangeThreadPriority(
                worker_id, TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP);
            if (priority_result >= 0) {
                setup_priority_active = true;
            } else {
                (void) atomic_fetch_add_explicit(
                    &fetch_background.worker_priority_failures, 1u,
                    memory_order_relaxed);
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            (void) atomic_fetch_add_explicit(
                &fetch_background.worker_setup_performs, 1u,
                memory_order_relaxed);
#endif
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t perform_started_us = fetch_background_now_us();
#endif
        CURLMcode code = curl_multi_perform(fetch_background.multi, &running);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t perform_us =
            fetch_background_now_us() - perform_started_us;
#endif
        if (setup_priority_active
            && sceKernelChangeThreadPriority(
                   worker_id, TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT) < 0) {
            (void) atomic_fetch_add_explicit(
                &fetch_background.worker_priority_failures, 1u,
                memory_order_relaxed);
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        fetch_background_atomic_max(
            &fetch_background.worker_perform_max_us, perform_us);
        fetch_background_atomic_max(
            setup_perform
                ? &fetch_background.worker_setup_perform_max_us
                : &fetch_background.worker_steady_perform_max_us,
            perform_us);
        if (perform_us > UINT64_C(33000)) {
            (void) atomic_fetch_add_explicit(
                &fetch_background.worker_perform_over_33ms, 1u,
                memory_order_relaxed);
        }
        if (perform_us > UINT64_C(100000)) {
            (void) atomic_fetch_add_explicit(
                &fetch_background.worker_perform_over_100ms, 1u,
                memory_order_relaxed);
        }
#endif
        (void) atomic_fetch_add_explicit(
            &fetch_background.worker_performs, 1u, memory_order_relaxed);
        atomic_store_explicit(
            &fetch_background.last_multi_code, (int) code,
            memory_order_relaxed);
        atomic_store_explicit(
            &fetch_background.last_running, running, memory_order_relaxed);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        service_started_us = fetch_background_now_us();
#endif
        fetch_background_collect_messages();
        fetch_background_service_preconnect();
        fetch_background_resume_streams();
        fetch_background_start_queued();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        service_us = fetch_background_now_us() - service_started_us;
        fetch_background_atomic_max(
            &fetch_background.worker_service_max_us, service_us);
#endif
        if (atomic_load_explicit(
                &fetch_background.stop_requested, memory_order_acquire)) {
            if (!fetch_background_has_worker_work()) break;
        }
        if (code != CURLM_OK) {
            for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
                FetchBackgroundSlot *slot = &fetch_background.slots[i];
                if (atomic_load_explicit(
                        &slot->state, memory_order_acquire)
                    == FETCH_BACKGROUND_SLOT_RUNNING) {
                    snprintf(slot->result.error,
                             sizeof(slot->result.error),
                             "background curl multi failed (%d)", (int) code);
                    fetch_background_slot_release_curl(slot);
                    fetch_background_publish_complete(
                        slot, false, CURLE_FAILED_INIT);
                }
            }
            continue;
        }
        /* PSP libcurl can report zero running handles during the first
           perform that transitions a freshly-added easy handle into its
           resolver state, without publishing CURLMSG_DONE. Descriptor state
           remains the ownership oracle: never sleep on the event flag while
           a queued/running descriptor still needs curl progress. */
        if (running > 0 || fetch_background_has_worker_work()) {
            int descriptors = 0;
            (void) atomic_fetch_add_explicit(
                &fetch_background.worker_polls, 1u,
                memory_order_relaxed);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t poll_started_us = fetch_background_now_us();
#endif
            CURLMcode poll_code = curl_multi_poll(
                fetch_background.multi, NULL, 0,
                FETCH_BACKGROUND_POLL_MS, &descriptors);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            fetch_background_atomic_max(
                &fetch_background.worker_poll_max_us,
                fetch_background_now_us() - poll_started_us);
            fetch_background_atomic_max(
                &fetch_background.worker_loop_max_us,
                fetch_background_now_us() - loop_started_us);
#endif
            atomic_store_explicit(
                &fetch_background.last_multi_code, (int) poll_code,
                memory_order_relaxed);
            continue;
        }
        u32 bits = 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        fetch_background_atomic_max(
            &fetch_background.worker_loop_max_us,
            fetch_background_now_us() - loop_started_us);
#endif
        (void) sceKernelWaitEventFlag(
            fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
            &bits, NULL);
    }
    return 0;
}

bool fetch_background_transport_available(void)
{
    return true;
}

bool fetch_background_transport_initialize(Budget *budget)
{
    if (fetch_background.initialized)
        return fetch_background.budget == budget;
    if (budget == NULL) return false;
    if (!curl_global_acquire(budget)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_log_printf(
                "tilefinch-background-transport: init=failed "
                "stage=curl-global code=0 free=%u largest=%u "
                "budget-remaining=%zu\n",
                (unsigned) sceKernelTotalFreeMemSize(),
                (unsigned) sceKernelMaxFreeMemSize(),
                budget_remaining(budget));
#endif
        return false;
    }
    const char *failure_stage = "unknown";
    int failure_code = 0;
#ifdef TILEFINCH_FETCH_CA_BLOB
    if (fetch_ca_bundle[0] != '\0' && !fetch_ca_blob_ensure()) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_log_printf(
                "tilefinch-background-transport: init=failed "
                "stage=ca-blob code=0 free=%u largest=%u "
                "budget-remaining=%zu\n",
                (unsigned) sceKernelTotalFreeMemSize(),
                (unsigned) sceKernelMaxFreeMemSize(),
                budget_remaining(budget));
#endif
        (void) curl_global_release();
        return false;
    }
#endif
    memset(&fetch_background, 0, sizeof(fetch_background));
    fetch_background.budget = budget;
    fetch_background.event = -1;
    fetch_background.thread = -1;
    atomic_init(&fetch_background.stop_requested, 0);
    atomic_init(&fetch_background.streaming_started, 0u);
    atomic_init(&fetch_background.fixed_started, 0u);
    atomic_init(&fetch_background.peak_streaming_active, 0u);
    atomic_init(&fetch_background.peak_fixed_active, 0u);
    atomic_init(&fetch_background.worker_performs, 0u);
    atomic_init(&fetch_background.worker_polls, 0u);
    atomic_init(&fetch_background.header_callbacks, 0u);
    atomic_init(&fetch_background.body_callbacks, 0u);
    atomic_init(&fetch_background.completions, 0u);
    atomic_init(&fetch_background.worker_perform_max_us, 0u);
    atomic_init(&fetch_background.worker_perform_over_33ms, 0u);
    atomic_init(&fetch_background.worker_perform_over_100ms, 0u);
    atomic_init(&fetch_background.worker_setup_performs, 0u);
    atomic_init(&fetch_background.worker_setup_perform_max_us, 0u);
    atomic_init(&fetch_background.worker_steady_perform_max_us, 0u);
    atomic_init(&fetch_background.worker_priority_failures, 0u);
    atomic_init(&fetch_background.worker_service_max_us, 0u);
    atomic_init(&fetch_background.worker_poll_max_us, 0u);
    atomic_init(&fetch_background.worker_loop_max_us, 0u);
    atomic_init(&fetch_background.last_multi_code, 0);
    atomic_init(&fetch_background.last_running, 0);
    atomic_init(
        &fetch_background.preconnect.state,
        FETCH_BACKGROUND_PRECONNECT_FREE);
    atomic_init(&fetch_background.preconnect.cancel_requested, 0);
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        atomic_init(
            &fetch_background.slots[i].state,
            FETCH_BACKGROUND_SLOT_FREE);
        atomic_init(&fetch_background.slots[i].generation, 0u);
        atomic_init(&fetch_background.slots[i].cancel_requested, 0);
        atomic_init(&fetch_background.slots[i].discard_requested, 0);
        atomic_init(&fetch_background.slots[i].received_body_bytes, 0u);
        atomic_init(
            &fetch_background.slots[i].chunk_state,
            FETCH_BACKGROUND_CHUNK_EMPTY);
        atomic_init(&fetch_background.slots[i].chunk_bytes, 0u);
        atomic_init(
            &fetch_background.slots[i].headers_state,
            FETCH_BACKGROUND_HEADERS_EMPTY);
        fetch_background.slots[i].buffer_index = -1;
    }
    if (!budget_reservation_acquire(
            &fetch_background.stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE,
            FETCH_BACKGROUND_THREAD_STACK_BYTES)) {
        failure_stage = "stack-reserve";
        goto fail;
    }
    fetch_background.multi = curl_multi_init();
    if (fetch_background.multi == NULL) {
        failure_stage = "curl-multi";
        goto fail;
    }
#if defined(TILEFINCH_PSP_HTTP2)
    (void) curl_multi_setopt(
        fetch_background.multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
    fetch_background.event = sceKernelCreateEventFlag(
        "tilefinch_transport", PSP_EVENT_WAITSINGLE, 0, NULL);
    if (fetch_background.event < 0) {
        failure_stage = "event";
        failure_code = fetch_background.event;
        goto fail;
    }
    fetch_background.thread = sceKernelCreateThread(
        "tilefinch_transport", fetch_background_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT,
        FETCH_BACKGROUND_THREAD_STACK_BYTES,
        PSP_THREAD_ATTR_USER, NULL);
    if (fetch_background.thread < 0) {
        failure_stage = "thread-create";
        failure_code = fetch_background.thread;
        goto fail;
    }
    failure_code = sceKernelStartThread(fetch_background.thread, 0, NULL);
    if (failure_code < 0) {
        failure_stage = "thread-start";
        goto fail;
    }
    fetch_background.initialized = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_log_printf(
            "tilefinch-background-transport: init=ready "
            "stream-buffers=lazy free=%u largest=%u "
            "budget-remaining=%zu\n",
            (unsigned) sceKernelTotalFreeMemSize(),
            (unsigned) sceKernelMaxFreeMemSize(),
            budget_remaining(budget));
#endif
    return true;

fail:
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_log_printf(
            "tilefinch-background-transport: init=failed "
            "stage=%s code=0x%08x free=%u largest=%u "
            "budget-remaining=%zu\n",
            failure_stage, (unsigned) failure_code,
            (unsigned) sceKernelTotalFreeMemSize(),
            (unsigned) sceKernelMaxFreeMemSize(),
            budget_remaining(budget));
#endif
    if (fetch_background.thread >= 0)
        (void) sceKernelDeleteThread(fetch_background.thread);
    if (fetch_background.event >= 0)
        (void) sceKernelDeleteEventFlag(fetch_background.event);
    if (fetch_background.multi != NULL)
        (void) curl_multi_cleanup(fetch_background.multi);
    for (unsigned i = 0; i < FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT; i++)
        budget_free(budget, fetch_background.stream_buffers[i]);
    for (unsigned i = 0; i < FETCH_BACKGROUND_ACTIVE_LIMIT; i++)
        budget_free(budget, fetch_background.fixed_buffers[i]);
    budget_reservation_release(&fetch_background.stack_reservation);
    memset(&fetch_background, 0, sizeof(fetch_background));
    (void) curl_global_release();
    return false;
}

static bool fetch_background_preconnect_begin(
    Budget *budget, const char *origin, long timeout_ms)
{
    if (origin == NULL || origin[0] == '\0' || timeout_ms <= 0
        || strlen(origin) >= sizeof(fetch_background.preconnect.origin)
        || atomic_load_explicit(
               &fetch_background_admission_open,
               memory_order_acquire) == 0
        || !fetch_background_transport_initialize(budget)) return false;
    FetchBackgroundPreconnect *preconnect = &fetch_background.preconnect;
    int expected = FETCH_BACKGROUND_PRECONNECT_FREE;
    if (!atomic_compare_exchange_strong_explicit(
            &preconnect->state, &expected,
            FETCH_BACKGROUND_PRECONNECT_PREPARING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    snprintf(preconnect->origin, sizeof(preconnect->origin), "%s", origin);
    preconnect->timeout_ms = timeout_ms;
    preconnect->result = CURLE_OK;
    atomic_store_explicit(
        &preconnect->cancel_requested, 0, memory_order_release);
    atomic_store_explicit(
        &preconnect->state, FETCH_BACKGROUND_PRECONNECT_QUEUED,
        memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
    return true;
}

static bool fetch_background_preconnect_poll(
    bool *complete, bool *success)
{
    if (!fetch_background.initialized || complete == NULL || success == NULL)
        return false;
    int state = atomic_load_explicit(
        &fetch_background.preconnect.state, memory_order_acquire);
    if (state == FETCH_BACKGROUND_PRECONNECT_FREE) return false;
    *complete = state == FETCH_BACKGROUND_PRECONNECT_CONNECTED
        || state == FETCH_BACKGROUND_PRECONNECT_FAILED;
    *success = state == FETCH_BACKGROUND_PRECONNECT_CONNECTED;
    return true;
}

static void fetch_background_preconnect_retire(void)
{
    if (!fetch_background.initialized) return;
    int state = atomic_load_explicit(
        &fetch_background.preconnect.state, memory_order_acquire);
    if (state == FETCH_BACKGROUND_PRECONNECT_FREE) return;
    atomic_store_explicit(
        &fetch_background.preconnect.cancel_requested, 1,
        memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
}

static bool fetch_background_copy_field(
    char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0) return false;
    if (source == NULL) {
        destination[0] = '\0';
        return true;
    }
    size_t length = strlen(source);
    if (length >= capacity) return false;
    memcpy(destination, source, length + 1u);
    return true;
}

static bool fetch_background_stream_request_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, bool single_hop)
{
    return fetch_background_transport_stream_shape_supported(
        url, request, maximum_bytes, timeout_ms, single_hop);
}

static uint64_t fetch_background_transport_enqueue_stream_internal(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, bool single_hop,
    size_t publication_bytes, bool foreground_media,
    FetchBackgroundEnqueueStatus *status)
{
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    if (!fetch_background.initialized) return 0;
    if (atomic_load_explicit(
            &fetch_background_admission_open,
            memory_order_acquire) == 0) {
        if (status != NULL)
            *status = FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED;
        return 0;
    }
    if (!fetch_background_stream_request_supported(
            url, request, maximum_bytes, timeout_ms, single_hop)) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
        return 0;
    }
    FetchBackgroundEnqueueStatus capacity =
        fetch_background_ensure_stream_capacity();
    if (capacity != FETCH_BACKGROUND_ENQUEUE_ADMITTED) {
        if (status != NULL) *status = capacity;
        return 0;
    }
    const char *method = request->method == NULL ? "GET" : request->method;
    size_t url_length = strlen(url);
    if (url_length >= FETCH_BACKGROUND_URL_CAPACITY) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
        return 0;
    }
    unsigned slot_limit = fetch_background_admission_slot_limit(
        foreground_media,
        atomic_load_explicit(
            &fetch_background_media_priority_active,
            memory_order_acquire) != 0);
    for (unsigned i = 0; i < slot_limit; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int expected = FETCH_BACKGROUND_SLOT_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, FETCH_BACKGROUND_SLOT_PREPARING,
                memory_order_acq_rel, memory_order_acquire)) continue;
        bool copied =
            fetch_background_copy_field(
                slot->initial_url, sizeof(slot->initial_url), url)
            && fetch_background_copy_field(
                slot->url, sizeof(slot->url), url)
            && fetch_background_copy_field(
                slot->method, sizeof(slot->method), method)
            && fetch_background_copy_field(
                slot->content_type, sizeof(slot->content_type),
                request->content_type)
            && fetch_background_copy_field(
                slot->extra_headers, sizeof(slot->extra_headers),
                request->extra_headers)
            && fetch_background_copy_field(
                slot->cookie, sizeof(slot->cookie), request->cookie)
            && fetch_background_copy_field(
                slot->referer, sizeof(slot->referer), request->referer)
            && fetch_background_copy_field(
                slot->origin, sizeof(slot->origin), request->origin)
            && fetch_background_copy_field(
                slot->accept, sizeof(slot->accept), request->accept)
            && fetch_background_copy_field(
                slot->user_agent, sizeof(slot->user_agent),
                request->user_agent)
            && fetch_background_copy_field(
                slot->if_none_match, sizeof(slot->if_none_match),
                request->if_none_match)
            && fetch_background_copy_field(
                slot->if_modified_since, sizeof(slot->if_modified_since),
                request->if_modified_since)
            && fetch_background_copy_field(
                slot->client_hint_tokens,
                sizeof(slot->client_hint_tokens),
                request->client_hint_tokens)
            && fetch_background_copy_field(
                slot->client_hint_origin,
                sizeof(slot->client_hint_origin),
                request->client_hint_origin)
            && fetch_background_copy_field(
                slot->sec_fetch_dest, sizeof(slot->sec_fetch_dest),
                request->sec_fetch_dest)
            && fetch_background_copy_field(
                slot->sec_fetch_mode, sizeof(slot->sec_fetch_mode),
                request->sec_fetch_mode)
            && fetch_background_copy_field(
                slot->sec_fetch_site, sizeof(slot->sec_fetch_site),
                request->sec_fetch_site);
        if (copied && request->body_length != 0) {
            memcpy(slot->body, request->body, request->body_length);
            slot->body[request->body_length] = '\0';
        } else {
            slot->body[0] = '\0';
        }
        if (!copied) {
            atomic_store_explicit(
                &slot->state, FETCH_BACKGROUND_SLOT_FREE,
                memory_order_release);
            if (status != NULL)
                *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
            return 0;
        }
        uint32_t generation = fetch_background_generation_next(
            atomic_load_explicit(
                &slot->generation, memory_order_relaxed));
        atomic_store_explicit(
            &slot->generation, generation, memory_order_release);
        slot->buffer_index = -1;
        slot->streaming = true;
        slot->single_hop = single_hop;
        slot->transfer_done = false;
        slot->curl_paused = false;
        slot->pause_started_us = 0;
        slot->allow_http_errors = request->allow_http_errors;
        slot->redirect_same_origin_only = !single_hop;
        slot->body_enabled = true;
        slot->body_length = request->body_length;
        slot->identity_encoding = request->identity_encoding;
        slot->force_fresh_connection = request->force_fresh_connection;
        slot->send_low_client_hints = request->send_low_client_hints;
        slot->send_client_hints = request->send_client_hints;
        slot->suppress_high_client_hints = false;
        slot->sec_fetch_user = request->sec_fetch_user;
        slot->upgrade_insecure_requests = request->upgrade_insecure_requests;
        slot->private_network.required = request->block_private_network;
        slot->private_network.blocked = false;
        slot->validator = request->redirect_url_validator;
        slot->maximum_bytes = maximum_bytes;
        slot->stream_chunk_target = publication_bytes == 0
            ? FETCH_BACKGROUND_STREAM_CHUNK_TARGET : publication_bytes;
        if (slot->stream_chunk_target > maximum_bytes)
            slot->stream_chunk_target = maximum_bytes;
        slot->timeout_ms = timeout_ms;
        slot->connect_timeout_ms = request->connect_timeout_ms;
        slot->started_us = fetch_background_now_us();
        slot->last_progress_us = slot->started_us;
        uint64_t duration = (uint64_t) timeout_ms * UINT64_C(1000);
        slot->deadline_us = duration > UINT64_MAX - slot->started_us
            ? UINT64_MAX : slot->started_us + duration;
        slot->redirects = 0;
        slot->redirect_transition_count = 0u;
        slot->redirect_loop_detected = false;
        slot->tls12_retry_attempted = false;
        slot->header_status = 0;
        slot->length = 0;
        slot->total_length = 0;
        slot->location[0] = '\0';
        slot->content_range[0] = '\0';
        slot->content_type_response[0] = '\0';
        slot->curl_error[0] = '\0';
        memset(&slot->result, 0, sizeof(slot->result));
        atomic_store_explicit(
            &slot->cancel_requested, 0, memory_order_release);
        atomic_store_explicit(
            &slot->discard_requested, 0, memory_order_release);
        atomic_store_explicit(
            &slot->received_body_bytes, 0u, memory_order_release);
        atomic_store_explicit(
            &slot->chunk_state, FETCH_BACKGROUND_CHUNK_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &slot->chunk_bytes, 0u, memory_order_release);
        atomic_store_explicit(
            &slot->headers_state, FETCH_BACKGROUND_HEADERS_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &slot->state, FETCH_BACKGROUND_SLOT_QUEUED,
            memory_order_release);
        (void) sceKernelSetEventFlag(
            fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
        (void) curl_multi_wakeup(fetch_background.multi);
#endif
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_ADMITTED;
        return fetch_background_make_id(i, generation);
    }
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_SATURATED;
    return 0;
}

uint64_t fetch_background_transport_enqueue_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, false,
        FETCH_BACKGROUND_STREAM_CHUNK_TARGET, false, NULL);
}

uint64_t fetch_background_transport_enqueue_stream_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, false,
        FETCH_BACKGROUND_STREAM_CHUNK_TARGET, false, status);
}

uint64_t fetch_background_transport_enqueue_media_stream_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, false,
        FETCH_BACKGROUND_STREAM_CHUNK_TARGET, true, status);
}

uint64_t fetch_background_transport_enqueue_stream_sized(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, size_t publication_bytes)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, false,
        publication_bytes, false, NULL);
}

uint64_t fetch_background_transport_enqueue_media_stream_sized_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, size_t publication_bytes,
    FetchBackgroundEnqueueStatus *status)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, false,
        publication_bytes, true, status);
}

uint64_t fetch_background_transport_enqueue_hop_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    return fetch_background_transport_enqueue_stream_internal(
        url, request, maximum_bytes, timeout_ms, true,
        FETCH_BACKGROUND_STREAM_CHUNK_TARGET, false, NULL);
}

bool fetch_background_transport_hop_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    return fetch_background.initialized
        && fetch_background_stream_request_supported(
               url, request, maximum_bytes, timeout_ms, true);
}

static uint64_t fetch_background_transport_enqueue_fixed_internal(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    bool foreground_media,
    FetchBackgroundEnqueueStatus *status)
{
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    if (!fetch_background.initialized) return 0;
    if (atomic_load_explicit(
            &fetch_background_admission_open,
            memory_order_acquire) == 0) {
        if (status != NULL)
            *status = FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED;
        return 0;
    }
    if (url == NULL || request == NULL
        || maximum_bytes == 0
        || maximum_bytes > FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES
        || timeout_ms <= 0 || !fetch_request_validate(request, NULL)
        || (request->method != NULL
            && strcasecmp(request->method, "GET") != 0)
        || request->body != NULL || request->body_length != 0
        || request->extra_headers != NULL || request->cookie != NULL
        || request->cookie_session != NULL
        || request->credentials != FETCH_CREDENTIALS_OMIT
        || request->redirect_url_validator == NULL
        || !request->redirect_url_validator(url)) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
        return 0;
    }
    /* Fixed-size media fetches need the larger buffers. Keep them out of the
       normal browsing footprint and allocate them only on the browser thread,
       before publishing any descriptor that can reference them. */
    bool allocated_now[FETCH_BACKGROUND_ACTIVE_LIMIT] = {false};
    for (unsigned i = 0; i < FETCH_BACKGROUND_ACTIVE_LIMIT; i++) {
        if (fetch_background.fixed_buffers[i] != NULL) continue;
        fetch_background.fixed_buffers[i] = budget_malloc_category(
            fetch_background.budget, BUDGET_CATEGORY_RESOURCE,
            FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES);
        if (fetch_background.fixed_buffers[i] == NULL) {
            for (unsigned j = 0; j < FETCH_BACKGROUND_ACTIVE_LIMIT; j++) {
                if (!allocated_now[j]) continue;
                budget_free(fetch_background.budget,
                            fetch_background.fixed_buffers[j]);
                fetch_background.fixed_buffers[j] = NULL;
            }
            if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_MEMORY;
            return 0;
        }
        allocated_now[i] = true;
    }
    size_t url_length = strlen(url);
    const char *referer = request->referer == NULL ? "" : request->referer;
    size_t referer_length = strlen(referer);
    if (url_length >= FETCH_BACKGROUND_URL_CAPACITY
        || referer_length >= FETCH_BACKGROUND_REFERER_CAPACITY) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
        return 0;
    }
    unsigned slot_limit = fetch_background_admission_slot_limit(
        foreground_media,
        atomic_load_explicit(
            &fetch_background_media_priority_active,
            memory_order_acquire) != 0);
    for (unsigned i = 0; i < slot_limit; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int expected = FETCH_BACKGROUND_SLOT_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, FETCH_BACKGROUND_SLOT_PREPARING,
                memory_order_acq_rel, memory_order_acquire)) continue;
        uint32_t generation = fetch_background_generation_next(
            atomic_load_explicit(
                &slot->generation, memory_order_relaxed));
        atomic_store_explicit(
            &slot->generation, generation, memory_order_release);
        slot->buffer_index = -1;
        slot->streaming = false;
        slot->single_hop = false;
        slot->transfer_done = false;
        slot->curl_paused = false;
        slot->pause_started_us = 0;
        slot->allow_http_errors = false;
        slot->redirect_same_origin_only = false;
        slot->body_enabled = false;
        slot->identity_encoding = false;
        /* Fixed-size media windows use this enqueue path. Preserve the
           caller's retry policy just as the streaming path does; clearing it
           here silently routed every trickle retry back onto the throttled
           HTTP/2 connection it was meant to replace. */
        slot->force_fresh_connection = request->force_fresh_connection;
        slot->send_low_client_hints = false;
        slot->send_client_hints = false;
        slot->suppress_high_client_hints = false;
        slot->validator = request->redirect_url_validator;
        slot->maximum_bytes = maximum_bytes;
        slot->timeout_ms = timeout_ms;
        slot->connect_timeout_ms = request->connect_timeout_ms;
        slot->started_us = fetch_background_now_us();
        slot->last_progress_us = slot->started_us;
        uint64_t duration = (uint64_t) timeout_ms * UINT64_C(1000);
        slot->deadline_us = duration > UINT64_MAX - slot->started_us
            ? UINT64_MAX : slot->started_us + duration;
        slot->redirects = 0;
        slot->redirect_transition_count = 0u;
        slot->redirect_loop_detected = false;
        slot->tls12_retry_attempted = false;
        slot->header_status = 0;
        slot->length = 0;
        slot->total_length = 0;
        memcpy(slot->initial_url, url, url_length + 1u);
        memcpy(slot->url, url, url_length + 1u);
        memcpy(slot->referer, referer, referer_length + 1u);
        slot->location[0] = '\0';
        slot->content_range[0] = '\0';
        slot->content_type_response[0] = '\0';
        slot->method[0] = '\0';
        slot->body[0] = '\0';
        slot->body_length = 0;
        slot->content_type[0] = '\0';
        slot->extra_headers[0] = '\0';
        slot->cookie[0] = '\0';
        slot->origin[0] = '\0';
        slot->accept[0] = '\0';
        slot->user_agent[0] = '\0';
        slot->sec_fetch_dest[0] = '\0';
        slot->sec_fetch_mode[0] = '\0';
        slot->sec_fetch_site[0] = '\0';
        slot->sec_fetch_user = false;
        slot->upgrade_insecure_requests = false;
        slot->curl_error[0] = '\0';
        memset(&slot->result, 0, sizeof(slot->result));
        atomic_store_explicit(
            &slot->cancel_requested, 0, memory_order_release);
        atomic_store_explicit(
            &slot->discard_requested, 0, memory_order_release);
        atomic_store_explicit(
            &slot->received_body_bytes, 0u, memory_order_release);
        atomic_store_explicit(
            &slot->chunk_state, FETCH_BACKGROUND_CHUNK_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &slot->chunk_bytes, 0u, memory_order_release);
        atomic_store_explicit(
            &slot->state, FETCH_BACKGROUND_SLOT_QUEUED,
            memory_order_release);
        (void) sceKernelSetEventFlag(
            fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
        (void) curl_multi_wakeup(fetch_background.multi);
#endif
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_ADMITTED;
        return fetch_background_make_id(i, generation);
    }
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_SATURATED;
    return 0;
}

uint64_t fetch_background_transport_enqueue(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    return fetch_background_transport_enqueue_fixed_internal(
        url, request, maximum_bytes, timeout_ms, false, NULL);
}

uint64_t fetch_background_transport_enqueue_media_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    return fetch_background_transport_enqueue_fixed_internal(
        url, request, maximum_bytes, timeout_ms, true, status);
}

bool fetch_background_transport_progress(
    uint64_t request_id, FetchBackgroundProgress *progress)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || progress == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    int state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (!fetch_background_slot_matches(slot, request_id)
        || state == FETCH_BACKGROUND_SLOT_FREE
        || state == FETCH_BACKGROUND_SLOT_PREPARING) return false;
    *progress = (FetchBackgroundProgress) {
        .received_body_bytes = atomic_load_explicit(
            &slot->received_body_bytes, memory_order_acquire),
        .available_body_bytes = slot->streaming
            && atomic_load_explicit(
                   &slot->chunk_state, memory_order_acquire)
                   == FETCH_BACKGROUND_CHUNK_READY
            ? atomic_load_explicit(
                  &slot->chunk_bytes, memory_order_acquire) : 0,
        .status_code = state == FETCH_BACKGROUND_SLOT_COMPLETE
            ? slot->result.status_code : 0,
        .active = state == FETCH_BACKGROUND_SLOT_QUEUED
            || state == FETCH_BACKGROUND_SLOT_RUNNING,
        .complete = state == FETCH_BACKGROUND_SLOT_COMPLETE
    };
    return true;
}

bool fetch_background_transport_metrics(
    FetchBackgroundTransportMetrics *metrics)
{
    if (!fetch_background.initialized || metrics == NULL) return false;
    size_t occupied_slots = 0;
    size_t queued_slots = 0;
    size_t running_slots = 0;
    size_t complete_slots = 0;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        int state = atomic_load_explicit(
            &fetch_background.slots[i].state, memory_order_acquire);
        if (state != FETCH_BACKGROUND_SLOT_FREE) occupied_slots++;
        if (state == FETCH_BACKGROUND_SLOT_QUEUED) queued_slots++;
        if (state == FETCH_BACKGROUND_SLOT_RUNNING) running_slots++;
        if (state == FETCH_BACKGROUND_SLOT_COMPLETE) complete_slots++;
    }
    uint64_t worker_run_clocks = 0;
    unsigned worker_thread_preemptions = 0;
    int worker_priority = -1;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    SceKernelThreadRunStatus status = {0};
    status.size = sizeof(status);
    if (fetch_background.thread >= 0
        && sceKernelReferThreadRunStatus(fetch_background.thread, &status)
               >= 0) {
        worker_run_clocks = ((uint64_t) status.runClocks.hi << 32)
            | status.runClocks.low;
        worker_thread_preemptions = status.threadPreemptCount;
        worker_priority = status.currentPriority;
    }
#endif
    *metrics = (FetchBackgroundTransportMetrics) {
        .occupied_slots = occupied_slots,
        .queued_slots = queued_slots,
        .running_slots = running_slots,
        .complete_slots = complete_slots,
        .streaming_started = atomic_load_explicit(
            &fetch_background.streaming_started, memory_order_relaxed),
        .fixed_started = atomic_load_explicit(
            &fetch_background.fixed_started, memory_order_relaxed),
        .peak_streaming_active = atomic_load_explicit(
            &fetch_background.peak_streaming_active, memory_order_relaxed),
        .peak_fixed_active = atomic_load_explicit(
            &fetch_background.peak_fixed_active, memory_order_relaxed),
        .worker_performs = atomic_load_explicit(
            &fetch_background.worker_performs, memory_order_relaxed),
        .worker_polls = atomic_load_explicit(
            &fetch_background.worker_polls, memory_order_relaxed),
        .header_callbacks = atomic_load_explicit(
            &fetch_background.header_callbacks, memory_order_relaxed),
        .body_callbacks = atomic_load_explicit(
            &fetch_background.body_callbacks, memory_order_relaxed),
        .completions = atomic_load_explicit(
            &fetch_background.completions, memory_order_relaxed),
        .worker_perform_max_us = atomic_load_explicit(
            &fetch_background.worker_perform_max_us, memory_order_relaxed),
        .worker_perform_over_33ms = atomic_load_explicit(
            &fetch_background.worker_perform_over_33ms,
            memory_order_relaxed),
        .worker_perform_over_100ms = atomic_load_explicit(
            &fetch_background.worker_perform_over_100ms,
            memory_order_relaxed),
        .worker_setup_performs = atomic_load_explicit(
            &fetch_background.worker_setup_performs, memory_order_relaxed),
        .worker_setup_perform_max_us = atomic_load_explicit(
            &fetch_background.worker_setup_perform_max_us,
            memory_order_relaxed),
        .worker_steady_perform_max_us = atomic_load_explicit(
            &fetch_background.worker_steady_perform_max_us,
            memory_order_relaxed),
        .worker_priority_failures = atomic_load_explicit(
            &fetch_background.worker_priority_failures,
            memory_order_relaxed),
        .worker_service_max_us = atomic_load_explicit(
            &fetch_background.worker_service_max_us, memory_order_relaxed),
        .worker_poll_max_us = atomic_load_explicit(
            &fetch_background.worker_poll_max_us, memory_order_relaxed),
        .worker_loop_max_us = atomic_load_explicit(
            &fetch_background.worker_loop_max_us, memory_order_relaxed),
        .worker_run_clocks = worker_run_clocks,
        .worker_thread_preemptions = worker_thread_preemptions,
        .worker_priority = worker_priority,
        .last_multi_code = atomic_load_explicit(
            &fetch_background.last_multi_code, memory_order_relaxed),
        .last_running = atomic_load_explicit(
            &fetch_background.last_running, memory_order_relaxed)
    };
    return true;
}

static void fetch_background_copy_response_metadata(
    FetchResult *destination, const FetchBackgroundResult *source)
{
    if (destination == NULL || source == NULL) return;
    destination->status_code = source->status_code;
    destination->transport_code = source->transport_code;
    destination->tls_verify_result = source->tls_verify_result;
    destination->negotiated_http_version = source->negotiated_http_version;
    destination->new_connections = source->new_connections;
    destination->tls_handshake_measured = source->tls_handshake_measured;
    destination->tls_handshake_us = source->tls_handshake_us;
    destination->tls_connection_reuse_known =
        source->tls_connection_reuse_known;
    destination->tls_connection_reused = source->tls_connection_reused;
    destination->tls12_compatibility_retry =
        source->tls12_compatibility_retry;
    destination->timed_out = source->timed_out;
    snprintf(destination->effective_url, sizeof(destination->effective_url),
             "%s", source->effective_url);
    snprintf(destination->content_type, sizeof(destination->content_type),
             "%s", source->content_type);
    snprintf(destination->etag, sizeof(destination->etag), "%s",
             source->etag);
    snprintf(destination->last_modified,
             sizeof(destination->last_modified), "%s",
             source->last_modified);
    snprintf(destination->cf_mitigated,
             sizeof(destination->cf_mitigated), "%s",
             source->cf_mitigated);
    snprintf(destination->accept_ch, sizeof(destination->accept_ch), "%s",
             source->accept_ch);
    snprintf(destination->critical_ch,
             sizeof(destination->critical_ch), "%s",
             source->critical_ch);
    snprintf(destination->server, sizeof(destination->server), "%s",
             source->server);
    snprintf(destination->cf_ray, sizeof(destination->cf_ray), "%s",
             source->cf_ray);
    size_t header_length = source->response_headers_length;
    if (header_length >= sizeof(destination->response_headers)) {
        header_length = sizeof(destination->response_headers) - 1u;
    }
    memcpy(destination->response_headers, source->response_headers,
           header_length);
    destination->response_headers[header_length] = '\0';
    destination->response_headers_length = header_length;
    destination->security = source->security;
    snprintf(destination->response_referrer_policy,
             sizeof(destination->response_referrer_policy), "%s",
             source->response_referrer_policy);
    destination->response_referrer_policy_header_present =
        source->response_referrer_policy_header_present;
    destination->response_referrer_policy_metadata_valid =
        source->response_referrer_policy_metadata_valid;
    destination->response_security_headers_truncated =
        source->response_security_headers_truncated;
    destination->set_cookies_truncated = source->set_cookies_truncated;
}

static void fetch_background_copy_media_headers(
    FetchBackgroundMediaResponse *destination,
    const FetchBackgroundSlot *slot)
{
    if (destination == NULL || slot == NULL) return;
    const FetchBackgroundResult *source = &slot->result;
    destination->status_code = source->status_code;
    size_t url_length = strnlen(
        source->effective_url, sizeof(destination->effective_url) - 1u);
    memcpy(destination->effective_url, source->effective_url, url_length);
    destination->effective_url[url_length] = '\0';
    snprintf(destination->content_range,
             sizeof(destination->content_range), "%s",
             slot->content_range);
    destination->content_length[0] = '\0';
    size_t offset = 0;
    static const char content_length_name[] = "content-length";
    while (offset < source->response_headers_length) {
        const char *line = source->response_headers + offset;
        const char *newline = memchr(
            line, '\n', source->response_headers_length - offset);
        size_t length = newline == NULL
            ? source->response_headers_length - offset
            : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL
            && (size_t) (colon - line) == sizeof(content_length_name) - 1u
            && strncasecmp(
                   line, content_length_name,
                   sizeof(content_length_name) - 1u) == 0) {
            copy_header_value(
                destination->content_length,
                sizeof(destination->content_length), colon + 1,
                length - (size_t) (colon + 1 - line));
            break;
        }
        offset += length + (newline == NULL ? 0u : 1u);
    }
}

static void fetch_background_copy_media_response(
    FetchBackgroundMediaResponse *destination,
    const FetchBackgroundSlot *slot)
{
    if (destination == NULL || slot == NULL) return;
    fetch_background_copy_media_headers(destination, slot);
    const FetchBackgroundResult *source = &slot->result;
    destination->length = source->length;
    destination->received_body_bytes = source->received_body_bytes;
    destination->new_connections = source->new_connections;
    destination->success = source->success;
    destination->timed_out = source->timed_out;
    destination->tls_handshake_measured = source->tls_handshake_measured;
    destination->tls_handshake_us = source->tls_handshake_us;
    destination->tls_connection_reuse_known =
        source->tls_connection_reuse_known;
    destination->tls_connection_reused = source->tls_connection_reused;
    destination->tls12_compatibility_retry =
        source->tls12_compatibility_retry;
    snprintf(destination->error, sizeof(destination->error), "%s",
             source->error);
}

bool fetch_background_transport_take_headers(
    uint64_t request_id, FetchResult *metadata)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || metadata == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!slot->streaming
        || !fetch_background_slot_matches(slot, request_id)) return false;
    int expected = FETCH_BACKGROUND_HEADERS_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->headers_state, &expected,
            FETCH_BACKGROUND_HEADERS_TAKEN,
            memory_order_acq_rel, memory_order_acquire)) return false;
    fetch_background_copy_response_metadata(metadata, &slot->result);
    return true;
}

bool fetch_background_transport_take_media_headers(
    uint64_t request_id, FetchBackgroundMediaResponse *metadata)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || metadata == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!slot->streaming
        || !fetch_background_slot_matches(slot, request_id)) return false;
    int expected = FETCH_BACKGROUND_HEADERS_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->headers_state, &expected,
            FETCH_BACKGROUND_HEADERS_TAKEN,
            memory_order_acq_rel, memory_order_acquire)) return false;
    fetch_background_copy_media_headers(metadata, slot);
    return true;
}

bool fetch_background_transport_take_chunk(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, size_t *length)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || destination == NULL
        || length == NULL || index >= FETCH_BACKGROUND_REQUEST_LIMIT) {
        return false;
    }
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!slot->streaming
        || !fetch_background_slot_matches(slot, request_id)) return false;
    size_t available = atomic_load_explicit(
        &slot->chunk_bytes, memory_order_acquire);
    if (available == 0 || available > capacity) return false;
    int expected = FETCH_BACKGROUND_CHUNK_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->chunk_state, &expected, FETCH_BACKGROUND_CHUNK_TAKING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    unsigned char *source = fetch_background_slot_buffer(slot);
    if (source == NULL) {
        atomic_store_explicit(
            &slot->chunk_state, FETCH_BACKGROUND_CHUNK_READY,
            memory_order_release);
        return false;
    }
    memcpy(destination, source, available);
    slot->length = 0;
    *length = available;
    atomic_store_explicit(
        &slot->chunk_bytes, 0u, memory_order_release);
    atomic_store_explicit(
        &slot->chunk_state, FETCH_BACKGROUND_CHUNK_EMPTY,
        memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
    return true;
}

static void fetch_background_release_slot(FetchBackgroundSlot *slot)
{
    atomic_store_explicit(
        &slot->state, FETCH_BACKGROUND_SLOT_FREE, memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
}

bool fetch_background_transport_take_fetch_result_consumed(
    uint64_t request_id, Budget *budget, FetchResult *result,
    size_t consumed_body_bytes)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || budget == NULL || result == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!slot->streaming
        || !fetch_background_slot_matches(slot, request_id)) return false;
    int expected = FETCH_BACKGROUND_SLOT_COMPLETE;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, FETCH_BACKGROUND_SLOT_TAKING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    bool assembled = consumed_body_bytes == slot->result.length
        && consumed_body_bytes == slot->total_length;
    result->budget = budget;
    result->received_body_bytes = slot->result.received_body_bytes;
    fetch_background_copy_response_metadata(result, &slot->result);
    snprintf(result->error, sizeof(result->error), "%s",
             assembled ? slot->result.error
                       : "background streamed response was incomplete");
    bool cookies_ok = true;
    bool cookie_capacity_rejected = false;
    bool redirect_response =
        fetch_background_redirect_cookie_overflow_fatal(
            slot->single_hop, slot->result.status_code,
            slot->location[0] != '\0');
    for (size_t i = 0; i < slot->response_cookie_count; i++) {
        if (result->set_cookie_count >= FETCH_RESPONSE_COOKIE_CAPACITY) {
            if (redirect_response) {
                cookies_ok = false;
                cookie_capacity_rejected = true;
            } else {
                result->set_cookies_truncated = true;
            }
            break;
        }
        const FetchBackgroundCookie *cookie = &slot->response_cookies[i];
        size_t url_length = strlen(slot->url);
        char *value = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            (size_t) cookie->value_length + 1u);
        char *url = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            url_length + 1u);
        if (value == NULL || url == NULL) {
            budget_free(budget, value);
            budget_free(budget, url);
            cookies_ok = false;
            break;
        }
        memcpy(value,
               slot->response_cookie_storage + cookie->value_offset,
               (size_t) cookie->value_length + 1u);
        memcpy(url, slot->url, url_length + 1u);
        result->set_cookies[result->set_cookie_count] = value;
        result->set_cookie_urls[result->set_cookie_count] = url;
        result->set_cookie_count++;
    }
    if (!cookies_ok) {
        while (result->set_cookie_count != 0) {
            size_t at = --result->set_cookie_count;
            budget_free(budget, result->set_cookies[at]);
            budget_free(budget, result->set_cookie_urls[at]);
            result->set_cookies[at] = NULL;
            result->set_cookie_urls[at] = NULL;
        }
        if (cookie_capacity_rejected) {
            snprintf(
                result->error, sizeof(result->error),
                "%s", "redirect response cookies exceeded transport bounds");
        } else {
            snprintf(result->error, sizeof(result->error), "%s",
                     "background response cookies exceeded shared memory budget");
        }
    }
    FetchHandshakeSample handshake = {
        .classified = result->tls_connection_reuse_known,
        .measured = result->tls_handshake_measured,
        .handshake_us = result->tls_handshake_us,
        .reused = result->tls_connection_reused
    };
    fetch_tls_counters_add(&fetch_tls_counters, &handshake);
    bool success = assembled && cookies_ok && slot->result.success;
    fetch_background_release_slot(slot);
    return success;
}

bool fetch_background_transport_take_media_result_consumed(
    uint64_t request_id, FetchBackgroundMediaResponse *result,
    size_t consumed_body_bytes)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || result == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!slot->streaming
        || !fetch_background_slot_matches(slot, request_id)) return false;
    int expected = FETCH_BACKGROUND_SLOT_COMPLETE;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, FETCH_BACKGROUND_SLOT_TAKING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    bool assembled = consumed_body_bytes == slot->result.length
        && consumed_body_bytes == slot->total_length;
    fetch_background_copy_media_response(result, slot);
    result->success = assembled && slot->result.success;
    if (!assembled) {
        snprintf(result->error, sizeof(result->error), "%s",
                 "background streamed response was incomplete");
    }
    FetchHandshakeSample handshake = {
        .classified = result->tls_connection_reuse_known,
        .measured = result->tls_handshake_measured,
        .handshake_us = result->tls_handshake_us,
        .reused = result->tls_connection_reused
    };
    fetch_tls_counters_add(&fetch_tls_counters, &handshake);
    bool success = result->success;
    fetch_background_release_slot(slot);
    return success;
}

bool fetch_background_transport_take_media(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundMediaResponse *result)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || result == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (slot->streaming) return false;
    int expected = FETCH_BACKGROUND_SLOT_COMPLETE;
    if (!fetch_background_slot_matches(slot, request_id)
        || !atomic_compare_exchange_strong_explicit(
               &slot->state, &expected, FETCH_BACKGROUND_SLOT_TAKING,
               memory_order_acq_rel, memory_order_acquire)) return false;
    fetch_background_copy_media_response(result, slot);
    unsigned char *source = fetch_background_slot_buffer(slot);
    bool copied = result->length <= capacity
        && (result->length == 0 || destination != NULL)
        && source != NULL;
    if (copied && result->length != 0)
        memcpy(destination, source, result->length);
    if (!copied) {
        result->success = false;
        snprintf(result->error, sizeof(result->error), "%s",
                 "background result destination was too small");
    }
    FetchHandshakeSample handshake = {
        .classified = result->tls_connection_reuse_known,
        .measured = result->tls_handshake_measured,
        .handshake_us = result->tls_handshake_us,
        .reused = result->tls_connection_reused
    };
    fetch_tls_counters_add(&fetch_tls_counters, &handshake);
    fetch_background_release_slot(slot);
    return true;
}

bool fetch_background_transport_take_fetch_result(
    uint64_t request_id, Budget *budget, FetchResult *result)
{
    return result != NULL
        && fetch_background_transport_take_fetch_result_consumed(
               request_id, budget, result, result->length);
}

bool fetch_background_transport_take(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundResult *result)
{
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized || result == NULL
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (slot->streaming) return false;
    int expected = FETCH_BACKGROUND_SLOT_COMPLETE;
    if (!fetch_background_slot_matches(slot, request_id)
        || !atomic_compare_exchange_strong_explicit(
               &slot->state, &expected, FETCH_BACKGROUND_SLOT_TAKING,
               memory_order_acq_rel, memory_order_acquire)) return false;
    *result = slot->result;
    unsigned char *source = fetch_background_slot_buffer(slot);
    bool copied = result->length <= capacity
        && (result->length == 0 || destination != NULL)
        && source != NULL;
    if (copied && result->length != 0) {
        memcpy(destination, source, result->length);
    }
    if (!copied) {
        result->success = false;
        snprintf(result->error, sizeof(result->error), "%s",
                 "background result destination was too small");
    }
    FetchHandshakeSample handshake = {
        .classified = result->tls_connection_reuse_known,
        .measured = result->tls_handshake_measured,
        .handshake_us = result->tls_handshake_us,
        .reused = result->tls_connection_reused
    };
    fetch_tls_counters_add(&fetch_tls_counters, &handshake);
    atomic_store_explicit(
        &slot->state, FETCH_BACKGROUND_SLOT_FREE, memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
    return true;
}

static bool fetch_background_result_reserve(
    Budget *budget, FetchResult *result, size_t needed,
    size_t maximum_bytes)
{
    if (needed > maximum_bytes || needed == SIZE_MAX) return false;
    size_t required = needed + 1u;
    if (required <= result->capacity) return true;
    size_t capacity = result->capacity == 0 ? 64u * 1024u : result->capacity;
    while (capacity < required) {
        size_t next = capacity > maximum_bytes / 2u
            ? maximum_bytes + 1u : capacity * 2u;
        if (next <= capacity) return false;
        capacity = next;
    }
    if (capacity > maximum_bytes + 1u) capacity = maximum_bytes + 1u;
    char *data = result->data == NULL
        ? budget_malloc_category(
              budget, BUDGET_CATEGORY_RESOURCE, capacity)
        : budget_realloc_category(
              budget, BUDGET_CATEGORY_RESOURCE, result->data, capacity);
    if (data == NULL) return false;
    result->data = data;
    result->capacity = capacity;
    return true;
}

bool fetch_background_request_cancelable(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel,
    void *cancel_opaque, FetchResult *result)
{
    if (budget == NULL || result == NULL
        || !fetch_background_transport_initialize(budget)) return false;
    memset(result, 0, sizeof(*result));
    result->budget = budget;
    uint64_t id = fetch_background_transport_enqueue_stream(
        url, request, maximum_bytes, timeout_ms);
    if (id == 0) {
        snprintf(result->error, sizeof(result->error), "%s",
                 "background request was not admitted");
        return false;
    }
    for (;;) {
        if (cancel != NULL && cancel(cancel_opaque)) {
            (void) fetch_background_transport_cancel(id, "request cancelled");
            snprintf(result->error, sizeof(result->error), "%s",
                     "request cancelled");
            budget_free(budget, result->data);
            result->data = NULL;
            result->capacity = 0;
            result->length = 0;
            return false;
        }
        FetchBackgroundProgress progress = {0};
        if (!fetch_background_transport_progress(id, &progress)) {
            snprintf(result->error, sizeof(result->error), "%s",
                     "background request retired unexpectedly");
            budget_free(budget, result->data);
            result->data = NULL;
            result->capacity = 0;
            result->length = 0;
            return false;
        }
        if (progress.available_body_bytes != 0) {
            if (progress.available_body_bytes
                    > maximum_bytes - result->length
                || !fetch_background_result_reserve(
                       budget, result,
                       result->length + progress.available_body_bytes,
                       maximum_bytes)) {
                (void) fetch_background_transport_cancel(
                    id, "response exceeded shared memory budget");
                snprintf(result->error, sizeof(result->error), "%s",
                         "response exceeded shared memory budget");
                budget_free(budget, result->data);
                result->data = NULL;
                result->capacity = 0;
                result->length = 0;
                return false;
            }
            size_t taken = 0;
            if (!fetch_background_transport_take_chunk(
                    id, (unsigned char *) result->data + result->length,
                    result->capacity - result->length, &taken)) {
                continue;
            }
            result->length += taken;
            result->data[result->length] = '\0';
            continue;
        }
        if (progress.complete) {
            if (result->data == NULL
                && !fetch_background_result_reserve(
                       budget, result, 0, maximum_bytes)) {
                (void) fetch_background_transport_cancel(
                    id, "empty response allocation failed");
                snprintf(result->error, sizeof(result->error), "%s",
                         "empty response allocation failed");
                return false;
            }
            result->data[result->length] = '\0';
            return fetch_background_transport_take_fetch_result(
                id, budget, result);
        }
        fetch_background_transport_wait(1u);
    }
}

bool fetch_background_transport_cancel(
    uint64_t request_id, const char *reason)
{
    (void) reason;
    unsigned index = fetch_background_id_slot(request_id);
    if (!fetch_background.initialized
        || index >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    FetchBackgroundSlot *slot = &fetch_background.slots[index];
    if (!fetch_background_slot_matches(slot, request_id)) return false;
    atomic_store_explicit(
        &slot->discard_requested, 1, memory_order_release);
    atomic_store_explicit(
        &slot->cancel_requested, 1, memory_order_release);
    if (slot->streaming) {
        int chunk = FETCH_BACKGROUND_CHUNK_READY;
        if (atomic_compare_exchange_strong_explicit(
                &slot->chunk_state, &chunk,
                FETCH_BACKGROUND_CHUNK_TAKING,
                memory_order_acq_rel, memory_order_acquire)) {
            slot->length = 0;
            atomic_store_explicit(
                &slot->chunk_bytes, 0u, memory_order_release);
            atomic_store_explicit(
                &slot->chunk_state, FETCH_BACKGROUND_CHUNK_EMPTY,
                memory_order_release);
        }
    }
    int expected = FETCH_BACKGROUND_SLOT_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, FETCH_BACKGROUND_SLOT_FREE,
            memory_order_acq_rel, memory_order_acquire)) {
        expected = FETCH_BACKGROUND_SLOT_COMPLETE;
        (void) atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, FETCH_BACKGROUND_SLOT_FREE,
            memory_order_acq_rel, memory_order_acquire);
    }
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
    return true;
}

void fetch_background_transport_wait(unsigned milliseconds)
{
    if (milliseconds == 0) return;
    uint64_t delay = (uint64_t) milliseconds * 1000u;
    (void) sceKernelDelayThread(
        delay > UINT_MAX ? UINT_MAX : (SceUInt) delay);
}

static bool fetch_background_all_free(void)
{
    if (atomic_load_explicit(
            &fetch_background.preconnect.state, memory_order_acquire)
        != FETCH_BACKGROUND_PRECONNECT_FREE) return false;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        if (atomic_load_explicit(
                &fetch_background.slots[i].state, memory_order_acquire)
            != FETCH_BACKGROUND_SLOT_FREE) return false;
    }
    return true;
}

size_t fetch_background_transport_active_operations(void)
{
    if (!fetch_background.initialized) return 0;
    size_t active = atomic_load_explicit(
        &fetch_background.preconnect.state, memory_order_acquire)
            == FETCH_BACKGROUND_PRECONNECT_FREE ? 0u : 1u;
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        if (atomic_load_explicit(
                &fetch_background.slots[i].state, memory_order_acquire)
            != FETCH_BACKGROUND_SLOT_FREE)
            active++;
    }
    return active;
}

void fetch_background_transport_request_quiesce(void)
{
    if (!fetch_background.initialized) return;
    fetch_background_preconnect_retire();
    for (unsigned i = 0; i < FETCH_BACKGROUND_REQUEST_LIMIT; i++) {
        FetchBackgroundSlot *slot = &fetch_background.slots[i];
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state == FETCH_BACKGROUND_SLOT_FREE) continue;
        uint64_t id = fetch_background_make_id(
            i, atomic_load_explicit(
                   &slot->generation, memory_order_acquire));
        (void) fetch_background_transport_cancel(id, "transport quiesce");
    }
}

bool fetch_background_transport_is_quiesced(void)
{
    return !fetch_background.initialized || fetch_background_all_free();
}

bool fetch_background_transport_take_network_regression_hint(void)
{
    return fetch_background.initialized
        && atomic_exchange_explicit(
               &fetch_background.network_regression_hint, 0,
               memory_order_acq_rel) != 0;
}

void fetch_background_transport_set_admission(bool open)
{
    atomic_store_explicit(
        &fetch_background_admission_open, open ? 1 : 0,
        memory_order_release);
}

void fetch_background_transport_set_media_priority(bool active)
{
    atomic_store_explicit(
        &fetch_background_media_priority_active, active ? 1 : 0,
        memory_order_release);
}

bool fetch_background_transport_quiesce(unsigned timeout_ms)
{
    if (!fetch_background.initialized) return true;
    fetch_background_transport_request_quiesce();
    uint64_t started_us = fetch_background_now_us();
    uint64_t timeout_us = (uint64_t) timeout_ms * 1000u;
    while (!fetch_background_transport_is_quiesced()) {
        if (fetch_background_now_us() - started_us >= timeout_us)
            return false;
        (void) sceKernelDelayThread(1000u);
    }
    return true;
}

bool fetch_background_transport_shutdown(unsigned timeout_ms)
{
    if (!fetch_background.initialized) return true;
    if (!fetch_background_transport_quiesce(timeout_ms)) return false;
    atomic_store_explicit(
        &fetch_background.stop_requested, 1, memory_order_release);
    (void) sceKernelSetEventFlag(
        fetch_background.event, FETCH_BACKGROUND_EVENT_WAKE);
#if LIBCURL_VERSION_NUM >= 0x074400
    (void) curl_multi_wakeup(fetch_background.multi);
#endif
    uint64_t wait64 = (uint64_t) timeout_ms * 1000u;
    SceUInt wait_us = wait64 > UINT_MAX ? UINT_MAX : (SceUInt) wait64;
    if (psp_thread_wait_end_bounded(
            fetch_background.thread, wait_us) < 0)
        return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The media session can be the final transport consumer, after the last
       navigation report. Emit one bounded aggregate here, on the browser
       thread and before deleting the worker; never log from the worker hot
       path itself. */
    FetchBackgroundTransportMetrics final_metrics = {0};
    if (fetch_background_transport_metrics(&final_metrics)) {
        psp_log_printf(
            "tilefinch-background-transport-worker-final: "
            "perform-max=%uus over-33ms=%u over-100ms=%u "
            "setup=%u/%uus steady-max=%uus priority-failures=%u "
            "service-max=%uus poll-max=%uus loop-max=%uus\n",
            final_metrics.worker_perform_max_us,
            final_metrics.worker_perform_over_33ms,
            final_metrics.worker_perform_over_100ms,
            final_metrics.worker_setup_performs,
            final_metrics.worker_setup_perform_max_us,
            final_metrics.worker_steady_perform_max_us,
            final_metrics.worker_priority_failures,
            final_metrics.worker_service_max_us,
            final_metrics.worker_poll_max_us,
            final_metrics.worker_loop_max_us);
    }
#endif
    (void) sceKernelDeleteThread(fetch_background.thread);
    (void) sceKernelDeleteEventFlag(fetch_background.event);
    (void) curl_multi_cleanup(fetch_background.multi);
    Budget *budget = fetch_background.budget;
    for (unsigned i = 0; i < FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT; i++)
        budget_free(budget, fetch_background.stream_buffers[i]);
    for (unsigned i = 0; i < FETCH_BACKGROUND_ACTIVE_LIMIT; i++)
        budget_free(budget, fetch_background.fixed_buffers[i]);
    budget_reservation_release(&fetch_background.stack_reservation);
    memset(&fetch_background, 0, sizeof(fetch_background));
    return curl_global_release();
}

#else

bool fetch_background_transport_available(void) { return false; }
bool fetch_background_transport_initialize(Budget *budget)
{
    (void) budget;
    return false;
}
bool fetch_background_transport_take_fetch_result_consumed(
    uint64_t request_id, Budget *budget, FetchResult *result,
    size_t consumed_body_bytes)
{
    (void) request_id; (void) budget; (void) result;
    (void) consumed_body_bytes;
    return false;
}
bool fetch_background_transport_take_media_result_consumed(
    uint64_t request_id, FetchBackgroundMediaResponse *result,
    size_t consumed_body_bytes)
{
    (void) request_id; (void) result; (void) consumed_body_bytes;
    return false;
}
bool fetch_background_transport_take_media(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundMediaResponse *result)
{
    (void) request_id; (void) destination; (void) capacity; (void) result;
    return false;
}
uint64_t fetch_background_transport_enqueue(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    return 0;
}
uint64_t fetch_background_transport_enqueue_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    return 0;
}
uint64_t fetch_background_transport_enqueue_stream_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    return 0;
}
uint64_t fetch_background_transport_enqueue_media_stream_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    return 0;
}
uint64_t fetch_background_transport_enqueue_stream_sized(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, size_t publication_bytes)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    (void) publication_bytes;
    return 0;
}
uint64_t fetch_background_transport_enqueue_media_stream_sized_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, size_t publication_bytes,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    (void) publication_bytes;
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    return 0;
}
uint64_t fetch_background_transport_enqueue_media_diagnosed(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    return 0;
}
uint64_t fetch_background_transport_enqueue_hop_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    return 0;
}
bool fetch_background_transport_hop_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    return false;
}
bool fetch_background_transport_progress(
    uint64_t request_id, FetchBackgroundProgress *progress)
{
    (void) request_id; (void) progress;
    return false;
}
bool fetch_background_transport_metrics(
    FetchBackgroundTransportMetrics *metrics)
{
    (void) metrics;
    return false;
}
bool fetch_background_transport_take_headers(
    uint64_t request_id, FetchResult *metadata)
{
    (void) request_id; (void) metadata;
    return false;
}
bool fetch_background_transport_take_media_headers(
    uint64_t request_id, FetchBackgroundMediaResponse *metadata)
{
    (void) request_id; (void) metadata;
    return false;
}
bool fetch_background_transport_take_chunk(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, size_t *length)
{
    (void) request_id; (void) destination; (void) capacity; (void) length;
    return false;
}
bool fetch_background_transport_take_fetch_result(
    uint64_t request_id, Budget *budget, FetchResult *result)
{
    (void) request_id; (void) budget; (void) result;
    return false;
}
bool fetch_background_transport_take(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundResult *result)
{
    (void) request_id; (void) destination; (void) capacity; (void) result;
    return false;
}
bool fetch_background_request_cancelable(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel,
    void *cancel_opaque, FetchResult *result)
{
    (void) budget; (void) url; (void) request; (void) maximum_bytes;
    (void) timeout_ms; (void) cancel; (void) cancel_opaque; (void) result;
    return false;
}
bool fetch_background_transport_cancel(
    uint64_t request_id, const char *reason)
{
    (void) request_id; (void) reason;
    return false;
}
void fetch_background_transport_wait(unsigned milliseconds)
{
    (void) milliseconds;
}
size_t fetch_background_transport_active_operations(void) { return 0; }
void fetch_background_transport_request_quiesce(void) {}
bool fetch_background_transport_is_quiesced(void) { return true; }
bool fetch_background_transport_take_network_regression_hint(void)
{
    return false;
}
void fetch_background_transport_set_admission(bool open) { (void) open; }
void fetch_background_transport_set_media_priority(bool active)
{
    (void) active;
}
bool fetch_background_transport_quiesce(unsigned timeout_ms)
{
    (void) timeout_ms;
    return true;
}
bool fetch_background_transport_shutdown(unsigned timeout_ms)
{
    (void) timeout_ms;
    return true;
}
static bool fetch_background_preconnect_begin(
    Budget *budget, const char *origin, long timeout_ms)
{
    (void) budget; (void) origin; (void) timeout_ms;
    return false;
}
static bool fetch_background_preconnect_poll(
    bool *complete, bool *success)
{
    (void) complete; (void) success;
    return false;
}
static void fetch_background_preconnect_retire(void) {}

#endif
