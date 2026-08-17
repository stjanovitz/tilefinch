#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_http.h"
#include "tilefinch/platform.h"
#include "tilefinch/session.h"
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

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3 && argc != 4 && argc != 5) {
        fprintf(
            stderr,
            "usage: %s WATCH_URL [MAXIMUM_HEIGHT "
            "[--range-check | --demux-open | --demux-full | "
            "VIDEO_OUTPUT AUDIO_OUTPUT]]\n",
            argv[0]);
        return 2;
    }
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
    bool resolved = youtube_resolve_progressive_mp4(
        &budget, &session, argv[1], maximum_height, 30000,
        &stream, error, sizeof(error));
    if (!resolved) {
        fprintf(stderr, "YouTube resolution failed: %s\n", error);
        browser_session_destroy(&session);
        return 1;
    }
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
        stream.watch_bytes, stream.player_bytes, stream.expires_unix);
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
    if (argc == 5) {
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
