#ifndef TILEFINCH_BACKGROUND_SLOT_POLICY_H
#define TILEFINCH_BACKGROUND_SLOT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/fetch.h"

#define FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS 2u
#define FETCH_BACKGROUND_RESPONSE_COOKIE_BYTES (16u * 1024u)
#define FETCH_BACKGROUND_STREAM_PUBLICATION_MAX (48u * 1024u)

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

static inline unsigned fetch_background_admission_slot_limit(
    bool foreground_media, bool media_priority_active)
{
    return media_priority_active && !foreground_media
        ? FETCH_BACKGROUND_REQUEST_LIMIT
              - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS
        : FETCH_BACKGROUND_REQUEST_LIMIT;
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
