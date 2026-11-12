/*
 * Page audio over real HTTP: the probe the PSP open path sends
 * (psp_media_open.c, psp_media_page_probe) and the MP3 source reading the
 * stream's own response, through real libcurl against the loopback server
 * in tests/run_media_mp3_stream_test.py.
 *
 *   probe URL DOCUMENT   prints the route the probe chose
 *   probe-scheduled URL DOCUMENT
 *                        the same probe through a FetchScheduler
 *   stream URL FRAMES    reads FRAMES frames through a MediaMp3Source and
 *                        prints what it saw; the source opens as live (no
 *                        length), as after a probe that overflowed, and
 *                        learns the response's Content-Length as the PSP
 *                        transport reports it
 *
 * The stream mode feeds the response body to the source through the
 * transport seam (MediaMp3Transport), as the PSP worker's poll does, and
 * drains frames as the decoder would; it stops the transfer once it has
 * read enough, which is how a live stream ends when the player leaves.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp3.h"
#include "tilefinch/session.h"

/* The probe exactly as psp_media_page_probe builds it for page audio.
   scheduled: through a FetchScheduler, as the PSP sends it (the scheduler
   copies the request and configures its own handle); otherwise through the
   direct transport. */
static int run_probe(const char *url, const char *document, bool scheduled)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    BrowserSession session;
    if (!browser_session_init(&session, &budget, 128u * 1024u)) return 2;
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = document,
        .top_level_url = document, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_AUDIO
    };
    char range_header[64];
    FetchRequest transport = {
        .allow_http_errors = true,
        .accept = "audio/mpeg,audio/mp4,audio/*;q=0.9,*/*;q=0.5",
        .icy_status_line = true,
        .connect_timeout_ms = 3000,
        .redirect_same_origin_only = false
    };
    FetchPreparedPageRequest *prepared = budget_malloc(
        &budget, sizeof(*prepared));
    FetchResult *result = fetch_result_create(&budget);
    if (prepared == NULL || result == NULL
        || !media_http_build_probe_range_header(
               range_header, sizeof(range_header))) return 2;
    transport.extra_headers = range_header;
    if (!fetch_prepare_page_request_context(
            &context, document, NULL, &session, NULL, NULL, &transport,
            prepared, NULL)) return 2;
    bool fetched = false;
    if (scheduled) {
        FetchScheduler *scheduler = fetch_scheduler_create(
            &budget, 1u, MEDIA_HTTP_PROBE_SNIFF_BYTES);
        if (scheduler == NULL) return 2;
        fetched = fetch_scheduler_request(
            scheduler, url, fetch_prepared_page_request(prepared),
            MEDIA_HTTP_PROBE_SNIFF_BYTES, 15000, result);
        fetch_scheduler_destroy(scheduler);
    } else {
        fetched = fetch_request_cancelable(
            &budget, url, fetch_prepared_page_request(prepared),
            MEDIA_HTTP_PROBE_SNIFF_BYTES, 15000, NULL, NULL, result);
    }
    uint64_t length = 0;
    bool seekable = false;
    MediaHttpProbeRoute route = media_http_probe_route_audio(
        &session, &context, result, fetched, &length, &seekable);
    printf("route=%s length=%llu seekable=%d fetched=%d limit=%d http=%ld\n",
           route == MEDIA_HTTP_PROBE_MP3 ? "mp3"
           : route == MEDIA_HTTP_PROBE_RANGE ? "range"
           : route == MEDIA_HTTP_PROBE_HLS ? "hls"
           : route == MEDIA_HTTP_PROBE_UNAVAILABLE ? "unavailable"
           : "refused",
           (unsigned long long) length, seekable ? 1 : 0, fetched ? 1 : 0,
           result->response_limit_exceeded ? 1 : 0, result->status_code);
    fetch_result_free(result);
    budget_free(&budget, prepared);
    browser_session_destroy(&session);
    return budget.current == 0 ? 0 : 3;
}

/* Response bytes the transfer delivered and the source has not polled. */
typedef struct {
    unsigned char staged[64u * 1024u];
    size_t staged_length;
    bool started;
    bool complete;
    MediaMp3Source *source;
    MediaSampleSource samples;
    uint64_t frames;
    uint64_t wanted;
    uint64_t now_us;
    uint64_t next_dts;
    size_t first_current;
    size_t peak_current;
    Budget *budget;
    bool broken;
    uint64_t declared_length;
} StreamState;

static uint64_t stream_start(void *opaque, uint64_t offset, char *error,
                             size_t error_size)
{
    StreamState *state = opaque;
    /* One response per run: this harness does not reconnect. */
    if (state->started || offset != 0) {
        snprintf(error, error_size, "%s", "one response only");
        return 0;
    }
    state->started = true;
    return 1;
}

static MediaHlsTransportPollResult stream_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    StreamState *state = opaque;
    (void) error;
    (void) error_size;
    *length = 0;
    if (handle != 1u) return MEDIA_HLS_TRANSPORT_ERROR;
    if (state->staged_length == 0)
        return state->complete ? MEDIA_HLS_TRANSPORT_COMPLETE
                               : MEDIA_HLS_TRANSPORT_WAIT;
    size_t take = state->staged_length < capacity
        ? state->staged_length : capacity;
    memcpy(destination, state->staged, take);
    memmove(state->staged, state->staged + take, state->staged_length - take);
    state->staged_length -= take;
    *length = take;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static uint64_t stream_declared_length(void *opaque, uint64_t handle)
{
    StreamState *state = opaque;
    return handle == 1u ? state->declared_length : 0;
}

/* The response's Content-Length, read as psp_media_hls.c reads it. */
static bool stream_headers(void *opaque, const FetchResult *metadata)
{
    StreamState *state = opaque;
    char value[32];
    if (fetch_response_header_value(metadata, "content-length", value,
                                    sizeof(value)))
        state->declared_length = strtoull(value, NULL, 10);
    return true;
}

static void stream_cancel(void *opaque, uint64_t handle)
{
    (void) opaque;
    (void) handle;
}

/* Pump and drain as the player does, until the staged bytes are taken or
   the source wants more. */
static void stream_drain(StreamState *state)
{
    for (unsigned spins = 0; spins < 100000u && !state->broken; spins++) {
        if (state->frames >= state->wanted) return;
        media_mp3_source_pump(state->source, state->now_us);
        state->now_us += 1000u;
        MediaMp4Sample sample;
        if (!state->samples.ops->next_sample(state->samples.opaque,
                                             &sample)) {
            if (state->samples.ops->would_block(state->samples.opaque)
                && state->staged_length == 0) return;
            if (state->samples.ops->would_block(state->samples.opaque))
                continue;
            return;
        }
        unsigned char packet[MEDIA_MP3_MAXIMUM_FRAME_BYTES];
        MediaMp3FrameHeader header;
        if (sample.size > sizeof(packet)
            || !state->samples.ops->read_sample(
                   state->samples.opaque, &sample, packet, sizeof(packet))
            || !media_mp3_parse_header(packet, &header)
            || header.frame_bytes != sample.size
            || (state->frames != 0 && sample.dts != state->next_dts)) {
            state->broken = true;
            return;
        }
        state->next_dts = sample.dts + sample.duration;
        state->frames++;
        if (state->frames == 1u) state->first_current = state->budget->current;
        if (state->budget->current > state->peak_current)
            state->peak_current = state->budget->current;
    }
}

static bool stream_body(void *opaque, const unsigned char *data,
                        size_t length)
{
    StreamState *state = opaque;
    while (length != 0) {
        size_t room = sizeof(state->staged) - state->staged_length;
        size_t take = length < room ? length : room;
        memcpy(state->staged + state->staged_length, data, take);
        state->staged_length += take;
        data += take;
        length -= take;
        stream_drain(state);
        if (state->broken || state->frames >= state->wanted) return false;
        if (take == 0) return false;
    }
    return true;
}

static int run_stream(const char *url, uint64_t wanted)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    StreamState *state = calloc(1, sizeof(*state));
    if (state == NULL) return 2;
    state->wanted = wanted;
    state->budget = &budget;
    MediaMp3Transport transport = {
        .opaque = state, .start = stream_start, .poll = stream_poll,
        .cancel = stream_cancel, .declared_length = stream_declared_length
    };
    MediaMp3SourceOptions options = {0};
    char error[160] = {0};
    state->source = media_mp3_source_create(
        &budget, &transport, &options, error, sizeof(error));
    if (state->source == NULL
        || !media_mp3_source_sample_source(state->source, &state->samples)) {
        fprintf(stderr, "source: %s\n", error);
        return 2;
    }
    FetchRequest request = {
        .accept = "audio/mpeg,audio/*;q=0.9,*/*;q=0.5",
        .icy_status_line = true,
        .connect_timeout_ms = 3000
    };
    FetchStreamOptions stream = {.on_headers = stream_headers,
                                 .on_body = stream_body, .opaque = state};
    FetchStreamMetrics metrics = {0};
    FetchResult *result = fetch_result_create(&budget);
    if (result == NULL) return 2;
    bool fetched = fetch_request_stream_cancelable(
        &budget, url, &request, 64u * 1024u * 1024u, 30000, NULL, NULL,
        &stream, &metrics, result);
    if (fetched) {
        state->complete = true;
        stream_drain(state);
    }
    MediaMp3Stats stats;
    media_mp3_source_stats(state->source, &stats);
    printf("frames=%llu rate=%u channels=%u live=%d ended=%d failed=%d "
           "sync_losses=%u skipped=%llu received=%llu first=%zu peak=%zu "
           "broken=%d fetched=%d http=%ld\n",
           (unsigned long long) state->frames, (unsigned) stats.sample_rate,
           (unsigned) stats.channels, stats.live ? 1 : 0,
           stats.ended ? 1 : 0,
           media_mp3_source_failed(state->source) ? 1 : 0, stats.sync_losses,
           (unsigned long long) stats.skipped_bytes,
           (unsigned long long) stats.bytes_received, state->first_current,
           state->peak_current, state->broken ? 1 : 0, fetched ? 1 : 0,
           result->status_code);
    fetch_result_free(result);
    media_mp3_source_destroy(state->source);
    free(state);
    return budget.current == 0 ? 0 : 3;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "probe") == 0)
        return run_probe(argv[2], argv[3], false);
    if (argc == 4 && strcmp(argv[1], "probe-scheduled") == 0)
        return run_probe(argv[2], argv[3], true);
    if (argc == 4 && strcmp(argv[1], "stream") == 0)
        return run_stream(argv[2], strtoull(argv[3], NULL, 10));
    fprintf(stderr, "usage: %s probe|probe-scheduled URL DOCUMENT | stream URL FRAMES\n",
            argv[0]);
    return 2;
}
