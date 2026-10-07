/* libFuzzer target: the HLS segment pipeline (src/media_hls.c source side):
   TS/ADTS segment bodies served by a fake transport flow through the TS
   demuxer, access-unit/ADTS callbacks, the sample queue and sample reads.

   Input: byte 0 selects the track (mixed/video/audio) and the transport
   chunk size; the remainder is split into three segment bodies at the
   first two occurrences of the separator "|SEG|" (or used whole for every
   segment). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_hls.h"

#define MIB (1024u * 1024u)
#define SEGMENTS 3u
#define REQUESTS 4u

typedef struct {
    const uint8_t *body[SEGMENTS];
    size_t length[SEGMENTS];
    size_t chunk;
    struct {
        bool active;
        size_t segment;
        size_t offset;
    } request[REQUESTS];
} FuzzTransport;

static uint64_t fuzz_start(void *opaque, const char *url, size_t maximum,
                           char *error, size_t error_size)
{
    FuzzTransport *transport = opaque;
    (void) maximum;
    (void) error;
    (void) error_size;
    size_t segment = 0;
    static const char *const names[SEGMENTS] = {"a.ts", "b.ts", "c.ts"};
    for (size_t i = 0; i < SEGMENTS; i++)
        if (strstr(url, names[i]) != NULL) segment = i;
    for (size_t i = 0; i < REQUESTS; i++) {
        if (!transport->request[i].active) {
            transport->request[i].active = true;
            transport->request[i].segment = segment;
            transport->request[i].offset = 0;
            return i + 1u;
        }
    }
    return 0;
}

static MediaHlsTransportPollResult fuzz_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    FuzzTransport *transport = opaque;
    (void) error;
    (void) error_size;
    *length = 0;
    if (handle == 0 || handle > REQUESTS
        || !transport->request[handle - 1u].active)
        return MEDIA_HLS_TRANSPORT_ERROR;
    size_t index = handle - 1u;
    size_t segment = transport->request[index].segment;
    size_t offset = transport->request[index].offset;
    if (offset == transport->length[segment]) {
        transport->request[index].active = false;
        return MEDIA_HLS_TRANSPORT_COMPLETE;
    }
    size_t bytes = transport->length[segment] - offset;
    if (bytes > transport->chunk) bytes = transport->chunk;
    if (bytes > capacity) bytes = capacity;
    memcpy(destination, transport->body[segment] + offset, bytes);
    transport->request[index].offset += bytes;
    *length = bytes;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static void fuzz_cancel(void *opaque, uint64_t handle)
{
    FuzzTransport *transport = opaque;
    if (handle != 0 && handle <= REQUESTS)
        transport->request[handle - 1u].active = false;
}

static const char vod[] =
    "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
    "#EXTINF:4,\na.ts\n#EXTINF:4,\nb.ts\n#EXT-X-DISCONTINUITY\n"
    "#EXTINF:4,\nc.ts\n#EXT-X-ENDLIST\n";
static const char live[] =
    "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:7\n"
    "#EXTINF:4,\na.ts\n#EXTINF:4,\nb.ts\n#EXTINF:4,\nc.ts\n";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 1024 * 1024) return 0;
    unsigned mode = data[0];
    uint8_t *body = malloc(size);
    if (body == NULL) return 0;
    memcpy(body, data + 1, size - 1);
    size_t body_length = size - 1;

    FuzzTransport transport;
    memset(&transport, 0, sizeof(transport));
    transport.chunk = 1u + (size_t) ((mode >> 2) & 15u) * 61u;
    static const char separator[] = "|SEG|";
    size_t start = 0;
    for (size_t i = 0; i < SEGMENTS; i++) {
        const uint8_t *found = NULL;
        if (i + 1 < SEGMENTS && start <= body_length) {
            for (size_t at = start; at + 5 <= body_length; at++) {
                if (memcmp(body + at, separator, 5) == 0) {
                    found = body + at;
                    break;
                }
            }
        }
        size_t end = found != NULL ? (size_t) (found - body) : body_length;
        transport.body[i] = body + (start < body_length ? start : body_length);
        transport.length[i] = start < end ? end - start : 0;
        if (found == NULL) {
            /* No more separators: reuse this body for later segments. */
            for (size_t j = i + 1; j < SEGMENTS; j++) {
                transport.body[j] = transport.body[i];
                transport.length[j] = transport.length[i];
            }
            break;
        }
        start = end + 5;
    }

    Budget budget;
    budget_init(&budget, 24u * MIB);
    char error[256];
    const char *text = (mode & 0x40u) != 0 ? live : vod;
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.test/v/index.m3u8",
        (const unsigned char *) text, strlen(text), error, sizeof(error));
    if (playlist == NULL) abort();
    MediaHlsTransport hls_transport = {
        .opaque = &transport, .start = fuzz_start,
        .poll = fuzz_poll, .cancel = fuzz_cancel
    };
    MediaHlsTrackSelection selection = (MediaHlsTrackSelection) (mode % 3u);
    MediaHlsSource *source = media_hls_source_create_track(
        &budget, playlist, &hls_transport, selection, error, sizeof(error));
    if (source == NULL) {
        media_hls_playlist_destroy(playlist);
    } else {
        MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
        for (unsigned i = 0; i < 256u && status == MEDIA_HLS_PRIME_PENDING;
             i++)
            status = media_hls_source_prime(source, error, sizeof(error));
        MediaSampleSource samples;
        if (media_hls_source_sample_source(source, &samples)) {
            MediaMp4TrackInfo video, audio;
            (void) media_hls_source_stream_info(source, &video, &audio);
            size_t tracks = samples.ops->track_count(samples.opaque);
            for (size_t i = 0; i < tracks; i++) {
                MediaMp4TrackInfo info;
                (void) samples.ops->track_info(samples.opaque, i, &info);
            }
            unsigned char *payload = malloc(512u * 1024u);
            for (unsigned tick = 1; payload != NULL && tick < 2000u; tick++) {
                uint64_t now_us = (uint64_t) tick * UINT64_C(50000);
                media_hls_source_pump(source, now_us);
                if ((mode & 0x40u) != 0
                    && media_hls_source_wants_playlist_refresh(source,
                                                               now_us)) {
                    MediaHlsPlaylist *next = media_hls_playlist_parse(
                        &budget, "https://media.test/v/index.m3u8",
                        (const unsigned char *) live, strlen(live), error,
                        sizeof(error));
                    if (next != NULL)
                        (void) media_hls_source_update_playlist(
                            source, next, now_us, error, sizeof(error));
                }
                MediaMp4Sample sample;
                if (samples.ops->next_sample(samples.opaque, &sample)) {
                    if (sample.size <= 512u * 1024u)
                        (void) samples.ops->read_sample(
                            samples.opaque, &sample, payload, sample.size);
                } else if (media_hls_source_failed(source)
                           || !samples.ops->would_block(samples.opaque)) {
                    break;
                }
                if (tick == 700u) {
                    uint64_t actual = 0;
                    (void) samples.ops->seek_us(samples.opaque, 4000000u,
                                                &actual);
                }
            }
            free(payload);
            (void) samples.ops->retained_bytes(samples.opaque);
        }
        MediaHlsStats stats;
        media_hls_source_stats(source, &stats);
        media_hls_source_destroy(source);
    }
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_hls_source: budget leak of %zu bytes\n",
                budget.current);
        abort();
    }
    free(body);
    return 0;
}
