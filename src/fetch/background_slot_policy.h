#ifndef TILEFINCH_BACKGROUND_SLOT_POLICY_H
#define TILEFINCH_BACKGROUND_SLOT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#define FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS 2u
#define FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES (16u * 1024u)
#define FETCH_BACKGROUND_STREAM_PUBLICATION_MAX (48u * 1024u)

/* The curl handle must still exist when these facts are sampled. Keep the
   numeric error even if a streamed tail is published after handle cleanup. */
static inline void fetch_background_terminal_tls_metadata(
    FetchBackgroundResult *result, long code, bool verification_available,
    long verification_result, bool peer_verification_failed)
{
    result->transport_code = code;
    result->tls_verify_result_available = verification_available;
    result->tls_verify_result = verification_result;
    result->tls_verification_failed = peer_verification_failed
        || (verification_available && verification_result != 0);
}

/* Every scheduled response is bounded by its byte reservation. The handoff
   buffer need not be larger than that reservation (notably a one-byte media
   range probe), while ordinary schedulers retain the 64 KiB ceiling. */
static inline size_t fetch_background_scheduler_chunk_capacity(
    size_t maximum_reserved_bytes)
{
    return maximum_reserved_bytes < FETCH_BACKGROUND_STREAM_BUFFER_BYTES
        ? maximum_reserved_bytes : FETCH_BACKGROUND_STREAM_BUFFER_BYTES;
}

/* Called only by the Budget-owning thread while its new fixed descriptor is
   PREPARING. Keep previously admitted slabs if a later allocation fails;
   release only slabs acquired by this attempt. The fixed array and bound are
   shared with the host ownership test. */
static inline FetchBackgroundEnqueueStatus
fetch_background_fixed_buffers_ensure(
    Budget *budget, unsigned char *buffers[FETCH_BACKGROUND_ACTIVE_LIMIT],
    size_t required, size_t *added)
{
    if (added != NULL) *added = 0;
    if (budget == NULL || buffers == NULL || required == 0
        || required > FETCH_BACKGROUND_ACTIVE_LIMIT) {
        return FETCH_BACKGROUND_ENQUEUE_SATURATED;
    }
    size_t allocated = 0;
    for (size_t i = 0; i < FETCH_BACKGROUND_ACTIVE_LIMIT; i++)
        if (buffers[i] != NULL) allocated++;
    if (allocated >= required) return FETCH_BACKGROUND_ENQUEUE_ADMITTED;

    bool allocated_now[FETCH_BACKGROUND_ACTIVE_LIMIT] = {false};
    for (size_t i = 0;
         i < FETCH_BACKGROUND_ACTIVE_LIMIT && allocated < required; i++) {
        if (buffers[i] != NULL) continue;
        buffers[i] = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES);
        if (buffers[i] == NULL) {
            for (size_t j = 0; j < FETCH_BACKGROUND_ACTIVE_LIMIT; j++) {
                if (!allocated_now[j]) continue;
                budget_free(budget, buffers[j]);
                buffers[j] = NULL;
            }
            if (added != NULL) *added = 0;
            return FETCH_BACKGROUND_ENQUEUE_MEMORY;
        }
        allocated_now[i] = true;
        allocated++;
        if (added != NULL) (*added)++;
    }
    return FETCH_BACKGROUND_ENQUEUE_ADMITTED;
}

/* A worker that failed to start is tried again by the next request that
   needs it, but at most once a second: each attempt repeats curl's global
   setup, the CA blob and the thread creation. */
#define FETCH_BACKGROUND_START_RETRY_US UINT64_C(1000000)
static inline bool fetch_background_start_retry_due(
    uint64_t now_us, uint64_t last_failure_us)
{
    return last_failure_us == 0 || now_us < last_failure_us
        || now_us - last_failure_us >= FETCH_BACKGROUND_START_RETRY_US;
}

/* Setup work and firmware services needed by a socket poll run below the
   browser. Keep at least 4 ms between completed 2 ms checkpoint donations;
   this bounds requested caller latency without elevating a long TLS call. */
#define FETCH_BACKGROUND_SETUP_YIELD_US 2000u
#define FETCH_BACKGROUND_SETUP_YIELD_GAP_US UINT64_C(4000)
static inline bool fetch_background_setup_yield_due(
    bool active, uint64_t now_us, uint64_t last_finished_us)
{
    return active && (last_finished_us == 0
        || (now_us >= last_finished_us
            && now_us - last_finished_us >= FETCH_BACKGROUND_SETUP_YIELD_GAP_US));
}

/* Credit only the idle-progress clock while another connection monopolizes
   the shared worker. The absolute request deadline remains unchanged. */
static inline uint64_t fetch_background_progress_after_setup(
    uint64_t last_progress, uint64_t setup_start, uint64_t setup_end)
{
    if (setup_end <= setup_start || last_progress >= setup_end)
        return last_progress;
    if (last_progress > setup_start) return setup_end;
    return setup_end - (setup_start - last_progress);
}

/* Only transport-owned time is eligible for a transport timeout. A published
   chunk belongs to the consumer, even before curl asks to write again. */
static inline bool fetch_background_deadline_eligible(
    bool transfer_done, bool curl_paused, uint64_t pause_started_us)
{
    return !transfer_done && !curl_paused && pause_started_us == 0;
}

typedef enum {
    FETCH_BACKGROUND_EXPIRY_NONE = 0,
    FETCH_BACKGROUND_EXPIRY_DEADLINE,
    FETCH_BACKGROUND_EXPIRY_STALL
} FetchBackgroundExpiry;

/* Whether a running transfer ends now, by FetchWorkerTiming (fetch.h).
   `default_stall_seconds` is the default timing's stall once a status line
   has arrived. */
static inline FetchBackgroundExpiry fetch_background_expiry(
    FetchWorkerTiming timing, bool headers_received,
    long stall_abort_seconds, long default_stall_seconds,
    uint64_t now_us, uint64_t deadline_us, uint64_t last_progress_us)
{
    bool deadline_applies =
        timing != FETCH_WORKER_TIMING_STREAM || !headers_received;
    if (deadline_applies && now_us >= deadline_us)
        return FETCH_BACKGROUND_EXPIRY_DEADLINE;
    long stall_seconds = stall_abort_seconds;
    if (timing == FETCH_WORKER_TIMING_DEFAULT && stall_seconds <= 0)
        stall_seconds = headers_received ? default_stall_seconds : 0;
    if (stall_seconds <= 0 || now_us < last_progress_us)
        return FETCH_BACKGROUND_EXPIRY_NONE;
    uint64_t window_us = (uint64_t) stall_seconds * UINT64_C(1000000);
    return now_us - last_progress_us >= window_us
        ? FETCH_BACKGROUND_EXPIRY_STALL : FETCH_BACKGROUND_EXPIRY_NONE;
}

/* A stream's bytes become visible to the browser when the slot holds its
   publication target, or once no byte has arrived for this long, so a slow
   stream (an EventSource, a document whose server flushes its head early)
   is latency-bounded as well as size-bounded. */
#define FETCH_BACKGROUND_STREAM_IDLE_PUBLISH_US UINT64_C(50000)

static inline bool fetch_background_stream_idle_publish_due(
    size_t unpublished_bytes, bool chunk_empty, bool transfer_done,
    uint64_t now_us, uint64_t last_progress_us)
{
    return unpublished_bytes != 0 && chunk_empty && !transfer_done
        && now_us >= last_progress_us
        && now_us - last_progress_us
               >= FETCH_BACKGROUND_STREAM_IDLE_PUBLISH_US;
}

static inline void fetch_background_end_consumer_pause(
    uint64_t now_us, uint64_t *pause_started_us,
    uint64_t *deadline_us, uint64_t *last_progress_us)
{
    if (*pause_started_us == 0) return;
    uint64_t elapsed = now_us >= *pause_started_us
        ? now_us - *pause_started_us : 0;
    *deadline_us = elapsed > UINT64_MAX - *deadline_us
        ? UINT64_MAX : *deadline_us + elapsed;
    *last_progress_us = now_us;
    *pause_started_us = 0;
}

typedef enum {
    FETCH_BACKGROUND_COOKIE_CAPTURED = 0,
    FETCH_BACKGROUND_COOKIE_TRUNCATED,
    FETCH_BACKGROUND_COOKIE_REJECTED
} FetchBackgroundCookieCapture;

static inline FetchBackgroundCookieCapture
fetch_background_cookie_capture_policy(
    size_t value_length, size_t cookie_count, size_t storage_bytes)
{
    if (value_length == 0 || value_length >= FETCH_SET_COOKIE_LIMIT)
        return FETCH_BACKGROUND_COOKIE_REJECTED;
    if (cookie_count >= FETCH_RESPONSE_COOKIE_CAPACITY
        || storage_bytes > FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES
        || value_length + 1u
             > FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES - storage_bytes) {
        return FETCH_BACKGROUND_COOKIE_TRUNCATED;
    }
    return FETCH_BACKGROUND_COOKIE_CAPTURED;
}

/* Which descriptors a request may claim. Claims take the lowest free index
   below the lane's limit, so the top descriptor is the blocking lane's
   alone and the two below it are reserved for foreground media while media
   priority is active:
     ordinary: 0..4, or 0..2 under media priority;
     media:    0..4;
     blocking: 0..5.
   A blocking request is one the browser thread waits for (a sync XHR, its
   CORS preflight, a module import). A descriptor frees only when the
   scheduler that owns its request is pumped, and a blocking wait pumps
   only its own scheduler, so without the reserve a page's six image or
   stylesheet hops would hold every descriptor until the blocking request
   timed out. At most one blocking request runs at a time: the script that
   made it is suspended until it returns. */
#define FETCH_BACKGROUND_BLOCKING_RESERVED_SLOTS 1u

typedef enum {
    FETCH_BACKGROUND_LANE_ORDINARY = 0,
    FETCH_BACKGROUND_LANE_MEDIA,
    FETCH_BACKGROUND_LANE_BLOCKING
} FetchBackgroundLane;

static inline unsigned fetch_background_admission_slot_limit(
    FetchBackgroundLane lane, bool media_priority_active)
{
    if (lane == FETCH_BACKGROUND_LANE_BLOCKING)
        return FETCH_BACKGROUND_REQUEST_LIMIT;
    unsigned shared = FETCH_BACKGROUND_REQUEST_LIMIT
        - FETCH_BACKGROUND_BLOCKING_RESERVED_SLOTS;
    return media_priority_active && lane == FETCH_BACKGROUND_LANE_ORDINARY
        ? shared - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS : shared;
}

/* A single-hop request deliberately exposes a 3xx response as its final
   response. Cookie truncation there is therefore the same bounded loss as on
   a 200 response. Only a redirect this transport will follow needs the atomic
   intermediate-cookie rule. */
static inline bool fetch_background_redirect_cookie_overflow_fatal(
    bool single_hop, long status_code, bool has_location)
{
    if (single_hop || !has_location) return false;
    return status_code == 301 || status_code == 302
        || status_code == 303 || status_code == 307
        || status_code == 308;
}

/* Once the terminating blank line has published a header snapshot, libcurl
   may still route HTTP trailers through the header callback. A trailer is
   never part of the already-published response-header authority. */
static inline bool fetch_background_header_line_ignored(
    bool header_block_complete, bool header_snapshot_published,
    bool status_line)
{
    return header_block_complete
        && (header_snapshot_published || !status_line);
}

static inline bool fetch_background_header_block_publishable(
    bool single_hop, long status_code, bool has_location)
{
    return single_hop || status_code < 300 || status_code >= 400
        || !has_location;
}

/* The 64 KiB worker buffer needs one libcurl write callback (normally 16
   KiB) of spare capacity after the publication threshold is crossed. */
static inline size_t fetch_background_stream_publication_target(
    size_t requested, size_t maximum_bytes)
{
    size_t target = requested == 0
        || requested > FETCH_BACKGROUND_STREAM_PUBLICATION_MAX
        ? FETCH_BACKGROUND_STREAM_PUBLICATION_MAX : requested;
    return target > maximum_bytes ? maximum_bytes : target;
}

static inline uint32_t fetch_background_generation_next(uint32_t current)
{
    uint32_t next = current + 1u;
    return next == 0 ? 1u : next;
}

static inline unsigned fetch_background_request_id_slot(uint64_t request_id)
{
    unsigned encoded = (unsigned) (request_id & UINT64_C(0xff));
    return encoded == 0 ? FETCH_BACKGROUND_REQUEST_LIMIT : encoded - 1u;
}

static inline uint32_t fetch_background_request_id_generation(
    uint64_t request_id)
{
    return (uint32_t) (request_id >> 8);
}

static inline uint64_t fetch_background_request_id_make(
    unsigned slot, uint32_t generation)
{
    return ((uint64_t) generation << 8) | (uint64_t) (slot + 1u);
}

#endif
