#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/media_http.h"
#include "tilefinch/platform.h"
#include "tilefinch/session.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/youtube_resolver.h"

static bool download(Budget *budget, BrowserSession *session,
                     const char *referer, const char *url,
                     uint64_t expected, const char *path)
{
    if (expected == 0 || expected >= SIZE_MAX) return false;
    char error[256] = {0};
    MediaHttpRangeOptions options = {
        .cache_bytes = 64u * 1024u,
        .timeout_ms = 15000,
        .referer = referer,
        .url_validator = youtube_media_url_supported
    };
    MediaHttpRange *range = media_http_range_create(
        budget, session, url, expected, &options, error, sizeof(error));
    FILE *file = range == NULL ? NULL : fopen(path, "wb");
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char chunk[64u * 1024u];
    uint64_t offset = 0;
    bool written = file != NULL;
    while (written && offset < expected) {
        size_t wanted = expected - offset < sizeof(chunk)
            ? (size_t) (expected - offset) : sizeof(chunk);
        written = reader.read(reader.opaque, offset, chunk, wanted)
            && fwrite(chunk, 1, wanted, file) == wanted;
        offset += wanted;
    }
    if (file != NULL && fclose(file) != 0) written = false;
    if (!written) {
        MediaHttpRangeStats stats = {0};
        (void) media_http_range_stats(range, &stats);
        fprintf(
            stderr,
            "download failed: %s offset=%" PRIu64
            " requests=%zu failures=%zu status=%ld last=%" PRIu64
            "-%" PRIu64 "\n",
            error[0] == '\0' ? "range read or output write failed" : error,
            offset, stats.requests, stats.failures, stats.last_http_status,
            stats.last_first_byte, stats.last_last_byte);
    } else {
        printf("downloaded=%s bytes=%" PRIu64 "\n", path, expected);
    }
    media_http_range_destroy(range);
    return written;
}

static bool probe_remote_demux(
    Budget *budget, BrowserSession *session, const char *referer,
    const char *url, uint64_t content_length, const char *label,
    bool traverse)
{
    MediaHttpRangeOptions options = {
        .cache_bytes = 256u * 1024u,
        .lookahead_windows = 1,
        .timeout_ms = 15000,
        .referer = referer,
        .url_validator = youtube_media_url_supported
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, session, url, content_length, &options,
        error, sizeof(error));
    if (range == NULL) {
        fprintf(stderr, "%s range create failed: %s\n", label, error);
        return false;
    }
    MediaRangeReader reader = media_http_range_reader(range);
    MediaMp4Demux *demux = media_mp4_open(
        budget, &reader, NULL, error, sizeof(error));
    MediaHttpRangeStats stats = {0};
    (void) media_http_range_stats(range, &stats);
    if (demux == NULL) {
        fprintf(
            stderr,
            "%s remote demux failed: %s "
            "(requests=%zu retry=%zu bytes=%zu fail=%zu http=%ld)\n",
            label, error, stats.requests, stats.retry_attempts,
            stats.bytes_received, stats.failures, stats.last_http_status);
        media_http_range_destroy(range);
        return false;
    }
    printf(
        "%s-remote-demux=PASS tracks=%zu retained=%zu "
        "requests=%zu retry=%zu bytes=%zu fail=%zu http=%ld\n",
        label, media_mp4_track_count(demux),
        media_mp4_retained_bytes(demux), stats.requests,
        stats.retry_attempts, stats.bytes_received,
        stats.failures, stats.last_http_status);
    bool traversed = true;
    MediaHttpRangePrimeStatus primed = MEDIA_HTTP_RANGE_PRIME_PENDING;
    for (unsigned step = 0; step < 20000u; step++) {
        primed = media_http_range_prime_successor(range);
        if (primed != MEDIA_HTTP_RANGE_PRIME_PENDING) break;
        usleep(1000);
    }
    if (primed != MEDIA_HTTP_RANGE_PRIME_READY) {
        char detail[256] = {0};
        (void) reader.describe_failure(
            reader.opaque, detail, sizeof(detail));
        (void) media_http_range_stats(range, &stats);
        fprintf(
            stderr,
            "%s delivery prime failed: %s "
            "(requests=%zu retry=%zu fail=%zu http=%ld)\n",
            label, detail[0] == '\0' ? "later range unavailable" : detail,
            stats.requests, stats.retry_attempts,
            stats.failures, stats.last_http_status);
        traversed = false;
    }
    if (traverse && traversed) {
        size_t maximum_sample = media_mp4_default_limits().maximum_sample_bytes;
        for (size_t i = 0; i < media_mp4_track_count(demux); i++) {
            MediaMp4TrackInfo info = {0};
            if (media_mp4_track_info(demux, i, &info)
                && info.largest_sample > maximum_sample)
                maximum_sample = info.largest_sample;
        }
        unsigned char *packet = maximum_sample == 0 ? NULL
            : budget_malloc_category(
                  budget, BUDGET_CATEGORY_RESOURCE, maximum_sample);
        traversed = packet != NULL;
        bool reached_end = false;
        MediaMp4Sample sample = {0};
        bool have_sample = false;
        size_t samples = 0;
        uint64_t next_report_us =
            tilefinch_platform_monotonic_time_us() + UINT64_C(5000000);
        for (size_t step = 0; traversed && step < 1000000u; step++) {
            uint64_t now_us = tilefinch_platform_monotonic_time_us();
            if (now_us >= next_report_us) {
                MediaHttpRangeStats progress = {0};
                (void) media_http_range_stats(range, &progress);
                printf(
                    "%s-remote-progress samples=%zu pending=%d "
                    "inflight=%zu requests=%zu installs=%zu reconnects=%zu "
                    "cache=%" PRIu64 "+%zu fill=%" PRIu64 "+%zu "
                    "read=%" PRIu64 "+%zu\n",
                    label, samples, progress.window_pending ? 1 : 0,
                    progress.bytes_in_flight, progress.requests,
                    progress.window_installs, progress.reconnects,
                    progress.cache_offset, progress.cache_length,
                    progress.fill_offset, progress.fill_length,
                    progress.last_read_offset, progress.last_read_length);
                next_report_us = now_us + UINT64_C(5000000);
            }
            if (!have_sample && !media_mp4_next_sample(demux, &sample)) {
                if (media_mp4_would_block(demux)) {
                    (void) media_http_range_pump(range);
                    usleep(1000);
                    continue;
                }
                char detail[256] = {0};
                traversed = !media_mp4_last_error(
                    demux, detail, sizeof(detail));
                if (!traversed)
                    fprintf(stderr, "%s traversal failed: %s\n",
                            label, detail);
                else
                    reached_end = true;
                break;
            }
            have_sample = true;
            if (!media_mp4_read_sample(
                    demux, &sample, packet, maximum_sample)) {
                if (media_mp4_would_block(demux)) {
                    (void) media_http_range_pump(range);
                    usleep(1000);
                    continue;
                }
                char detail[256] = {0};
                (void) media_mp4_last_error(demux, detail, sizeof(detail));
                fprintf(stderr, "%s sample read failed: %s\n",
                        label, detail);
                traversed = false;
                break;
            }
            have_sample = false;
            samples++;
        }
        traversed = traversed && reached_end;
        (void) media_http_range_stats(range, &stats);
        printf(
            "%s-remote-traverse=%s samples=%zu requests=%zu "
            "installs=%zu bytes=%zu failures=%zu\n",
            label, traversed ? "PASS" : "FAIL", samples,
            stats.requests, stats.window_installs,
            stats.bytes_received, stats.failures);
        budget_free(budget, packet);
    }
    media_mp4_close(demux);
    media_http_range_destroy(range);
    return traversed;
}

/*
 * --hls-live SECONDS: open a live stream's HLS master as the PSP does
 * (psp_media_hls.c's decoder-aware selection), then run the shared live
 * source for SECONDS of wall time against a real-time playback clock. A
 * blocking fetch stands in for the PSP worker transport. Reports the chosen
 * variant, its SPS route, playlist reloads, skipped segments and every
 * stall where the clock passed the newest delivered sample.
 */
#define PROBE_HLS_SLOTS 4u
typedef struct {
    uint64_t handle;
    unsigned char *data;
    size_t length, offset;
} ProbeHlsSlot;
typedef struct {
    Budget *budget;
    ProbeHlsSlot slots[PROBE_HLS_SLOTS];
    uint64_t next_handle;
    size_t fetches;
} ProbeHlsTransport;

static unsigned char *probe_fetch(Budget *budget, const char *url,
                                  size_t maximum_bytes, size_t *length,
                                  char *error, size_t error_size)
{
    FetchRequest request = {
        .accept = "*/*",
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .redirect_same_origin_only = false
    };
    FetchResult *result = fetch_result_create(budget);
    unsigned char *copy = NULL;
    if (result != NULL && fetch_request_cancelable(
            budget, url, &request, maximum_bytes, 15000, NULL, NULL, result)
        && result->status_code == 200 && result->length != 0) {
        copy = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, result->length);
        if (copy != NULL) {
            memcpy(copy, result->data, result->length);
            *length = result->length;
        }
    } else {
        snprintf(error, error_size, "fetch failed (HTTP %ld)",
                 result == NULL ? 0L : result->status_code);
    }
    fetch_result_free(result);
    return copy;
}

static uint64_t probe_hls_start(void *opaque, const char *url,
                                size_t maximum_bytes, char *error,
                                size_t error_size)
{
    ProbeHlsTransport *transport = opaque;
    for (size_t i = 0; i < PROBE_HLS_SLOTS; i++) {
        ProbeHlsSlot *slot = &transport->slots[i];
        if (slot->handle != 0) continue;
        slot->data = probe_fetch(transport->budget, url, maximum_bytes,
                                 &slot->length, error, error_size);
        if (slot->data == NULL) return 0;
        slot->offset = 0;
        slot->handle = ++transport->next_handle;
        transport->fetches++;
        return slot->handle;
    }
    return 0;
}

static ProbeHlsSlot *probe_hls_slot(ProbeHlsTransport *transport,
                                    uint64_t handle)
{
    for (size_t i = 0; i < PROBE_HLS_SLOTS; i++)
        if (transport->slots[i].handle == handle)
            return &transport->slots[i];
    return NULL;
}

static void probe_hls_cancel(void *opaque, uint64_t handle)
{
    ProbeHlsTransport *transport = opaque;
    ProbeHlsSlot *slot = probe_hls_slot(transport, handle);
    if (slot == NULL) return;
    budget_free(transport->budget, slot->data);
    memset(slot, 0, sizeof(*slot));
}

static MediaHlsTransportPollResult probe_hls_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    ProbeHlsTransport *transport = opaque;
    ProbeHlsSlot *slot = probe_hls_slot(transport, handle);
    *length = 0;
    if (slot == NULL) {
        snprintf(error, error_size, "unknown transport handle");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    if (slot->offset == slot->length) {
        probe_hls_cancel(opaque, handle);
        return MEDIA_HLS_TRANSPORT_COMPLETE;
    }
    size_t take = slot->length - slot->offset;
    if (take > capacity) take = capacity;
    memcpy(destination, slot->data + slot->offset, take);
    slot->offset += take;
    *length = take;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static MediaHlsPlaylist *probe_hls_playlist(Budget *budget, const char *url,
                                            char *error, size_t error_size)
{
    size_t length = 0;
    unsigned char *bytes = probe_fetch(
        budget, url, MEDIA_HLS_MAXIMUM_PLAYLIST_DOWNLOAD_BYTES, &length,
        error, error_size);
    if (bytes == NULL) return NULL;
    /* The PSP's incremental capture: an oversized live media playlist keeps
       only its newest bounded window. */
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        budget, url, error, error_size);
    MediaHlsPlaylist *playlist = NULL;
    bool fed = stream != NULL;
    for (size_t at = 0; fed && at < length;
         at += MEDIA_HLS_TRANSPORT_CHUNK_BYTES) {
        size_t take = length - at < MEDIA_HLS_TRANSPORT_CHUNK_BYTES
            ? length - at : MEDIA_HLS_TRANSPORT_CHUNK_BYTES;
        fed = media_hls_playlist_stream_feed(
            stream, bytes + at, take, error, error_size);
    }
    if (fed) playlist = media_hls_playlist_stream_finish(
        stream, error, error_size);
    media_hls_playlist_stream_destroy(stream);
    budget_free(budget, bytes);
    return playlist;
}

static bool probe_live_hls(Budget *budget, const char *master_url,
                           unsigned seconds)
{
    char error[256] = {0};
    MediaHlsPlaylist *master = probe_hls_playlist(
        budget, master_url, error, sizeof(error));
    if (master == NULL) {
        fprintf(stderr, "hls master failed: %s\n", error);
        return false;
    }
    static char urls[2][4096];
    urls[0][0] = urls[1][0] = '\0';
    MediaHlsPlaylist *media[2] = {NULL, NULL};
    size_t count = 1;
    if (media_hls_playlist_kind(master) == MEDIA_HLS_PLAYLIST_MASTER) {
        MediaHlsDecoderLimits limits = {
            .software_width = 432u, .software_height = 240u,
            .target_height = 240u, .admit_undeclared_firmware = true
        };
        MediaHlsSelection selection = {0};
        bool selected = media_hls_playlist_select_decodable_streams(
            master, &limits, urls[0], sizeof(urls[0]),
            urls[1], sizeof(urls[1]), &selection);
        media_hls_playlist_destroy(master);
        if (!selected) {
            fprintf(stderr, "hls-variant=NONE video-selected=%d\n",
                    selection.video_selected ? 1 : 0);
            return false;
        }
        count = urls[1][0] != '\0' ? 2u : 1u;
        printf("hls-variant geometry=%ux%u unverified=%d separate-audio=%d\n",
               selection.width, selection.height,
               selection.unverified ? 1 : 0, count == 2u ? 1 : 0);
        for (size_t i = 0; i < count; i++)
            printf("hls-%s-url=%s\n", i == 0 ? "video" : "audio", urls[i]);
        for (size_t i = 0; i < count; i++) {
            media[i] = probe_hls_playlist(budget, urls[i], error,
                                          sizeof(error));
            if (media[i] == NULL) {
                fprintf(stderr, "hls media playlist failed: %s\n", error);
                media_hls_playlist_destroy(media[0]);
                return false;
            }
        }
    } else {
        snprintf(urls[0], sizeof(urls[0]), "%s", master_url);
        media[0] = master;
    }
    printf("hls-playlist live=%d segments=%zu\n",
           media_hls_playlist_is_live(media[0]) ? 1 : 0,
           media_hls_playlist_segment_count(media[0]));
    ProbeHlsTransport transport = {.budget = budget};
    MediaHlsTransport ops = {
        .opaque = &transport, .start = probe_hls_start,
        .poll = probe_hls_poll, .cancel = probe_hls_cancel
    };
    MediaHlsSource *sources[2] = {NULL, NULL};
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        sources[i] = media_hls_source_create_track(
            budget, media[i], &ops,
            count == 1u ? MEDIA_HLS_TRACK_MIXED
                : i == 0 ? MEDIA_HLS_TRACK_VIDEO : MEDIA_HLS_TRACK_AUDIO,
            error, sizeof(error));
        if (sources[i] == NULL) {
            media_hls_playlist_destroy(media[i]);
            fprintf(stderr, "hls source create failed: %s\n", error);
            ok = false;
        }
    }
    uint64_t started = tilefinch_platform_monotonic_time_us();
    for (size_t i = 0; ok && i < count; i++) {
        MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
        while (status == MEDIA_HLS_PRIME_PENDING
               && tilefinch_platform_monotonic_time_us() - started
                      < UINT64_C(30000000)) {
            status = media_hls_source_prime(sources[i], error, sizeof(error));
            if (status == MEDIA_HLS_PRIME_PENDING) {
                media_hls_source_pump(
                    sources[i], tilefinch_platform_monotonic_time_us());
                usleep(1000);
            }
        }
        if (status != MEDIA_HLS_PRIME_READY) {
            fprintf(stderr, "hls prime failed: %s\n", error);
            ok = false;
        }
    }
    MediaSampleSource samples[2];
    for (size_t i = 0; ok && i < count; i++)
        ok = media_hls_source_sample_source(sources[i], &samples[i]);
    printf("hls-prime=%s ms=%llu\n", ok ? "PASS" : "FAIL",
           (unsigned long long) ((tilefinch_platform_monotonic_time_us()
                                  - started) / 1000u));
    unsigned char *packet = ok
        ? budget_malloc_category(budget, BUDGET_CATEGORY_RESOURCE,
                                 MEDIA_HLS_MAXIMUM_SEGMENT_BYTES) : NULL;
    ok = ok && packet != NULL;
    size_t video = 0, audio = 0, refreshes = 0, refresh_failures = 0;
    size_t stalls = 0;
    uint64_t stall_us = 0, longest_stall_us = 0, stall_began = 0;
    bool sps_checked = false;
    int64_t first_pts_us = INT64_MIN, last_pts_us = INT64_MIN;
    uint64_t clock_origin = 0;
    MediaMp4Sample pending[2];
    bool have[2] = {false, false};
    uint64_t deadline = tilefinch_platform_monotonic_time_us()
        + (uint64_t) seconds * UINT64_C(1000000);
    uint64_t next_report = tilefinch_platform_monotonic_time_us()
        + UINT64_C(5000000);
    while (ok && tilefinch_platform_monotonic_time_us() < deadline) {
        uint64_t now = tilefinch_platform_monotonic_time_us();
        for (size_t i = 0; i < count; i++) {
            if (media_hls_source_wants_playlist_refresh(sources[i], now)) {
                MediaHlsPlaylist *next = probe_hls_playlist(
                    budget, urls[i], error, sizeof(error));
                if (next == NULL) {
                    refresh_failures++;
                    media_hls_source_note_playlist_refresh_failure(
                        sources[i], now);
                } else if (!media_hls_source_update_playlist(
                               sources[i], next, now,
                               error, sizeof(error))) {
                    refresh_failures++;
                } else {
                    refreshes++;
                }
            }
            media_hls_source_pump(sources[i], now);
            if (media_hls_source_failed(sources[i])) {
                char detail[256] = {0};
                (void) samples[i].ops->last_error(
                    samples[i].opaque, detail, sizeof(detail));
                fprintf(stderr, "hls source %zu failed: %s\n", i, detail);
                ok = false;
            }
        }
        bool starved = false;
        for (size_t i = 0; ok && i < count; i++) {
            for (unsigned taken = 0; taken < 64u; taken++) {
                if (!have[i]) {
                    if (!samples[i].ops->next_sample(
                            samples[i].opaque, &pending[i])) {
                        starved = starved || (clock_origin != 0
                            && samples[i].ops->would_block(samples[i].opaque));
                        break;
                    }
                    have[i] = true;
                }
                int64_t pts_us = pending[i].timescale == 0 ? 0
                    : (int64_t) ((double) pending[i].pts * 1000000.0
                                 / pending[i].timescale);
                if (first_pts_us == INT64_MIN) {
                    first_pts_us = pts_us;
                    clock_origin = now;
                }
                /* A real-time clock from the first sample: take what is due. */
                if (pts_us - first_pts_us
                        > (int64_t) (now - clock_origin) + 500000) break;
                if (!samples[i].ops->read_sample(
                        samples[i].opaque, &pending[i], packet,
                        MEDIA_HLS_MAXIMUM_SEGMENT_BYTES)) {
                    if (samples[i].ops->would_block(samples[i].opaque)) break;
                    fprintf(stderr, "hls read failed\n");
                    ok = false;
                    break;
                }
                have[i] = false;
                if (pending[i].kind == MEDIA_MP4_TRACK_VIDEO) {
                    video++;
                    if (pts_us > last_pts_us) last_pts_us = pts_us;
                    if (!sps_checked && pending[i].keyframe) {
                        uint8_t profile = 0;
                        MediaH264DecoderRoute route =
                            media_h264_annexb_decoder_route(
                                packet, pending[i].size, &profile);
                        MediaH264FirmwareReferences references =
                            media_h264_annexb_firmware_references(
                                packet, pending[i].size);
                        printf("hls-sps profile=%u route=%s references=%s\n",
                               profile,
                               route == MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE
                                   ? "firmware"
                               : route == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
                                   ? "software" : "unsupported",
                               references
                                   == MEDIA_H264_FIRMWARE_REFERENCES_SUPPORTED
                                   ? "ok"
                               : references
                                   == MEDIA_H264_FIRMWARE_REFERENCES_PYRAMID
                                   ? "pyramid" : "too-many");
                        sps_checked = true;
                    }
                } else if (pending[i].kind == MEDIA_MP4_TRACK_AUDIO) {
                    audio++;
                }
            }
        }
        if (starved && stall_began == 0) stall_began = now;
        if (!starved && stall_began != 0) {
            uint64_t stalled = now - stall_began;
            if (stalled >= UINT64_C(250000)) {
                stalls++;
                stall_us += stalled;
                if (stalled > longest_stall_us) longest_stall_us = stalled;
            }
            stall_began = 0;
        }
        if (now >= next_report) {
            MediaHlsStats stats = {0};
            media_hls_source_stats(sources[0], &stats);
            printf("hls-progress t=%llus video=%zu audio=%zu "
                   "media-s=%.1f refreshes=%zu/%zu skipped=%zu "
                   "buffered-us=%llu fetches=%zu\n",
                   (unsigned long long) ((now - started) / 1000000u),
                   video, audio,
                   last_pts_us == INT64_MIN ? 0.0
                       : (double) (last_pts_us - first_pts_us) / 1e6,
                   refreshes, refresh_failures, stats.skipped_live_segments,
                   (unsigned long long) stats.buffered_until_us,
                   transport.fetches);
            next_report = now + UINT64_C(5000000);
        }
        usleep(5000);
    }
    uint64_t wall_us = clock_origin == 0 ? 0
        : tilefinch_platform_monotonic_time_us() - clock_origin;
    double media_s = last_pts_us == INT64_MIN ? 0.0
        : (double) (last_pts_us - first_pts_us) / 1e6;
    printf("hls-live=%s video=%zu audio=%zu media-s=%.1f wall-s=%.1f "
           "refreshes=%zu refresh-failures=%zu stalls=%zu stall-ms=%llu "
           "longest-stall-ms=%llu\n",
           ok && video != 0 ? "PASS" : "FAIL", video, audio, media_s,
           (double) wall_us / 1e6, refreshes, refresh_failures, stalls,
           (unsigned long long) (stall_us / 1000u),
           (unsigned long long) (longest_stall_us / 1000u));
    budget_free(budget, packet);
    for (size_t i = 0; i < count; i++)
        media_hls_source_destroy(sources[i]);
    for (size_t i = 0; i < PROBE_HLS_SLOTS; i++)
        probe_hls_cancel(&transport, transport.slots[i].handle);
    return ok && video != 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3 && argc != 4 && argc != 5) {
        fprintf(
            stderr,
            "usage: %s WATCH_URL [MAXIMUM_HEIGHT "
            "[--range-check | --demux-open | --demux-full | "
            "--hls-live SECONDS | VIDEO_OUTPUT AUDIO_OUTPUT]]\n",
            argv[0]);
        return 2;
    }
    bool live_probe = argc == 5 && strcmp(argv[3], "--hls-live") == 0;
    if (argc == 4
        && strcmp(argv[3], "--range-check") != 0
        && strcmp(argv[3], "--demux-open") != 0
        && strcmp(argv[3], "--demux-full") != 0) {
        fprintf(
            stderr,
            "fourth argument must be --range-check, --demux-open, "
            "or --demux-full\n");
        return 2;
    }
    int maximum_height = argc >= 3 ? atoi(argv[2]) : 240;
    if (maximum_height <= 0 || maximum_height > 1080) {
        fprintf(stderr, "maximum height must be between 1 and 1080\n");
        return 2;
    }
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BrowserSession session = {0};
    if (!browser_session_init(&session, &budget, 1024u * 1024u)) {
        fprintf(stderr, "could not create bounded browser session\n");
        return 1;
    }
    YoutubeStream stream = {0};
    char error[256] = {0};
    /* The device's defaults: English system language, subtitles follow it. */
    YoutubeTrackPreferences preferences = {0};
    snprintf(preferences.audio_language, sizeof(preferences.audio_language),
             "%s", "en");
    snprintf(preferences.caption_language,
             sizeof(preferences.caption_language), "%s", "en");
    bool resolved = youtube_resolve_progressive_mp4_cancelable_with_preferences(
        &budget, &session, argv[1], maximum_height, 30000, &preferences,
        NULL, NULL, &stream, error, sizeof(error));
    if (!resolved) {
        fprintf(stderr, "YouTube resolution failed: %s\n", error);
        browser_session_destroy(&session);
        return 1;
    }
    printf("live-hls=%d\n", stream.live_hls ? 1 : 0);
    printf(
        "video-id=%s title=%s split=%s video-itag=%d geometry=%dx%d "
        "video-bytes=%" PRIu64 " audio-itag=%d audio-bytes=%" PRIu64
        " duration-ms=%" PRIu64 " client=%s attempts=%u "
        "watch-bytes=%zu player-bytes=%zu expires=%" PRIu64 "\n",
        stream.video_id, stream.title,
        stream.split_streams ? "yes" : "no",
        stream.itag, stream.width, stream.height, stream.content_length,
        stream.audio_itag, stream.audio_content_length, stream.duration_ms,
        stream.client_name, stream.client_attempts,
        stream.watch_bytes, stream.player_bytes, stream.expires_monotonic_s);
    printf("captions=%u selected=%d\n",
           (unsigned) stream.caption_track_count,
           (int) stream.selected_caption_track);
    for (size_t at = 0; at < stream.caption_track_count; at++) {
        const YoutubeTrack *track = &stream.caption_tracks[at];
        printf("caption[%zu] id=%s language=%s auto=%d label=%s\n", at,
               track->id, track->language, track->automatic ? 1 : 0,
               track->label);
    }
    printf("audio-tracks=%u\n", (unsigned) stream.audio_track_count);
    printf("video-url=%s\n", stream.media_url);
    if (stream.split_streams) printf("audio-url=%s\n", stream.audio_url);
    bool downloaded = true;
    if (argc == 4 && strcmp(argv[3], "--range-check") == 0) {
        MediaHttpRangeOptions options = {
            .cache_bytes = 4096,
            .timeout_ms = 15000,
            .referer = argv[1],
            .url_validator = youtube_media_url_supported
        };
        MediaHttpRange *range = media_http_range_create(
            &budget, &session, stream.media_url,
            stream.content_length, &options, error, sizeof(error));
        unsigned char prefix[4096];
        MediaRangeReader reader = media_http_range_reader(range);
        MediaHttpRangeStats stats = {0};
        downloaded = range != NULL
            && reader.read(reader.opaque, 0, prefix, sizeof(prefix))
            && media_http_range_stats(range, &stats);
        if (downloaded) {
            printf(
                "range-check=PASS status=%ld requests=%zu bytes=%zu\n",
                stats.last_http_status, stats.requests,
                stats.bytes_received);
        } else {
            fprintf(
                stderr, "range-check failed: %s\n",
                error[0] == '\0' ? "range read failed" : error);
        }
        media_http_range_destroy(range);
    }
    if (argc == 4
        && (strcmp(argv[3], "--demux-open") == 0
            || strcmp(argv[3], "--demux-full") == 0)) {
        bool traverse = strcmp(argv[3], "--demux-full") == 0;
        downloaded = probe_remote_demux(
            &budget, &session, argv[1], stream.media_url,
            stream.content_length, "video", traverse);
        if (downloaded && stream.split_streams) {
            downloaded = probe_remote_demux(
                &budget, &session, argv[1], stream.audio_url,
                stream.audio_content_length, "audio", traverse);
        }
    }
    if (live_probe) {
        if (!stream.live_hls) {
            fprintf(stderr, "stream is not a live HLS stream\n");
            downloaded = false;
        } else {
            downloaded = probe_live_hls(
                &budget, stream.media_url, (unsigned) atoi(argv[4]));
        }
    } else if (argc == 5) {
        downloaded = download(
            &budget, &session, argv[1], stream.media_url,
            stream.content_length, argv[3]);
        if (downloaded && stream.split_streams) {
            downloaded = download(
                &budget, &session, argv[1], stream.audio_url,
                stream.audio_content_length, argv[4]);
        }
    }
    browser_session_destroy(&session);
    size_t active = budget_active_allocations(&budget, NULL);
    if (budget.current != 0 || active != 0) {
        fprintf(stderr, "resolver probe leaked %zu bytes in %zu allocations\n",
                budget.current, active);
        return 1;
    }
    return downloaded ? 0 : 1;
}
