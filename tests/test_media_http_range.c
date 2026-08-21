/*
 * The bounded media range source against a real HTTP server.
 *
 * tests/test_media_mp4.c covers this file through a substituted synchronous
 * transport, which is exactly the seam that skips the scheduler: the issue,
 * the poll, the completion check, the admission of the response and the
 * install into the window are all untested there. That gap shipped a defect
 * where a completed 256 KiB window was installed with its length read out of a
 * FetchResult that had already been destroyed, so the cache held the right
 * bytes and claimed to be empty. The device found it; this finds it now.
 *
 * The same seam hides a second class: a fragmented stream whose next moof is
 * in a transport window the source has not fetched. tests/test_media_mp4.c
 * builds sidx fixtures, but its transport answers every read from memory, so
 * the window load there can never block and the retry that a playing pipeline
 * depends on is never taken. The fragmented cases below cross both kinds of
 * boundary at once, over the real scheduler.
 *
 * tests/run_media_http_range_test.py starts the server and passes its port,
 * the fragmented fixture's length, and its shape.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/platform.h"
#include "../src/media_backend_psp_policy.h"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("media-range-test failed at %s:%d: %s\n", \
                   __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static unsigned char expected_byte(uint64_t at)
{
    return (unsigned char) ((at * 31u + 7u) & 0xFFu);
}

static void test_video_lookahead_policy(void)
{
    CHECK(psp_media_video_lookahead_limit(true, 0) == 1u);
    CHECK(psp_media_video_lookahead_limit(
              true, PSP_MEDIA_BUFFER_STARTUP_TARGET_US - 1u) == 1u);
    CHECK(psp_media_video_lookahead_limit(
              true, PSP_MEDIA_BUFFER_STARTUP_TARGET_US) == 2u);
    CHECK(psp_media_video_lookahead_limit(false, 0) == 2u);
}

static void test_optional_second_lookahead_budget_fallback(void)
{
    const char *url = "https://media.invalid/video.mp4";
    MediaHttpRangeOptions options = {
        .cache_bytes = 64u * 1024u,
        .lookahead_windows = 2u,
        .initial_lookahead_windows = 1u
    };
    char error[256] = {0};
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    /* State, URL, range URL, referrer, active cache, first successor: the
       next allocation is exactly the optional second successor. */
    budget_inject_failure_after(&budget, 6u);
    MediaHttpRange *range = media_http_range_create(
        &budget, NULL, url, 512u * 1024u, &options,
        error, sizeof(error));
    CHECK(range != NULL);
    media_http_range_set_lookahead_limit(range, 2u);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_slots == 1u
          && stats.lookahead_fetch_limit == 1u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);

    budget_init(&budget, 8u * 1024u * 1024u);
    range = media_http_range_create(
        &budget, NULL, url, 512u * 1024u, &options,
        error, sizeof(error));
    CHECK(range != NULL);
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_slots == 1u
          && stats.lookahead_fetch_limit == 1u);
    media_http_range_set_lookahead_limit(range, 2u);
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_slots == 2u
          && stats.lookahead_fetch_limit == 2u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_buffering_policy_hysteresis(void)
{
    const uint64_t origin = UINT64_C(1000000);
    PspMediaBufferPolicyInput input = {
        .playing = true,
        .source_blocked = true,
        .fill_pending = true,
        .now_us = origin,
        .remaining_us = UINT64_C(30000000)
    };
    PspMediaBufferPolicyDecision decision =
        psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP
          && decision.starved_since_us == origin);

    /* A sub-debounce Wi-Fi gap is invisible and leaves no armed transition. */
    input.starved_since_us = decision.starved_since_us;
    input.now_us = origin + PSP_MEDIA_BUFFER_DEBOUNCE_US - 1u;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP);
    input.source_blocked = false;
    input.fill_pending = false;
    input.starved_since_us = decision.starved_since_us;
    input.now_us++;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP
          && decision.starved_since_us == 0);
    input.starved_since_us = decision.starved_since_us;

    /* A sustained empty source opens once, then requires useful buffered
       time and a continuous stable interval before closing. */
    input.source_blocked = true;
    input.fill_pending = true;
    input.now_us += UINT64_C(100000);
    decision = psp_media_buffer_policy(input);
    input.starved_since_us = decision.starved_since_us;
    input.now_us += PSP_MEDIA_BUFFER_DEBOUNCE_US;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_BEGIN);

    input.buffering = true;
    input.startup = false;
    input.buffer_events = 1u;
    input.source_blocked = false;
    input.fill_pending = true;
    input.starved_since_us = 0;
    input.ready_since_us = 0;
    input.network_ahead_us = PSP_MEDIA_BUFFER_TARGET_US;
    input.decoded_ahead_us = PSP_MEDIA_BUFFER_DECODE_READY_US;
    input.now_us += UINT64_C(100000);
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP
          && decision.ready_since_us == input.now_us);
    input.ready_since_us = decision.ready_since_us;
    input.now_us += PSP_MEDIA_BUFFER_STABLE_US - 1u;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP);

    /* One bad refill sample restarts the stability interval instead of
       producing a one-frame BUFFERING/off/BUFFERING flash. */
    input.source_blocked = true;
    input.now_us++;
    input.ready_since_us = decision.ready_since_us;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_KEEP
          && decision.ready_since_us == 0);
    input.source_blocked = false;
    input.now_us += UINT64_C(100000);
    input.ready_since_us = 0;
    decision = psp_media_buffer_policy(input);
    input.ready_since_us = decision.ready_since_us;
    input.now_us += PSP_MEDIA_BUFFER_STABLE_US;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_END);

    /* Startup intentionally asks the same fixed windows for an extra second
       of runway. It does not change later first-stall behavior. */
    input.buffering = true;
    input.startup = true;
    input.pause_after_next_frame = false;
    input.buffer_events = 1u;
    input.ready_since_us = 0;
    input.network_ahead_us = PSP_MEDIA_BUFFER_TARGET_US;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.target_us == PSP_MEDIA_BUFFER_STARTUP_TARGET_US
          && decision.ready_since_us == 0);
    input.network_ahead_us = PSP_MEDIA_BUFFER_STARTUP_TARGET_US;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.ready_since_us == input.now_us);

    /* Repeated trouble deliberately raises both the reserve and the stable
       interval, while pausing always dismisses the transient surface. */
    input.buffering = true;
    input.startup = false;
    input.buffer_events = 3u;
    input.ready_since_us = 0;
    input.network_ahead_us = PSP_MEDIA_BUFFER_TARGET_US;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.target_us == PSP_MEDIA_BUFFER_REPEAT_TARGET_US
          && decision.stable_us == PSP_MEDIA_BUFFER_REPEAT_STABLE_US
          && decision.ready_since_us == 0);
    input.pause_after_next_frame = true;
    decision = psp_media_buffer_policy(input);
    CHECK(decision.action == PSP_MEDIA_BUFFER_END
          && decision.ready_since_us == 0
          && decision.starved_since_us == 0);
}

static void test_buffering_burst_and_slow_recovery_scenario(void)
{
    const uint64_t tick_us = UINT64_C(50000);
    PspMediaBufferPolicyInput input = {
        .playing = true,
        .fill_pending = true,
        .remaining_us = UINT64_C(60000000),
        .decoded_ahead_us = PSP_MEDIA_BUFFER_DECODE_READY_US,
        .now_us = UINT64_C(1000000)
    };
    unsigned begins = 0;
    unsigned ends = 0;

#define APPLY_BUFFER_TICK(blocked, ahead) do { \
    input.source_blocked = (blocked); \
    input.network_ahead_us = (ahead); \
    PspMediaBufferPolicyDecision next = psp_media_buffer_policy(input); \
    input.starved_since_us = next.starved_since_us; \
    input.ready_since_us = next.ready_since_us; \
    if (next.action == PSP_MEDIA_BUFFER_BEGIN) { \
        CHECK(!input.buffering); \
        input.buffering = true; \
        input.buffer_events++; \
        begins++; \
    } else if (next.action == PSP_MEDIA_BUFFER_END) { \
        CHECK(input.buffering); \
        input.buffering = false; \
        ends++; \
    } \
    input.now_us += tick_us; \
} while (0)

    /* Repeated 250 ms radio gaps are below the 350 ms visibility debounce. */
    for (unsigned burst = 0; burst < 3u; burst++) {
        for (unsigned tick = 0; tick < 5u; tick++)
            APPLY_BUFFER_TICK(true, 0);
        for (unsigned tick = 0; tick < 10u; tick++)
            APPLY_BUFFER_TICK(false, PSP_MEDIA_BUFFER_TARGET_US);
    }
    CHECK(begins == 0 && ends == 0 && !input.buffering);

    /* A real outage opens exactly one surface. Remaining starved samples do
       not repeatedly dispatch BEGIN while it is already visible. */
    for (unsigned tick = 0; tick < 20u; tick++)
        APPLY_BUFFER_TICK(true, 0);
    CHECK(begins == 1 && ends == 0 && input.buffering);

    /* Slow recovery does not close early. A one-tick regression after the
       target is first reached resets the stable interval, preventing the
       black player chrome from oscillating at a burst boundary. */
    for (unsigned tick = 0; tick < 20u; tick++) {
        uint64_t ahead = (uint64_t) tick
            * PSP_MEDIA_BUFFER_TARGET_US / 19u;
        APPLY_BUFFER_TICK(false, ahead);
    }
    CHECK(input.buffering && ends == 0);
    APPLY_BUFFER_TICK(true, PSP_MEDIA_BUFFER_TARGET_US);
    CHECK(input.buffering && input.ready_since_us == 0);
    for (unsigned tick = 0;
         tick < PSP_MEDIA_BUFFER_STABLE_US / tick_us + 2u; tick++)
        APPLY_BUFFER_TICK(false, PSP_MEDIA_BUFFER_TARGET_US);
    CHECK(begins == 1 && ends == 1 && !input.buffering);

    for (unsigned tick = 0; tick < 5u; tick++)
        APPLY_BUFFER_TICK(true, 0);
    APPLY_BUFFER_TICK(false, PSP_MEDIA_BUFFER_TARGET_US);
    CHECK(begins == 1 && ends == 1 && !input.buffering);
#undef APPLY_BUFFER_TICK
}

static void test_range_window_liveness_policy(void)
{
    /* A healthy-but-slow link keeps every useful byte. Throughput alone is
       not evidence that reconnecting will improve the route. */
    CHECK(media_http_window_liveness(
              false, true, 0, UINT64_C(12000000), 0)
          == MEDIA_HTTP_WINDOW_WAIT);
    /* A demanded window with no byte progress receives bounded fresh
       connections. The first is prompt, while later attempts back off so a
       longer radio outage is not made worse by connection churn. */
    CHECK(media_http_window_liveness(
              false, true, MEDIA_HTTP_STARVED_NO_PROGRESS_US,
              MEDIA_HTTP_STARVED_NO_PROGRESS_US, 0)
          == MEDIA_HTTP_WINDOW_RECONNECT);
    CHECK(media_http_window_liveness(
              false, true, MEDIA_HTTP_STARVED_NO_PROGRESS_US,
              MEDIA_HTTP_STARVED_NO_PROGRESS_US,
              1u) == MEDIA_HTTP_WINDOW_WAIT);
    CHECK(media_http_window_liveness(
              false, true, 2u * MEDIA_HTTP_STARVED_NO_PROGRESS_US,
              3u * MEDIA_HTTP_STARVED_NO_PROGRESS_US,
              1u) == MEDIA_HTTP_WINDOW_RECONNECT);
    CHECK(media_http_window_liveness(
              false, true, UINT64_C(20000000), UINT64_C(20000000),
              MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS)
          == MEDIA_HTTP_WINDOW_WAIT);
    /* One-byte trickles cannot keep an incident alive forever. */
    CHECK(media_http_window_liveness(
              false, true, 0,
              MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US,
              0)
          == MEDIA_HTTP_WINDOW_FAIL);
    CHECK(media_http_window_liveness(
              true, true, UINT64_MAX, UINT64_MAX,
              MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS)
          == MEDIA_HTTP_WINDOW_WAIT);
    CHECK(media_http_window_liveness(
              false, false, UINT64_MAX, UINT64_MAX,
              MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS)
          == MEDIA_HTTP_WINDOW_WAIT);

    /* The production tracker accepts the same observations without a socket
       or wall-clock sleep. This deliberately runs far below the historical
       throughput floor while bytes keep arriving: useful progress wins. */
    MediaHttpWindowTracker tracker = {0};
    uint64_t now_us = UINT64_C(1000000);
    media_http_window_tracker_request_started(&tracker, now_us);
    media_http_window_tracker_demand(&tracker, now_us);
    for (size_t step = 1; step <= 10u; step++) {
        now_us += UINT64_C(1500000);
        CHECK(media_http_window_tracker_observe(
                  &tracker, now_us, step * 1024u, false, true)
              == MEDIA_HTTP_WINDOW_WAIT);
    }
    CHECK(media_http_window_tracker_observe(
              &tracker, now_us, 10u * 1024u, true, true)
          == MEDIA_HTTP_WINDOW_WAIT);

    /* A dead physical request is replaced three times with bounded backoff,
       but the logical demand's original deadline and retry count survive each
       replacement. After the third replacement it remains recoverable until
       that absolute deadline rather than failing at roughly eight seconds. */
    media_http_window_tracker_reset(&tracker);
    now_us = UINT64_C(1000000);
    media_http_window_tracker_request_started(&tracker, now_us);
    media_http_window_tracker_demand(&tracker, now_us);
    for (unsigned reconnect = 0;
         reconnect < MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS; reconnect++) {
        now_us += media_http_window_reconnect_delay_us(reconnect);
        CHECK(media_http_window_tracker_observe(
                  &tracker, now_us, 0, false, true)
              == MEDIA_HTTP_WINDOW_RECONNECT);
        media_http_window_tracker_reconnected(&tracker);
        media_http_window_tracker_request_started(&tracker, now_us);
    }
    now_us += UINT64_C(10000000);
    CHECK(media_http_window_tracker_observe(
              &tracker, now_us, 0, false, true)
          == MEDIA_HTTP_WINDOW_WAIT
          && tracker.reconnects
               == MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS);
    now_us = tracker.demanded_started_us
        + MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US;
    CHECK(media_http_window_tracker_observe(
              &tracker, now_us, 0, false, true)
          == MEDIA_HTTP_WINDOW_FAIL);

    /* Descriptor starvation has no connection to replace. It still reaches
       the absolute logical-window deadline, and a newly selected window gets
       a clean incident scope. */
    media_http_window_tracker_reset(&tracker);
    now_us = UINT64_C(5000000);
    media_http_window_tracker_demand(&tracker, now_us);
    CHECK(media_http_window_tracker_observe(
              &tracker,
              now_us + MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US - 1u,
              0, false, false) == MEDIA_HTTP_WINDOW_WAIT);
    CHECK(media_http_window_tracker_observe(
              &tracker,
              now_us + MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US,
              0, false, false) == MEDIA_HTTP_WINDOW_FAIL);
    media_http_window_tracker_reset(&tracker);
    CHECK(!tracker.demanded && tracker.reconnects == 0
          && tracker.last_progress_bytes == 0);
}

static bool cadence_url_allowed(const char *url)
{
    return url != NULL && strncmp(url, "http://127.0.0.1:", 17u) == 0;
}

static MediaHttpRange *open_range_cancellable(
    Budget *budget, const char *mode, int port, uint64_t length,
    size_t cache_bytes, FetchCancelCallback cancel, void *cancel_opaque)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/%s/media.mp4", port, mode);
    MediaHttpRangeOptions options = {
        .cache_bytes = cache_bytes,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .cancel = cancel,
        .cancel_opaque = cancel_opaque
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, NULL, url, length, &options, error, sizeof(error));
    if (range == NULL) printf("open %s failed: %s\n", mode, error);
    return range;
}

static MediaHttpRange *open_range(
    Budget *budget, const char *mode, int port, uint64_t length,
    size_t cache_bytes)
{
    return open_range_cancellable(
        budget, mode, port, length, cache_bytes, NULL, NULL);
}

static MediaHttpRange *open_range_with_lookahead(
    Budget *budget, const char *mode, int port, uint64_t length,
    size_t cache_bytes)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/%s/media.mp4", port, mode);
    MediaHttpRangeOptions options = {
        .cache_bytes = cache_bytes,
        .lookahead_windows = 1,
        .stream_publication_bytes = 16u * 1024u,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .url_validator = cadence_url_allowed
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, NULL, url, length, &options, error, sizeof(error));
    if (range == NULL) printf("open %s failed: %s\n", mode, error);
    return range;
}

static MediaHttpRange *open_range_with_two_lookaheads(
    Budget *budget, int port, uint64_t length, size_t cache_bytes)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/query200/media.mp4", port);
    MediaHttpRangeOptions options = {
        .cache_bytes = cache_bytes,
        .lookahead_windows = 2u,
        .initial_lookahead_windows = 1u,
        .stream_publication_bytes = 16u * 1024u,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .url_validator = cadence_url_allowed
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, NULL, url, length, &options, error, sizeof(error));
    if (range == NULL) printf("open two-lookahead failed: %s\n", error);
    return range;
}

static MediaHttpRange *open_cadence_source(
    Budget *budget, const char *mode, int port, uint64_t length,
    bool lookahead, size_t publication_bytes)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/%s/media.mp4", port, mode);
    MediaHttpRangeOptions options = {
        .cache_bytes = 256u * 1024u,
        .lookahead_windows = lookahead ? 2u : 0u,
        .initial_lookahead_windows = lookahead ? 1u : 0u,
        .stream_publication_bytes = publication_bytes,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        /* Shipping media always carries the googlevideo host validator. Use
           an equivalent bounded-origin policy here so the shared native
           stream-shape gate exercises the same contract. */
        .url_validator = cadence_url_allowed
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, NULL, url, length, &options, error, sizeof(error));
    if (range == NULL) printf("open %s failed: %s\n", mode, error);
    return range;
}

/*
 * The shape the rewrite broke: one blocking read of a few bytes at offset zero,
 * which is what media_mp4_open() performs first. Everything about it has to be
 * true at once -- the bytes are the server's, the window reports the length it
 * actually holds, and the source counts one request and no failure.
 */
static void test_blocking_open_read(int port, uint64_t length,
                                    const char *mode, long expected_status)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, mode, port, length, 256u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char header[8] = {0};
    CHECK(reader.read(reader.opaque, 0, header, sizeof(header)));
    for (size_t at = 0; at < sizeof(header); at++)
        CHECK(header[at] == expected_byte(at));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 1
          && stats.retry_attempts == 0
          && stats.failures == 0
          && stats.window_installs == 1
          && stats.last_http_status == expected_status
          /* The regression: a window installed after its response was
             released reported zero bytes received and an empty cache. */
          && stats.bytes_received == 256u * 1024u);
    char detail[192] = {0};
    CHECK(!reader.describe_failure(reader.opaque, detail, sizeof(detail)));
    /* The rest of the window must be served from the cache, not refetched. */
    unsigned char tail[32] = {0};
    CHECK(reader.read(reader.opaque, 4096, tail, sizeof(tail)));
    for (size_t at = 0; at < sizeof(tail); at++)
        CHECK(tail[at] == expected_byte(4096u + at));
    CHECK(media_http_range_stats(range, &stats) && stats.requests == 1
          && stats.cache_hits != 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_metadata_prefetch_is_reused(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "query200", port, length, 256u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    CHECK(media_http_range_prefetch_metadata(range));
    MediaHttpRangeStats before = {0};
    CHECK(media_http_range_stats(range, &before)
          && before.requests == 1u);
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char header[8] = {0};
    CHECK(reader.read(reader.opaque, 0, header, sizeof(header)));
    MediaHttpRangeStats after = {0};
    CHECK(media_http_range_stats(range, &after)
          && after.requests == 1u
          && after.window_installs == 1u
          && after.failures == 0u);
    for (size_t at = 0; at < sizeof(header); at++)
        CHECK(header[at] == expected_byte(at));
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_standard_range_header(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/partial206/media.mp4", port);
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = url,
        .top_level_url = url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    MediaHttpRangeOptions options = {
        .cache_bytes = 64u * 1024u,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .standard_range_header = true,
        .page_request_context = &context
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        &budget, NULL, url, length, &options, error, sizeof(error));
    CHECK(range != NULL);
    if (range != NULL) {
        MediaRangeReader reader = media_http_range_reader(range);
        unsigned char bytes[8] = {0};
        CHECK(reader.read(reader.opaque, 0, bytes, sizeof(bytes)));
        for (size_t at = 0; at < sizeof(bytes); at++)
            CHECK(bytes[at] == expected_byte(at));
        media_http_range_destroy(range);
    }
    CHECK(budget.current == 0);
}

static void test_audio_range_representation(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%d/audio206/media.m4a", port);
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = url,
        .top_level_url = url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    MediaHttpRangeOptions options = {
        .cache_bytes = 64u * 1024u,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .standard_range_header = true,
        .audio_only = true,
        .page_request_context = &context
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        &budget, NULL, url, length, &options, error, sizeof(error));
    CHECK(range != NULL);
    if (range != NULL) {
        MediaRangeReader reader = media_http_range_reader(range);
        unsigned char bytes[8] = {0};
        CHECK(reader.read(reader.opaque, 0, bytes, sizeof(bytes)));
        media_http_range_destroy(range);
    }
    CHECK(budget.current == 0);
}

/*
 * A read that crosses the cache's nominal alignment must keep both halves of
 * the answer straight. Since the complete logical span fits one bounded
 * window, the source shifts that window to the span instead of fetching two
 * aligned windows.
 */
static void test_window_crossing(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "query200", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char spanning[64] = {0};
    uint64_t at = 64u * 1024u - 32u;
    CHECK(reader.read(reader.opaque, at, spanning, sizeof(spanning)));
    for (size_t i = 0; i < sizeof(spanning); i++)
        CHECK(spanning[i] == expected_byte(at + i));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 1
          && stats.window_installs == 1
          && stats.failures == 0
          && stats.bytes_received == 64u * 1024u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The playing pump's contract. A poll may never wait, and once the window has
 * landed the same read must answer with the bytes. This is the only place the
 * would-block path meets a real transport.
 */
static void test_poll_never_waits(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "slow", port, length, 256u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    CHECK(reader.poll != NULL);
    unsigned char header[8] = {0};
    MediaRangeReadStatus status = reader.poll(
        reader.opaque, 0, header, sizeof(header));
    CHECK(status == MEDIA_RANGE_READ_WOULD_BLOCK);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.would_block_reads == 1
          && stats.failures == 0
          && stats.window_pending
          && stats.requests == 1);
    /* Pump the way the media session does: once per frame, with the frame's
       own time passing in between. A tight spin would outrun the server and
       prove nothing about a transfer that takes wall-clock time. */
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(reader.opaque, 0, header, sizeof(header));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(header); at++)
        CHECK(header[at] == expected_byte(at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.bytes_received == 256u * 1024u
          && stats.window_installs == 1
          && stats.failures == 0
          && !stats.window_pending);
    /* The UI estimate counts only unread installed bytes and deliberately
       discounts average bitrate by 20%. With duration == content bytes the
       result is therefore about 80% of this window minus the 8-byte read. */
    uint64_t ahead_us = media_http_range_buffered_ahead_us(range, length);
    CHECK(ahead_us >= 200u * 1024u && ahead_us <= 205u * 1024u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The real-video liveness failure: a lazy fragment's moof crossed an arbitrary
 * transport-window boundary. The first poll copied the prefix from window A,
 * yielded while B arrived, then its retry started at A and evicted B forever.
 * A logical read that fits one bounded cache has to become resident atomically
 * even when its start is awkwardly aligned.
 */
static void test_poll_crossing_window_eventually_completes(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "slow", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char spanning[64] = {0};
    uint64_t at = 64u * 1024u - 32u;
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(
            reader.opaque, at, spanning, sizeof(spanning));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    for (size_t i = 0; i < sizeof(spanning); i++)
        CHECK(spanning[i] == expected_byte(at + i));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.failures == 0
          && stats.window_installs == 1
          && stats.bytes_received == 64u * 1024u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/* A failed aligned successor can be replaced by a shifted window so one
   logical sample remains atomic. Those are different authored requests:
   losing the first connection for each must not make the shifted request
   inherit the aligned request's exhausted retry count. */
static void test_shifted_recovery_has_its_own_retry_scope(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "drop-each-once", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[16] = {0};
    CHECK(reader.read(reader.opaque, 0, probe, sizeof(probe)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, probe, sizeof(probe)));

    unsigned char spanning[64] = {0};
    uint64_t at = 64u * 1024u - 32u;
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(
            reader.opaque, at, spanning, sizeof(spanning));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    for (size_t i = 0; i < sizeof(spanning); i++)
        CHECK(spanning[i] == expected_byte(at + i));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 4
          && stats.retry_attempts == 1
          && stats.window_installs == 2
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * Reading through the back half of a window must put the next one on the wire
 * before it is needed. That is the whole reason a playing unit stops waiting.
 */
static void test_readahead_starts_before_the_window_runs_out(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "query200", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.readahead_requests == 0);
    /* Before the measured quarter-window threshold, nothing is issued. */
    CHECK(reader.read(reader.opaque, 8u * 1024u, sample, sizeof(sample)));
    CHECK(media_http_range_stats(range, &stats)
          && stats.readahead_requests == 0);
    /* Past the quarter mark of the window, still inside it. */
    CHECK(reader.read(reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    CHECK(media_http_range_stats(range, &stats)
          && stats.readahead_requests == 1
          && stats.window_pending
          && stats.window_installs == 1);
    uint64_t installed_ahead =
        media_http_range_buffered_ahead_us(range, length);
    uint64_t resident_ahead = installed_ahead;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        resident_ahead =
            media_http_range_buffered_ahead_us(range, length);
        if (resident_ahead > installed_ahead) break;
        usleep(1000);
    }
    /* Completion leaves the second window in the scheduler until the demux
       crosses the boundary. Buffer hysteresis must nevertheless count those
       already-resident contiguous bytes, or pausing the demux deadlocks the
       very condition which is supposed to resume it. */
    CHECK(resident_ahead >= installed_ahead + 48u * 1024u);
    CHECK(media_http_range_stats(range, &stats)
          && stats.window_pending
          && stats.window_installs == 1);
    /* And the window it prefetched is the one the next read wants, so that
       read costs no new request. */
    CHECK(reader.read(reader.opaque, 64u * 1024u, sample, sizeof(sample)));
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(64u * 1024u + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 2
          && stats.window_installs == 2
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);

    /* Buffering may move the same sequential request earlier without changing
       the normal metadata/seek default or allocating another window. */
    range = open_range(&budget, "query200", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range != NULL) {
        reader = media_http_range_reader(range);
        CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
        CHECK(reader.read(
            reader.opaque, 8u * 1024u, sample, sizeof(sample)));
        CHECK(media_http_range_stats(range, &stats)
              && stats.readahead_requests == 0);
        media_http_range_set_aggressive_readahead(range, true);
        CHECK(media_http_range_stats(range, &stats)
              && stats.readahead_requests == 1
              && stats.window_pending);
        media_http_range_destroy(range);
    }
    CHECK(budget.current == 0);
}

/*
 * A completed aligned readahead and the tail of the active window together
 * already contain this whole logical read. The playback path must stitch the
 * two in one successful poll rather than discard the response and issue a
 * shifted third request. Real fragmented MP4 samples hit this shape whenever
 * their payload crosses an arbitrary HTTP cache boundary.
 */
static void test_completed_readahead_bridges_a_logical_read(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "query200", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[16] = {0};
    CHECK(reader.read(reader.opaque, 0, probe, sizeof(probe)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, probe, sizeof(probe)));

    MediaHttpRangeStats stats = {0};
    uint64_t installed_ahead =
        media_http_range_buffered_ahead_us(range, length);
    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        uint64_t ahead = media_http_range_buffered_ahead_us(range, length);
        if (ahead > installed_ahead) break;
        usleep(1000);
    }
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 2
          && stats.window_installs == 1
          && stats.window_pending);

    unsigned char spanning[64] = {0};
    uint64_t at = 64u * 1024u - 32u;
    MediaRangeReadStatus status = reader.poll(
        reader.opaque, at, spanning, sizeof(spanning));
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    for (size_t i = 0; i < sizeof(spanning); i++)
        CHECK(spanning[i] == expected_byte(at + i));
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 2
          && stats.window_installs == 2
          && stats.split_window_bridges == 1
          && stats.readahead_superseded == 0
          && stats.completed_readahead_superseded == 0
          && stats.superseded_bytes == 0
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The 512 KiB fallback retains one completed successor and does not fetch a
 * second until promotion makes that successor active. Its auxiliary then
 * becomes the immediate predecessor: keep it until demand rotates the next
 * response into place. Lazy MP4 parsing legitimately revisits a moof a few
 * bytes behind the arbitrary cache boundary.
 */
static void test_owned_lookahead_pipelines_three_windows(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "query200", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 8u * 1024u, sample, sizeof(sample)));
    media_http_range_set_aggressive_readahead(range, true);

    MediaHttpRangeStats stats = {0};
    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        CHECK(media_http_range_stats(range, &stats));
        if (stats.lookahead_installs >= 1) break;
        usleep(1000);
    }
    CHECK(stats.lookahead_installs == 1 && stats.requests == 2
          && stats.lookahead_retained_bytes == 64u * 1024u);
    unsigned char spanning[64] = {0};
    uint64_t boundary = 64u * 1024u - 32u;
    CHECK(reader.poll(
              reader.opaque, boundary, spanning, sizeof(spanning))
          == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(spanning); at++)
        CHECK(spanning[at] == expected_byte(boundary + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_promotions == 1
          && stats.requests == 3);

    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        CHECK(media_http_range_stats(range, &stats));
        if (stats.window_pending
            && stats.bytes_in_flight == 64u * 1024u) break;
        usleep(1000);
    }
    CHECK(stats.lookahead_installs == 1 && stats.requests == 3
          && stats.lookahead_retained_bytes == 64u * 1024u);

    /* The future response is complete, but it must not overwrite the old
       active window before this bounded backward metadata read. */
    uint64_t backward = 64u * 1024u - sizeof(sample);
    CHECK(reader.poll(reader.opaque, backward, sample, sizeof(sample))
          == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(backward + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 3
          && stats.readahead_superseded == 0);

    CHECK(reader.poll(reader.opaque, 128u * 1024u, sample, sizeof(sample))
          == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(128u * 1024u + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_installs == 2
          && stats.lookahead_promotions >= 3
          && stats.requests == 4);

    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        CHECK(media_http_range_stats(range, &stats));
        if (stats.window_pending
            && stats.bytes_in_flight == 64u * 1024u) break;
        usleep(1000);
    }
    CHECK(stats.lookahead_installs == 2 && stats.requests == 4);
    CHECK(reader.poll(reader.opaque, 192u * 1024u, sample, sizeof(sample))
          == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(192u * 1024u + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_installs == 3
          && stats.lookahead_promotions >= 4
          && stats.requests == 5
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_second_lookahead_waits_for_audio_reserve(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_two_lookaheads(
        &budget, port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    media_http_range_set_aggressive_readahead(range, true);

    MediaHttpRangeStats stats = {0};
    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        CHECK(media_http_range_stats(range, &stats));
        if (stats.lookahead_installs == 1u) break;
        usleep(1000);
    }
    CHECK(stats.lookahead_slots == 1u
          && stats.lookahead_fetch_limit == 1u
          && stats.lookahead_installs == 1u
          && stats.requests == 2u);
    for (unsigned frame = 0; frame < 50u; frame++) {
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 2u
          && stats.lookahead_installs == 1u);

    /* This is the audio-reserve edge in production. Raising the limit starts
       exactly one additional successor; startup itself waited only for the
       first window above. */
    media_http_range_set_lookahead_limit(range, 2u);
    for (unsigned frame = 0; frame < 4000u; frame++) {
        (void) media_http_range_pump(range);
        CHECK(media_http_range_stats(range, &stats));
        if (stats.lookahead_installs == 2u) break;
        usleep(1000);
    }
    CHECK(stats.lookahead_fetch_limit == 2u
          && stats.lookahead_installs == 2u
          && stats.requests == 3u
          && stats.lookahead_retained_bytes == 128u * 1024u);
    media_http_range_set_lookahead_limit(range, 1u);
    CHECK(reader.poll(
              reader.opaque, 64u * 1024u, sample, sizeof(sample))
          == MEDIA_RANGE_READ_COMPLETE);
    for (unsigned frame = 0; frame < 50u; frame++) {
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_fetch_limit == 1u
          && stats.requests == 3u);
    media_http_range_set_lookahead_limit(range, 2u);
    CHECK(media_http_range_resident(
        range, 128u * 1024u, sizeof(sample)));
    CHECK(reader.poll(
              reader.opaque, 128u * 1024u, sample, sizeof(sample))
          == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(128u * 1024u + at));
    CHECK(media_http_range_stats(range, &stats)
          && stats.lookahead_fetch_limit == 2u
          && stats.requests == 4u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/* The slow tail of a bounded window must not hide a prefix which passed the
   response-header gate and is already owned by the range source. */
static void test_streamed_successor_is_visible_before_eof(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "slow", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    MediaHttpRangeStats stats = {0};
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(
            reader.opaque, 64u * 1024u, sample, sizeof(sample));
        CHECK(media_http_range_stats(range, &stats));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    for (size_t at = 0; at < sizeof(sample); at++)
        CHECK(sample[at] == expected_byte(64u * 1024u + at));
    CHECK(stats.window_pending && stats.window_installs == 1
          && stats.lookahead_installs == 0
          && stats.streaming_partial_reads == 1
          && stats.streaming_partial_bytes == sizeof(sample));
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_stream_without_length_falls_back_to_terminal_admission(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "stream-no-length", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    MediaHttpRangeStats stats = {0};
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(
            reader.opaque, 64u * 1024u, sample, sizeof(sample));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    CHECK(media_http_range_stats(range, &stats));
    CHECK(stats.streaming_partial_reads == 0
          && stats.window_installs == 2
          && stats.lookahead_installs == 1
          && stats.lookahead_promotions == 1
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_rejected_stream_headers_fail_without_wedging(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "stream-short", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader.poll(
            reader.opaque, 64u * 1024u, sample, sizeof(sample));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_FAILED);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.failures == 1
          && stats.retry_attempts == 1
          && stats.streaming_partial_reads == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static MediaRangeReadStatus poll_successor_until_terminal(
    MediaHttpRange *range, MediaRangeReader *reader, uint64_t offset,
    unsigned char *sample, size_t length)
{
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = reader->poll(reader->opaque, offset, sample, length);
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    return status;
}

/* A signed media URL commonly expires between windows. The range source must
   preserve HTTP 403 through its streaming header rejection so the session can
   re-resolve once, and a replacement URL must be able to resume at the exact
   failed offset. */
static void test_expired_url_is_classified_and_replacement_resumes(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *expired = open_range_with_lookahead(
        &budget, "expire403", port, length, 64u * 1024u);
    CHECK(expired != NULL);
    if (expired == NULL) return;
    MediaRangeReader reader = media_http_range_reader(expired);
    unsigned char sample[32] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    MediaRangeReadStatus status = poll_successor_until_terminal(
        expired, &reader, 64u * 1024u, sample, sizeof(sample));
    MediaHttpRangeStats stats = {0};
    CHECK(status == MEDIA_RANGE_READ_FAILED
          && media_http_range_stats(expired, &stats)
          && stats.last_http_status == 403
          && stats.retry_attempts == 1
          && stats.failures == 1);
    media_http_range_destroy(expired);

    MediaHttpRange *replacement = open_range_with_lookahead(
        &budget, "query200", port, length, 64u * 1024u);
    CHECK(replacement != NULL);
    if (replacement != NULL) {
        MediaRangeReader fresh = media_http_range_reader(replacement);
        CHECK(fresh.read(
                  fresh.opaque, 64u * 1024u, sample, sizeof(sample)));
        for (size_t at = 0; at < sizeof(sample); at++)
            CHECK(sample[at] == expected_byte(64u * 1024u + at));
        media_http_range_destroy(replacement);
    }
    CHECK(budget.current == 0);
}

static MediaHttpRangePrimeStatus pump_successor_prime(
    MediaHttpRange *range)
{
    MediaHttpRangePrimeStatus status = MEDIA_HTTP_RANGE_PRIME_PENDING;
    for (unsigned frame = 0; frame < 4000u; frame++) {
        status = media_http_range_prime_successor(range);
        if (status != MEDIA_HTTP_RANGE_PRIME_PENDING) break;
        usleep(1000);
    }
    return status;
}

/* Playback must not commit a signed candidate merely because its prefix is
   readable. The successful form retains the proven successor as ordinary
   lookahead. Every refusal form remains a PRIME_FAILED result after one
   bounded retry, including the non-403 shapes which require the session's
   explicit candidate-rejection trigger rather than its expiry policy. */
static void test_successor_prime_rejects_prefix_only_delivery(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    const char *modes[] = {
        "query200", "expire403", "prefix-short", "prefix416"
    };
    const MediaHttpRangePrimeStatus expected[] = {
        MEDIA_HTTP_RANGE_PRIME_READY,
        MEDIA_HTTP_RANGE_PRIME_FAILED,
        MEDIA_HTTP_RANGE_PRIME_FAILED,
        MEDIA_HTTP_RANGE_PRIME_FAILED
    };
    const long expected_http[] = {200, 403, 200, 416};
    for (size_t mode = 0; mode < 4u; mode++) {
        MediaHttpRange *range = open_range_with_lookahead(
            &budget, modes[mode], port, length, 64u * 1024u);
        CHECK(range != NULL);
        if (range == NULL) continue;
        MediaRangeReader reader = media_http_range_reader(range);
        unsigned char header[8] = {0};
        CHECK(reader.read(reader.opaque, 0, header, sizeof(header)));
        MediaHttpRangePrimeStatus status = pump_successor_prime(range);
        CHECK(status == expected[mode]);
        MediaHttpRangeStats stats = {0};
        CHECK(media_http_range_stats(range, &stats));
        if (status == MEDIA_HTTP_RANGE_PRIME_READY) {
            CHECK(media_http_range_resident(
                      range, 64u * 1024u, 1u)
                  && stats.requests == 2u
                  && stats.retry_attempts == 0u
                  && stats.failures == 0u);
        } else {
            char detail[256] = {0};
            CHECK(stats.requests == 3u
                  && stats.retry_attempts == 1u
                  && stats.failures == 1u
                  && stats.last_http_status == expected_http[mode]
                  && reader.describe_failure(
                         reader.opaque, detail, sizeof(detail))
                  && detail[0] != '\0');
        }
        media_http_range_destroy(range);
    }
    CHECK(budget.current == 0);
}

static void test_repeated_connection_reset_is_bounded(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range_with_lookahead(
        &budget, "reset-always", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char sample[16] = {0};
    CHECK(reader.read(reader.opaque, 0, sample, sizeof(sample)));
    CHECK(reader.read(
        reader.opaque, 20u * 1024u, sample, sizeof(sample)));
    MediaRangeReadStatus status = poll_successor_until_terminal(
        range, &reader, 64u * 1024u, sample, sizeof(sample));
    MediaHttpRangeStats stats = {0};
    CHECK(status == MEDIA_RANGE_READ_FAILED
          && media_http_range_stats(range, &stats)
          && stats.retry_attempts == 1 && stats.failures == 1
          && stats.streaming_partial_reads == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_corrupt_range_responses_are_never_admitted(
    int port, uint64_t length)
{
    const char *modes[] = {"bad206", "ignore-range"};
    const long statuses[] = {206, 200};
    for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
        Budget budget;
        budget_init(&budget, 8u * 1024u * 1024u);
        MediaHttpRange *range = open_range(
            &budget, modes[mode], port, length, 64u * 1024u);
        CHECK(range != NULL);
        if (range == NULL) continue;
        MediaRangeReader reader = media_http_range_reader(range);
        unsigned char sample[16] = {0};
        CHECK(!reader.read(reader.opaque, 0, sample, sizeof(sample)));
        MediaHttpRangeStats stats = {0};
        CHECK(media_http_range_stats(range, &stats)
              && stats.last_http_status == statuses[mode]
              && stats.window_installs == 0 && stats.failures == 1);
        media_http_range_destroy(range);
        CHECK(budget.current == 0);
    }
}

/* A short body is retried once and then reported, never admitted. */
static void test_short_body_is_reported(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "short", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char header[8] = {0};
    CHECK(!reader.read(reader.opaque, 0, header, sizeof(header)));
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.requests == 2
          && stats.retry_attempts == 1
          && stats.failures == 1
          && stats.window_installs == 0
          && stats.bytes_received == 0);
    char detail[192] = {0};
    CHECK(reader.describe_failure(reader.opaque, detail, sizeof(detail))
          && detail[0] != '\0');
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * A fragmented MP4 read sequentially across its fragment boundaries, over the
 * real scheduler.
 *
 * The demuxer's lazy sidx window advance is reached today only by
 * tests/test_media_mp4.c, through a substituted synchronous transport that
 * answers every read from memory. That seam hides the whole class this reader
 * is most exposed to: a fragment boundary that also needs a transport window
 * the source has not fetched yet. The moof of the next segment is read with
 * the non-blocking form, so it can answer WOULD_BLOCK, and the caller has to
 * come back for exactly that window on a later pump. Here it really does.
 *
 * This would not have caught the seek deadlock in 639afb6 -- that was an
 * eligibility horizon in the decode pump, above this layer, and no read in it
 * ever blocked. It pins the boundary-crossing class instead, which that
 * investigation's first hypothesis was and which nothing hermetic covered.
 */
static void test_fragment_boundaries_cross_over_real_windows(
    int port, uint64_t length, unsigned fragments, unsigned per_fragment,
    const char *mode, bool expect_blocked_read)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    /* The smallest window the source admits that still needs several fetches
       for this fixture, so boundary crossings and window crossings interleave
       instead of all landing in one cached window. */
    MediaHttpRange *range = open_range(
        &budget, mode, port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    char error[256] = {0};
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    CHECK(demux != NULL);
    if (demux == NULL) {
        printf("fragmented open failed: %s\n", error);
        media_http_range_destroy(range);
        return;
    }
    MediaMp4TrackInfo info = {0};
    CHECK(media_mp4_track_count(demux) == 1
          && media_mp4_track_info(demux, 0, &info)
          && info.kind == MEDIA_MP4_TRACK_VIDEO);

    unsigned char *payload = malloc(256u * 1024u);
    CHECK(payload != NULL);
    unsigned samples = 0;
    unsigned boundaries = 0;
    unsigned blocked_windows = 0;
    unsigned blocked_samples = 0;
    uint64_t contiguous = 0;
    bool contiguous_known = false;
    bool failed = false;
    MediaMp4Sample sample = {0};
    bool have_sample = false;
    /* Bounded by construction: every iteration either consumes a sample or
       pumps a window, and the fixture holds exactly fragments*per_fragment
       samples. A wedge exhausts this and fails rather than hanging the gate. */
    for (unsigned step = 0; payload != NULL && step < 20000u; step++) {
        if (!have_sample && !media_mp4_next_sample(demux, &sample)) {
            if (media_mp4_would_block(demux)) {
                /* The next fragment's moof is not in the window yet. This is
                   the crossing the substituted transport can never produce. */
                blocked_windows++;
                (void) media_http_range_pump(range);
                usleep(500);
                continue;
            }
            char detail[192] = {0};
            if (media_mp4_last_error(demux, detail, sizeof(detail))) {
                printf("fragmented sample stream failed: %s\n", detail);
                failed = true;
            }
            break;
        }
        /* next_sample has already advanced the track, so a would-block read
           has to be retried with this same sample rather than asked for the
           next one -- which is the contract media_mp4_read_sample documents
           and the mistake a caller makes once. */
        have_sample = true;
        if (!media_mp4_read_sample(demux, &sample, payload, 256u * 1024u)) {
            if (media_mp4_would_block(demux)) {
                blocked_samples++;
                (void) media_http_range_pump(range);
                usleep(500);
                continue;
            }
            char detail[192] = {0};
            (void) media_mp4_last_error(demux, detail, sizeof(detail));
            printf("fragmented sample read failed: %s\n", detail);
            failed = true;
            break;
        }
        /* Samples inside one fragment are adjacent in the mdat, so a gap is
           the moof of the next one: the demuxer advanced its sidx window
           between these two reads. */
        if (contiguous_known && sample.offset != contiguous) boundaries++;
        contiguous = sample.offset + sample.size;
        contiguous_known = true;
        samples++;
        have_sample = false;
    }
    CHECK(!failed);
    /* Every sample of every fragment, so the last boundary was crossed and
       the stream ended cleanly rather than wedging in the middle. */
    CHECK(samples == fragments * per_fragment);
    CHECK(boundaries == fragments - 1u);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.failures == 0
          /* The fixture is several windows long, so a run that installed one
             window would mean the reader never left it. */
          && stats.window_installs >= 4
          && stats.bytes_received >= 4u * 64u * 1024u);
    /* Dribbled out, readahead cannot keep every fragment byte ahead of the
       reader, so either its moof or a sample payload must answer WOULD_BLOCK
       and be retried. The sequential-window bridge can legitimately make the
       moof immediately available and move that wait to the first sample; the
       contract is the nonblocking source wait, not which demux call observes
       it. Served at full speed neither location is required to block. */
    if (expect_blocked_read)
        CHECK(blocked_windows != 0 || blocked_samples != 0);
    printf("fragmented(%s): samples=%u boundaries=%u installs=%zu "
           "blocked-windows=%u blocked-samples=%u\n",
           mode, samples, boundaries, stats.window_installs,
           blocked_windows, blocked_samples);
    free(payload);
    media_mp4_close(demux);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The shape a seek deadlocked in on a device, reduced to the mechanism.
 *
 * A read that misses its window pumps the scheduler once on its way to
 * WOULD_BLOCK, so any loop that reads every iteration keeps its own transfer
 * moving and this is invisible. A seek does not read every iteration: the
 * decode pump's eligibility horizon turns most iterations away before the
 * demuxer is asked for anything -- 5,261 horizon exits against 6 source-blocked
 * reads on the failing cycle. Those iterations touch the transport not at all,
 * and libcurl is only given a turn by an explicit pump.
 *
 * So the invariant the fix rests on, stated on its own: an outstanding window
 * advances by exactly zero bytes across a wait that does not pump, and lands
 * across the same wait that does. Nothing before this pinned it; the fixed
 * caller is PSP-only, but what it must do is provable here.
 */
static void test_a_pending_window_needs_someone_to_pump(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    /* Dribbled, so that one window is unambiguously more than any single
       bounded step can carry -- the device's condition, not a race. */
    MediaHttpRange *range = open_range(
        &budget, "fragment-slow", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[8] = {0};
    /* Establish a window first, so what follows is a seek out of it rather
       than an opening read. */
    CHECK(reader.read(reader.opaque, 0, probe, sizeof(probe)));
    uint64_t target = length - 4096u;
    MediaRangeReadStatus status =
        reader.poll(reader.opaque, target, probe, sizeof(probe));
    CHECK(status == MEDIA_RANGE_READ_WOULD_BLOCK);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats) && stats.window_pending);
    size_t unpumped = stats.bytes_in_flight;

    /* Two hundred iterations of the failing loop: no read, because the
       horizon refused them, and no pump. */
    for (unsigned step = 0; step < 200u; step++) usleep(1000);
    CHECK(media_http_range_stats(range, &stats)
          && stats.window_pending
          && stats.bytes_in_flight == unpumped);

    /* The same wait with the bounded step the playing path already takes. */
    bool landed = false;
    for (unsigned step = 0; step < 8000u && !landed; step++) {
        (void) media_http_range_pump(range);
        usleep(1000);
        landed = media_http_range_stats(range, &stats)
            && stats.bytes_in_flight != unpumped;
    }
    CHECK(landed);
    /* And the read that loop was waiting on completes. */
    for (unsigned step = 0; step < 8000u; step++) {
        status = reader.poll(reader.opaque, target, probe, sizeof(probe));
        if (status != MEDIA_RANGE_READ_WOULD_BLOCK) break;
        (void) media_http_range_pump(range);
        usleep(1000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    CHECK(media_http_range_stats(range, &stats)
          && stats.failures == 0 && stats.window_installs >= 2);
    printf("pending-window: unpumped-inflight=%zu installs=%zu\n",
           unpumped, stats.window_installs);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_slow_progress_is_retained(int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "slow-progress", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[8] = {0};
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    while (status == MEDIA_RANGE_READ_WOULD_BLOCK
           && tilefinch_platform_monotonic_time_us() - started_us
                  < UINT64_C(12000000)) {
        status = reader.poll(reader.opaque, 0, probe, sizeof(probe));
        (void) media_http_range_pump(range);
        usleep(10000);
    }
    MediaHttpRangeStats stats = {0};
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    CHECK(media_http_range_stats(range, &stats)
          && stats.reconnects == 0
          && stats.stalled_reconnect_exhaustions == 0
          && stats.failures == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

static void test_prolonged_window_can_be_superseded(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "stall-window", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[8] = {0};
    CHECK(reader.read(reader.opaque, 0, probe, sizeof(probe)));

    const uint64_t failed_offset = 256u * 1024u;
    MediaRangeReadStatus status = MEDIA_RANGE_READ_WOULD_BLOCK;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    while (status == MEDIA_RANGE_READ_WOULD_BLOCK
           && tilefinch_platform_monotonic_time_us() - started_us
                  < UINT64_C(16000000)) {
        status = reader.poll(
            reader.opaque, failed_offset, probe, sizeof(probe));
        (void) media_http_range_pump(range);
        usleep(10000);
    }
    MediaHttpRangeStats stats = {0};
    CHECK(status == MEDIA_RANGE_READ_WOULD_BLOCK);
    CHECK(media_http_range_stats(range, &stats)
          && stats.window_pending && !stats.delivery_stalled
          && stats.reconnects == MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS
          && stats.stalled_reconnect_exhaustions == 0u
          && stats.failures == 0u);

    /* A seek or later sample selecting a different logical window starts a
       fresh incident immediately. It must neither wait for the old incident's
       absolute deadline nor inherit its reconnect cap. */
    const uint64_t healthy_offset = 512u * 1024u;
    status = MEDIA_RANGE_READ_WOULD_BLOCK;
    started_us = tilefinch_platform_monotonic_time_us();
    while (status == MEDIA_RANGE_READ_WOULD_BLOCK
           && tilefinch_platform_monotonic_time_us() - started_us
                  < UINT64_C(5000000)) {
        status = reader.poll(
            reader.opaque, healthy_offset, probe, sizeof(probe));
        (void) media_http_range_pump(range);
        usleep(10000);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    CHECK(media_http_range_stats(range, &stats)
          && !stats.delivery_stalled && !stats.window_pending);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The open hang, reduced to its mechanism.
 *
 * A blocking read arms a deadline per window, which bounds one read and
 * nothing at all across the many an MP4 open performs: the top-level scan, the
 * moov, and every lazy index window each got a fresh fifteen seconds. Against
 * a source that accepts the connection and then never sends the body -- the
 * device's shape -- the open therefore ran for as long as there were reads
 * left to make, printed nothing, and could not be attributed to a phase.
 *
 * The transaction that owns the reads is the layer that knows what the
 * sequence may spend. With that budget set, the first read ends inside it
 * rather than inside the window timeout, and the second does not get a fresh
 * one.
 */
static void test_a_transaction_budget_bounds_every_read_in_it(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    MediaHttpRange *range = open_range(
        &budget, "stall", port, length, 64u * 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[8] = {0};
    media_http_range_set_wait_budget_us(range, UINT64_C(700000));
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    CHECK(!reader.read(reader.opaque, 0, probe, sizeof(probe)));
    uint64_t first_us = tilefinch_platform_monotonic_time_us() - started_us;
    /* The per-window timeout is fifteen seconds; the budget is 0.7. Three
       seconds is comfortably between them and immune to loopback jitter. */
    CHECK(first_us < UINT64_C(3000000));
    char detail[192] = {0};
    CHECK(reader.describe_failure(reader.opaque, detail, sizeof(detail))
          && strstr(detail, "transaction budget") != NULL);

    /* The defect stated directly: a second read must not re-arm anything. */
    uint64_t again_us = tilefinch_platform_monotonic_time_us();
    CHECK(!reader.read(reader.opaque, 0, probe, sizeof(probe)));
    uint64_t second_us = tilefinch_platform_monotonic_time_us() - again_us;
    CHECK(second_us < UINT64_C(1500000));

    /* And handing the source back leaves the ordinary per-window bound, so
       nothing outside an open transaction inherits a spent budget. */
    media_http_range_clear_wait_budget(range);
    MediaRangeReadStatus status =
        reader.poll(reader.opaque, 0, probe, sizeof(probe));
    CHECK(status == MEDIA_RANGE_READ_WOULD_BLOCK);
    printf("transaction-budget: first=%lluus second=%lluus\n",
           (unsigned long long) first_us, (unsigned long long) second_us);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/* The owner's absolute law: a stop request is answered within three seconds,
   in every phase, whatever the phase is waiting on. */
#define MEDIA_RANGE_CANCEL_LAW_US UINT64_C(3000000)

static uint64_t cancel_after_us;
static unsigned cancel_polls;

static bool cancel_when_due(void *opaque)
{
    (void) opaque;
    cancel_polls++;
    return tilefinch_platform_monotonic_time_us() >= cancel_after_us;
}

/*
 * The property the device run violated: CIRCLE was pressed during the open and
 * the session neither cancelled nor exited. Whatever else a blocking read is
 * waiting on, it observes the request once per bounded wait step, so the
 * answer arrives in milliseconds rather than at the window timeout -- and the
 * source it was waiting on here never sends a byte, so nothing but the
 * cancellation can end it inside the law.
 */
static void test_a_stalled_read_answers_cancellation_inside_the_law(
    int port, uint64_t length)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    cancel_polls = 0;
    cancel_after_us =
        tilefinch_platform_monotonic_time_us() + UINT64_C(400000);
    MediaHttpRange *range = open_range_cancellable(
        &budget, "stall", port, length, 64u * 1024u, cancel_when_due, NULL);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char probe[8] = {0};
    CHECK(!reader.read(reader.opaque, 0, probe, sizeof(probe)));
    uint64_t answered_us = tilefinch_platform_monotonic_time_us();
    CHECK(answered_us >= cancel_after_us
          && answered_us - cancel_after_us < MEDIA_RANGE_CANCEL_LAW_US);
    CHECK(cancel_polls > 1u);
    char detail[192] = {0};
    CHECK(reader.describe_failure(reader.opaque, detail, sizeof(detail))
          && strstr(detail, "cancelled") != NULL);
    printf("cancel-law: answered=%lluus-after polls=%u\n",
           (unsigned long long) (answered_us - cancel_after_us),
           cancel_polls);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

typedef struct {
    size_t cache_bytes;
    unsigned presented;
    unsigned hold_events;
    uint64_t held_us;
    uint64_t longest_hold_us;
    uint64_t startup_us;
    uint64_t first_hold_offset;
    uint64_t first_hold_cache_offset;
    uint64_t first_hold_fill_offset;
    size_t first_hold_cache_length;
    size_t first_hold_fill_length;
    uint64_t first_hold_ahead_us;
    uint64_t media_span_us;
    MediaHttpRangeStats transport;
} CadenceResult;

/*
 * A deterministic version of the device experiment. The server supplies the
 * captured 23.976 fps validation stream through a fixed byte-positioned
 * burst/gap schedule. This loop reads the real MP4 samples at their authored
 * DTS cadence and pumps the exact range source used by the PSP. A contiguous
 * sequence of WOULD_BLOCK answers is one user-visible hold; retry polls do not
 * inflate its count.
 *
 * This is deliberately a manual mode rather than part of ctest: it spends
 * real wall time to preserve the transport/consumer phase relationship. Run
 * it when changing media cache or readahead policy, then take only the winner
 * to hardware.
 */
static bool run_cadence_case(
    int port, uint64_t length, const char *profile, size_t cache_bytes,
    CadenceResult *result)
{
    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    result->cache_bytes = cache_bytes;
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    uint64_t opened_us = tilefinch_platform_monotonic_time_us();
    size_t publication_bytes = profile != NULL
            && strcmp(profile, "psp-48k") == 0
        ? 48u * 1024u : 16u * 1024u;
    MediaHttpRange *range = open_cadence_source(
        &budget, "cadence", port, length, true, publication_bytes);
    if (range == NULL) return false;
    /* This fixture is video-only, so there is no audio range to protect. */
    media_http_range_set_lookahead_limit(range, 2u);
    MediaRangeReader reader = media_http_range_reader(range);
    char error[256] = {0};
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    if (demux == NULL) {
        printf("cadence(%zuK): MP4 open failed: %s\n",
               cache_bytes / 1024u, error);
        media_http_range_destroy(range);
        return false;
    }
    result->startup_us =
        tilefinch_platform_monotonic_time_us() - opened_us;
    MediaMp4TrackInfo info = {0};
    bool have_video = false;
    for (size_t at = 0; at < media_mp4_track_count(demux); at++) {
        if (media_mp4_track_info(demux, at, &info)
            && info.kind == MEDIA_MP4_TRACK_VIDEO) {
            have_video = true;
            break;
        }
    }
    if (!have_video || info.largest_sample == 0) {
        printf("cadence(%zuK): no bounded video track\n",
               cache_bytes / 1024u);
        media_mp4_close(demux);
        media_http_range_destroy(range);
        return false;
    }
    /* A lazy fragmented track learns later fragments' maxima as it advances;
       the first-window TrackInfo is not a whole-file allocation authority. */
    size_t payload_capacity = media_mp4_default_limits().maximum_sample_bytes;
    unsigned char *payload = malloc(payload_capacity);
    if (payload == NULL) {
        media_mp4_close(demux);
        media_http_range_destroy(range);
        return false;
    }
    media_http_range_set_aggressive_readahead(range, true);
    const uint64_t run_media_us = UINT64_C(30000000);
    const uint64_t wall_limit_us = UINT64_C(50000000);
    uint64_t wall_started_us = tilefinch_platform_monotonic_time_us();
    uint64_t first_dts_us = 0;
    uint64_t hold_started_us = 0;
    MediaMp4Sample sample = {0};
    bool have_sample = false;
    bool complete = false;
    while (tilefinch_platform_monotonic_time_us() - wall_started_us
               < wall_limit_us) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        (void) media_http_range_pump(range);
        if (!have_sample) {
            if (!media_mp4_next_sample(demux, &sample)) {
                if (media_mp4_would_block(demux)) {
                    usleep(1000);
                    continue;
                }
                char detail[192] = {0};
                if (media_mp4_last_error(demux, detail, sizeof(detail)))
                    printf("cadence(%zuK): sample failed: %s\n",
                           cache_bytes / 1024u, detail);
                break;
            }
            if (sample.kind != MEDIA_MP4_TRACK_VIDEO) continue;
            have_sample = true;
            uint64_t dts_us = sample.timescale == 0 ? 0
                : sample.dts * UINT64_C(1000000) / sample.timescale;
            if (result->presented == 0) first_dts_us = dts_us;
            result->media_span_us = dts_us - first_dts_us;
            if (dts_us - first_dts_us >= run_media_us) {
                complete = true;
                break;
            }
            uint64_t due_us = wall_started_us + dts_us - first_dts_us;
            if (now_us < due_us) {
                uint64_t wait_us = due_us - now_us;
                usleep((useconds_t) (wait_us > 2000u ? 2000u : wait_us));
                continue;
            }
        }
        if (!media_mp4_read_sample(
                demux, &sample, payload, payload_capacity)) {
            if (media_mp4_would_block(demux)) {
                if (hold_started_us == 0) {
                    hold_started_us = now_us;
                    result->hold_events++;
                    if (result->hold_events == 1u) {
                        MediaHttpRangeStats at_hold = {0};
                        (void) media_http_range_stats(range, &at_hold);
                        result->first_hold_offset = sample.offset;
                        result->first_hold_cache_offset = at_hold.cache_offset;
                        result->first_hold_cache_length = at_hold.cache_length;
                        result->first_hold_fill_offset = at_hold.fill_offset;
                        result->first_hold_fill_length = at_hold.fill_length;
                        uint64_t duration_us = info.timescale == 0 ? 0
                            : info.duration * UINT64_C(1000000)
                                / info.timescale;
                        result->first_hold_ahead_us =
                            media_http_range_buffered_ahead_us(
                                range, duration_us);
                    }
                }
                usleep(1000);
                continue;
            }
            char detail[192] = {0};
            (void) media_mp4_last_error(demux, detail, sizeof(detail));
            printf("cadence(%zuK): payload failed: %s\n",
                   cache_bytes / 1024u, detail);
            break;
        }
        if (hold_started_us != 0) {
            uint64_t held = now_us - hold_started_us;
            result->held_us += held;
            if (held > result->longest_hold_us)
                result->longest_hold_us = held;
            hold_started_us = 0;
        }
        result->presented++;
        have_sample = false;
    }
    if (hold_started_us != 0) {
        uint64_t held = tilefinch_platform_monotonic_time_us()
            - hold_started_us;
        result->held_us += held;
        if (held > result->longest_hold_us) result->longest_hold_us = held;
    }
    CHECK(media_http_range_stats(range, &result->transport));
    printf(
        "cadence(%s,%zuK): startup=%llums presented=%u holds=%u held=%llums "
        "longest=%llums requests=%zu superseded=%zu complete-waste=%zu "
        "waste=%zu retries=%zu reconnects=%zu/%zu-starved failures=%zu "
        "prefetch=%zu/%zu/%zu consumed=%zu/%zu slots=%u aggressive=%u "
        "lookahead=%zu/%zu retained=%zuB partial=%zu/%zuB "
        "media-span=%llums "
        "first-hold=sample:%llu cache:%llu+%zu fill:%llu+%zu ahead:%llums\n",
        profile == NULL ? "unknown" : profile, cache_bytes / 1024u,
        (unsigned long long) (result->startup_us / 1000u),
        result->presented, result->hold_events,
        (unsigned long long) (result->held_us / 1000u),
        (unsigned long long) (result->longest_hold_us / 1000u),
        result->transport.requests,
        result->transport.readahead_superseded,
        result->transport.completed_readahead_superseded,
        result->transport.superseded_bytes,
        result->transport.retry_attempts,
        result->transport.reconnects,
        result->transport.starved_reconnects,
        result->transport.failures,
        result->transport.readahead_requests,
        result->transport.readahead_waiting_for_consumption,
        result->transport.readahead_issue_refusals,
        result->transport.cache_consumed,
        result->transport.cache_length,
        result->transport.lookahead_slots,
        result->transport.aggressive_readahead ? 1u : 0u,
        result->transport.lookahead_installs,
        result->transport.lookahead_promotions,
        result->transport.lookahead_retained_bytes,
        result->transport.streaming_partial_reads,
        result->transport.streaming_partial_bytes,
        (unsigned long long) (result->media_span_us / 1000u),
        (unsigned long long) result->first_hold_offset,
        (unsigned long long) result->first_hold_cache_offset,
        result->first_hold_cache_length,
        (unsigned long long) result->first_hold_fill_offset,
        result->first_hold_fill_length,
        (unsigned long long) (result->first_hold_ahead_us / 1000u));
    free(payload);
    media_mp4_close(demux);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
    return complete;
}

/* A deterministic two-source approximation of the session's delivery law.
   Video uses the captured MP4's authored samples; audio consumes 384-byte AAC-
   shaped blocks every 23.22 ms. The server serializes their response bodies to
   model the PSP transport worker's one authored HTTP hop, and presentation is
   held at the session's 250 ms cross-track lead. This deliberately tests
   transport, buffering and skew control rather than either firmware decoder. */
static bool run_coupled_cadence(
    int port, uint64_t length, const char *profile)
{
    const uint64_t run_media_us = UINT64_C(30000000);
    const uint64_t wall_limit_us = profile != NULL
            && strcmp(profile, "chaos-recovery") == 0
        ? UINT64_C(80000000) : UINT64_C(50000000);
    const uint64_t audio_period_us = UINT64_C(23220);
    const uint64_t maximum_track_lead_us = UINT64_C(75000);
    const uint64_t skew_recovery_limit_us = UINT64_C(500000);
    const size_t audio_block_bytes = 384u;
    Budget budget;
    budget_init(&budget, 24u * 1024u * 1024u);
    MediaHttpRange *video = open_cadence_source(
        &budget, "cadence-video", port, length, true, 16u * 1024u);
    MediaHttpRange *audio = open_cadence_source(
        &budget, "cadence-audio", port, length, false, 16u * 1024u);
    if (video == NULL || audio == NULL) {
        media_http_range_destroy(video);
        media_http_range_destroy(audio);
        return false;
    }
    MediaRangeReader video_reader = media_http_range_reader(video);
    MediaRangeReader audio_reader = media_http_range_reader(audio);
    char error[256] = {0};
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &video_reader, NULL, error, sizeof(error));
    if (demux == NULL) {
        printf("coupled: MP4 open failed: %s\n", error);
        media_http_range_destroy(video);
        media_http_range_destroy(audio);
        return false;
    }
    MediaMp4TrackInfo info = {0};
    bool have_video_track = false;
    for (size_t at = 0; at < media_mp4_track_count(demux); at++) {
        if (media_mp4_track_info(demux, at, &info)
            && info.kind == MEDIA_MP4_TRACK_VIDEO) {
            have_video_track = true;
            break;
        }
    }
    size_t payload_capacity = media_mp4_default_limits().maximum_sample_bytes;
    unsigned char *payload = malloc(payload_capacity);
    unsigned char audio_block[audio_block_bytes];
    if (!have_video_track || payload == NULL) {
        free(payload);
        media_mp4_close(demux);
        media_http_range_destroy(video);
        media_http_range_destroy(audio);
        return false;
    }
    media_http_range_set_aggressive_readahead(video, true);
    media_http_range_set_aggressive_readahead(audio, true);
    uint64_t wall_started_us = tilefinch_platform_monotonic_time_us();
    uint64_t timeline_paused_us = 0;
    uint64_t first_video_dts_us = 0;
    uint64_t video_media_us = 0;
    uint64_t audio_media_us = 0;
    uint64_t maximum_skew_us = 0;
    uint64_t skew_excursion_started_us = 0;
    uint64_t maximum_skew_recovery_us = 0;
    uint64_t video_hold_started_us = 0;
    uint64_t audio_hold_started_us = 0;
    uint64_t video_held_us = 0;
    uint64_t audio_held_us = 0;
    unsigned video_holds = 0;
    unsigned audio_holds = 0;
    unsigned video_presented = 0;
    unsigned audio_blocks = 0;
    unsigned sync_holds = 0;
    bool buffering = false;
    uint64_t buffer_started_us = 0;
    uint64_t buffer_starved_since_us = 0;
    uint64_t buffer_ready_since_us = 0;
    uint64_t shortest_buffer_us = UINT64_MAX;
    uint64_t shortest_clear_us = UINT64_MAX;
    uint64_t last_buffer_ended_us = 0;
    unsigned buffer_begins = 0;
    unsigned buffer_ends = 0;
    bool have_video_sample = false;
    bool have_video_payload = false;
    MediaMp4Sample sample = {0};
    bool complete = false;
    while (tilefinch_platform_monotonic_time_us() - wall_started_us
               < wall_limit_us) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        bool source_blocked = false;
        (void) media_http_range_pump(video);
        (void) media_http_range_pump(audio);
        if (!have_video_sample) {
            if (media_mp4_next_sample(demux, &sample)) {
                if (sample.kind == MEDIA_MP4_TRACK_VIDEO) {
                    have_video_sample = true;
                    uint64_t dts_us = sample.timescale == 0 ? 0
                        : sample.dts * UINT64_C(1000000) / sample.timescale;
                    if (video_presented == 0) first_video_dts_us = dts_us;
                }
            } else if (media_mp4_would_block(demux)) {
                source_blocked = true;
            } else {
                break;
            }
        }
        uint64_t next_audio_us = (uint64_t) audio_blocks * audio_period_us;
        if (!buffering && next_audio_us < run_media_us
            && now_us >= wall_started_us + timeline_paused_us + next_audio_us
            && (video_presented == 0
                || next_audio_us <= video_media_us + maximum_track_lead_us)) {
            uint64_t offset = (uint64_t) audio_blocks * audio_block_bytes;
            MediaRangeReadStatus status = audio_reader.poll(
                audio_reader.opaque, offset, audio_block, sizeof(audio_block));
            if (status == MEDIA_RANGE_READ_COMPLETE) {
                if (audio_hold_started_us != 0) {
                    audio_held_us += now_us - audio_hold_started_us;
                    audio_hold_started_us = 0;
                }
                audio_blocks++;
                audio_media_us = (uint64_t) audio_blocks * audio_period_us;
            } else if (status == MEDIA_RANGE_READ_WOULD_BLOCK) {
                source_blocked = true;
                if (audio_hold_started_us == 0) {
                    audio_hold_started_us = now_us;
                    audio_holds++;
                }
            } else {
                break;
            }
        }
        if (have_video_sample) {
            uint64_t dts_us = sample.timescale == 0 ? 0
                : sample.dts * UINT64_C(1000000) / sample.timescale;
            uint64_t next_video_us = dts_us - first_video_dts_us;
            if (next_video_us >= run_media_us
                && audio_media_us >= run_media_us) {
                complete = true;
                break;
            }
            bool due = now_us >= wall_started_us + timeline_paused_us
                + next_video_us;
            bool sync_ready = audio_blocks == 0
                || next_video_us <= audio_media_us + maximum_track_lead_us;
            /* Buffering holds presentation, not source/decode work. Preserve
               one fetched sample while the clock is stopped so a shifted
               recovery window can be installed and contribute to readiness.
               The old harness stopped calling read_sample here and could
               deadlock its own refill even though the server had delivered
               the complete response. */
            if (!have_video_payload && (buffering || (due && sync_ready))) {
                if (media_mp4_read_sample(
                        demux, &sample, payload, payload_capacity)) {
                    if (video_hold_started_us != 0) {
                        video_held_us += now_us - video_hold_started_us;
                        video_hold_started_us = 0;
                    }
                    have_video_payload = true;
                } else if (media_mp4_would_block(demux)) {
                    source_blocked = true;
                    if (video_hold_started_us == 0) {
                        video_hold_started_us = now_us;
                        video_holds++;
                    }
                } else {
                    break;
                }
            }
            if (!buffering && due) {
                if (!sync_ready) {
                    sync_holds++;
                } else if (have_video_payload) {
                    video_presented++;
                    video_media_us = next_video_us;
                    have_video_payload = false;
                    have_video_sample = false;
                }
            }
        }
        MediaHttpRangeStats video_now = {0};
        MediaHttpRangeStats audio_now = {0};
        CHECK(media_http_range_stats(video, &video_now));
        CHECK(media_http_range_stats(audio, &audio_now));
        bool fill_pending = video_now.window_pending
            || audio_now.window_pending;
        uint64_t duration_us = info.timescale == 0 ? 0
            : info.duration * UINT64_C(1000000) / info.timescale;
        uint64_t video_ahead_us =
            media_http_range_buffered_ahead_us(video, duration_us);
        uint64_t audio_ahead_us =
            media_http_range_buffered_ahead_us(audio, duration_us);
        media_http_range_set_lookahead_limit(
            video, psp_media_video_lookahead_limit(true, audio_ahead_us));
        uint64_t network_ahead_us = video_ahead_us < audio_ahead_us
            ? video_ahead_us : audio_ahead_us;
        uint64_t media_clock_us = video_media_us < audio_media_us
            ? video_media_us : audio_media_us;
        uint64_t remaining_us = run_media_us > media_clock_us
            ? run_media_us - media_clock_us : 0;
        PspMediaBufferPolicyDecision buffer_decision =
            psp_media_buffer_policy((PspMediaBufferPolicyInput) {
                .playing = true,
                .buffering = buffering,
                .source_blocked = source_blocked,
                .fill_pending = fill_pending,
                .buffer_events = buffer_begins,
                .now_us = now_us,
                .starved_since_us = buffer_starved_since_us,
                .ready_since_us = buffer_ready_since_us,
                .remaining_us = remaining_us,
                /* This scenario isolates transport and the presentation
                   policy; PSP decode readiness is covered by the ownership
                   and promotion gates. Give the policy its exact minimum. */
                .decoded_ahead_us = PSP_MEDIA_BUFFER_DECODE_READY_US,
                .network_ahead_us = network_ahead_us
            });
        buffer_starved_since_us = buffer_decision.starved_since_us;
        buffer_ready_since_us = buffer_decision.ready_since_us;
        if (buffer_decision.action == PSP_MEDIA_BUFFER_BEGIN) {
            CHECK(!buffering);
            buffering = true;
            buffer_started_us = now_us;
            buffer_starved_since_us = 0;
            buffer_ready_since_us = 0;
            if (last_buffer_ended_us != 0) {
                uint64_t clear_us = now_us - last_buffer_ended_us;
                if (clear_us < shortest_clear_us) shortest_clear_us = clear_us;
            }
            buffer_begins++;
        } else if (buffer_decision.action == PSP_MEDIA_BUFFER_END) {
            CHECK(buffering && now_us >= buffer_started_us);
            uint64_t visible_us = now_us - buffer_started_us;
            if (visible_us < shortest_buffer_us)
                shortest_buffer_us = visible_us;
            timeline_paused_us += visible_us;
            buffering = false;
            buffer_started_us = 0;
            buffer_starved_since_us = 0;
            buffer_ready_since_us = 0;
            last_buffer_ended_us = now_us;
            buffer_ends++;
        }
        if (video_presented != 0 && audio_blocks != 0) {
            uint64_t skew = video_media_us > audio_media_us
                ? video_media_us - audio_media_us
                : audio_media_us - video_media_us;
            if (skew > maximum_skew_us) maximum_skew_us = skew;
            if (skew > maximum_track_lead_us) {
                /* Time correction only while correction is possible. A CDN
                   outage and the intentional buffering hold are availability
                   time, not a skew-controller response; restart the clock at
                   the first unblocked presentation opportunity. */
                if (buffering || source_blocked) {
                    skew_excursion_started_us = 0;
                } else if (skew_excursion_started_us == 0) {
                    skew_excursion_started_us = now_us;
                }
            } else if (skew_excursion_started_us != 0) {
                uint64_t recovery_us = now_us - skew_excursion_started_us;
                if (recovery_us > maximum_skew_recovery_us)
                    maximum_skew_recovery_us = recovery_us;
                skew_excursion_started_us = 0;
            }
        }
        usleep(1000);
    }
    if (video_hold_started_us != 0)
        video_held_us += tilefinch_platform_monotonic_time_us()
            - video_hold_started_us;
    if (audio_hold_started_us != 0)
        audio_held_us += tilefinch_platform_monotonic_time_us()
            - audio_hold_started_us;
    MediaHttpRangeStats video_stats = {0};
    MediaHttpRangeStats audio_stats = {0};
    CHECK(media_http_range_stats(video, &video_stats));
    CHECK(media_http_range_stats(audio, &audio_stats));
    printf(
        "coupled(%s): video=%u holds=%u/%llums audio=%u holds=%u/%llums "
        "max-skew=%llums skew-recovery=%llums sync-holds=%u requests=%zu/%zu "
        "reconnects=%zu/%zu starved=%zu/%zu retries=%zu/%zu failures=%zu/%zu "
        "buffer-ui=%u/%u shortest=%llums clear=%llums "
        "video-window=pending:%u stalled:%u bytes:%zu "
        "fill:%llu+%zu cache:%llu+%zu read:%llu+%zu http:%ld\n",
        profile == NULL ? "unknown" : profile,
        video_presented, video_holds,
        (unsigned long long) (video_held_us / 1000u),
        audio_blocks, audio_holds,
        (unsigned long long) (audio_held_us / 1000u),
        (unsigned long long) (maximum_skew_us / 1000u),
        (unsigned long long) (maximum_skew_recovery_us / 1000u), sync_holds,
        video_stats.requests, audio_stats.requests,
        video_stats.reconnects, audio_stats.reconnects,
        video_stats.starved_reconnects, audio_stats.starved_reconnects,
        video_stats.retry_attempts, audio_stats.retry_attempts,
        video_stats.failures, audio_stats.failures,
        buffer_begins, buffer_ends,
        (unsigned long long) (shortest_buffer_us == UINT64_MAX
            ? 0 : shortest_buffer_us / 1000u),
        (unsigned long long) (shortest_clear_us == UINT64_MAX
            ? 0 : shortest_clear_us / 1000u),
        video_stats.window_pending ? 1u : 0u,
        video_stats.delivery_stalled ? 1u : 0u,
        video_stats.bytes_in_flight,
        (unsigned long long) video_stats.fill_offset,
        video_stats.fill_length,
        (unsigned long long) video_stats.cache_offset,
        video_stats.cache_length,
        (unsigned long long) video_stats.last_read_offset,
        video_stats.last_read_length,
        video_stats.last_http_status);
    /* Whole 41.7 ms video frames and 23.2 ms AAC blocks can cross the 75 ms
       hold edge in one scheduling visit. Bound that discrete overshoot to a
       second 75 ms, then independently require prompt convergence below. */
    uint64_t skew_ceiling_us = profile != NULL
            && strcmp(profile, "chaos-recovery") == 0
        /* Before the deliberate buffering hold begins, the 350 ms anti-
           flutter debounce may let the audible cursor advance while video
           has no bytes. Bound that designed interval plus the 75 ms track
           lead; post-refill convergence is still gated separately below. */
        ? PSP_MEDIA_BUFFER_DEBOUNCE_US + maximum_track_lead_us
        : maximum_track_lead_us + UINT64_C(75000);
    CHECK(maximum_skew_us <= skew_ceiling_us);
    CHECK(skew_excursion_started_us == 0
          && maximum_skew_recovery_us <= skew_recovery_limit_us);
    CHECK(video_stats.failures == 0 && audio_stats.failures == 0);
    if (profile != NULL && strcmp(profile, "chaos-recovery") == 0) {
        /* The composite schedule must actually exercise both recovery laws
           and present stable buffering surfaces rather than fluttering at
           burst boundaries. The long outage and later response loss may each
           create one episode, but every episode must close after a stable
           refill. */
        CHECK(video_stats.reconnects >= 2u
              && video_stats.requests >= 5u);
        CHECK(buffer_begins >= 1u && buffer_begins == buffer_ends);
        CHECK(shortest_buffer_us >= PSP_MEDIA_BUFFER_STABLE_US);
        CHECK(shortest_clear_us == UINT64_MAX
              || shortest_clear_us >= PSP_MEDIA_BUFFER_DEBOUNCE_US);
        CHECK(buffer_begins <= 2u);
    }
    free(payload);
    media_mp4_close(demux);
    media_http_range_destroy(video);
    media_http_range_destroy(audio);
    CHECK(budget.current == 0);
    return complete;
}

static void test_deterministic_cadence(
    int port, uint64_t length, const char *profile)
{
    if (profile != NULL
        && (strcmp(profile, "coupled-asymmetric") == 0
            || strcmp(profile, "chaos-recovery") == 0)) {
        CHECK(run_coupled_cadence(port, length, profile));
        return;
    }
    CadenceResult large = {0};
    if (profile != NULL && strcmp(profile, "device-gaps") == 0) {
        CadenceResult compact = {0};
        CadenceResult small = {0};
        CHECK(run_cadence_case(
            port, length, profile, 64u * 1024u, &compact));
        CHECK(run_cadence_case(
            port, length, profile, 128u * 1024u, &small));
    }
    CHECK(run_cadence_case(
        port, length, profile, 256u * 1024u, &large));
    if (profile != NULL && strcmp(profile, "trickle-reconnect") == 0) {
        /* The server injects the first-attempt trickle independently for
           every post-opening byte window. A low-bitrate capture may cross
           one such window during the 30 s sample; a 30 fps 360p capture
           crosses several. Pin bounded recovery, not one fixture's count. */
        CHECK(large.transport.reconnects >= 1
              && large.transport.starved_reconnects >= 1
              && large.transport.starved_reconnects
                     <= large.transport.reconnects
              && large.longest_hold_us
                     < MEDIA_HTTP_TRICKLE_WINDOW_US / 2u);
    }
    if (profile != NULL && strcmp(profile, "live-cdn-prefetch") == 0) {
        /* Pin the device failure, not merely a successful eventual read: the
           successor must have left before demand reached it, the native
           transport must have accepted its request shape, and PSP-shaped
           delivery must remain hidden behind the current cache window. */
        CHECK(large.transport.readahead_requests >= 2u);
        CHECK(large.transport.readahead_issue_refusals == 0u);
        CHECK(large.hold_events == 0u);
        CHECK(large.held_us == 0u);
    }
    /* This is a measurement, not a baked-in winner: changes print both
       complete funnels and the device policy is chosen from the result. */
}

int main(int argc, char **argv)
{
    char range_header[64];
    uint64_t complete_length = 0;
    CHECK(media_http_build_range_header(
              17, 39, range_header, sizeof(range_header))
          && strcmp(range_header, "Range: bytes=17-39") == 0
          && media_http_parse_content_range(
                 "bytes 17-39/4096", 17, 39, 23,
                 &complete_length)
          && complete_length == 4096
          && !media_http_parse_content_range(
                 "bytes 17-38/4096", 17, 39, 23,
                 &complete_length));
    test_buffering_policy_hysteresis();
    test_buffering_burst_and_slow_recovery_scenario();
    test_range_window_liveness_policy();
    test_video_lookahead_policy();
    test_optional_second_lookahead_budget_fallback();
    if (argc == 2 && strcmp(argv[1], "--policy-only") == 0) {
        if (failures != 0) {
            printf("media-window-policy-tests: %d check(s) failed\n",
                   failures);
            return 1;
        }
        puts("media-window-policy-tests: all checks passed");
        return 0;
    }
    if (argc != 6 && argc != 9) {
        printf("usage: %s PORT LENGTH FRAGMENTED-LENGTH FRAGMENTS "
               "SAMPLES-PER-FRAGMENT [--cadence LENGTH PROFILE]\n",
               argc > 0 ? argv[0] : "test");
        return 2;
    }
    int port = atoi(argv[1]);
    uint64_t length = strtoull(argv[2], NULL, 10);
    uint64_t fragmented_length = strtoull(argv[3], NULL, 10);
    unsigned fragments = (unsigned) strtoul(argv[4], NULL, 10);
    unsigned per_fragment = (unsigned) strtoul(argv[5], NULL, 10);
    if (argc == 9) {
        if (strcmp(argv[6], "--cadence") != 0) return 2;
        printf("test: deterministic captured-video cadence (%s)\n", argv[8]);
        test_deterministic_cadence(
            port, strtoull(argv[7], NULL, 10), argv[8]);
        if (failures != 0) {
            printf("media-cadence-tests: %d check(s) failed\n", failures);
            return 1;
        }
        puts("media-cadence-tests: all checks passed");
        return 0;
    }

    puts("test: a blocking open read installs the window it just fetched");
    test_blocking_open_read(port, length, "query200", 200);
    puts("test: an admitted metadata window is reused by open");
    test_metadata_prefetch_is_reused(port, length);
    puts("test: a bounded partial response installs the same way");
    test_blocking_open_read(port, length, "partial206", 206);
    puts("test: an HTML media source uses a standard Range header");
    test_standard_range_header(port, length);
    puts("test: an audio-only source advertises an audio representation");
    test_audio_range_representation(port, length);
    puts("test: a read across a window boundary fetches both");
    test_window_crossing(port, length);
    puts("test: a poll never waits and completes on a later pump");
    test_poll_never_waits(port, length);
    puts("test: a nonblocking read spanning a cache boundary completes");
    test_poll_crossing_window_eventually_completes(port, length);
    puts("test: a shifted recovery owns a fresh bounded retry scope");
    test_shifted_recovery_has_its_own_retry_scope(port, length);
    puts("test: readahead starts before the window runs out");
    test_readahead_starts_before_the_window_runs_out(port, length);
    puts("test: completed readahead bridges one logical read");
    test_completed_readahead_bridges_a_logical_read(port, length);
    puts("test: owned lookahead pipelines three sequential windows");
    test_owned_lookahead_pipelines_three_windows(port, length);
    puts("test: second lookahead waits for healthy audio reserve");
    test_second_lookahead_waits_for_audio_reserve(port, length);
    puts("test: a streamed successor prefix is visible before EOF");
    test_streamed_successor_is_visible_before_eof(port, length);
    puts("test: a stream without Content-Length waits for EOF admission");
    test_stream_without_length_falls_back_to_terminal_admission(port, length);
    puts("test: rejected stream headers fail without wedging");
    test_rejected_stream_headers_fail_without_wedging(port, length);
    puts("test: an expired media URL is classified and can be replaced");
    test_expired_url_is_classified_and_replacement_resumes(port, length);
    puts("test: startup proves delivery beyond the signed URL prefix");
    test_successor_prime_rejects_prefix_only_delivery(port, length);
    puts("test: repeated connection resets stop after one retry");
    test_repeated_connection_reset_is_bounded(port, length);
    puts("test: corrupt range responses are never admitted");
    test_corrupt_range_responses_are_never_admitted(port, length);
    puts("test: a short body is retried once and then reported");
    test_short_body_is_reported(port, length);
    puts("test: a fragmented MP4 is read across its fragment boundaries");
    test_fragment_boundaries_cross_over_real_windows(
        port, fragmented_length, fragments, per_fragment, "fragment", false);
    puts("test: a fragment boundary that lands on a window still in flight");
    test_fragment_boundaries_cross_over_real_windows(
        port, fragmented_length, fragments, per_fragment,
        "fragment-slow", true);
    puts("test: a pending window advances only across a wait that pumps");
    test_a_pending_window_needs_someone_to_pump(port, fragmented_length);
    puts("test: slow useful bytes are retained without reconnect churn");
    test_slow_progress_is_retained(port, length);
    puts("test: a prolonged demanded window remains supersedable");
    test_prolonged_window_can_be_superseded(port, length);
    puts("test: one transaction budget bounds every blocking read in it");
    test_a_transaction_budget_bounds_every_read_in_it(port, length);
    puts("test: a stalled read answers a stop request inside the law");
    test_a_stalled_read_answers_cancellation_inside_the_law(port, length);

    if (failures != 0) {
        printf("media-range-tests: %d check(s) failed\n", failures);
        return 1;
    }
    puts("media-range-tests: all checks passed");
    return 0;
}
