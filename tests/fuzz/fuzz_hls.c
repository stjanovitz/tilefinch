/* libFuzzer target: HLS playlist parsing (one-shot and incremental capture)
   and variant/rendition selection. The whole input is the playlist body. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/url.h"

#define MIB (1024u * 1024u)

static void exercise_playlist(MediaHlsPlaylist *playlist)
{
    if (playlist == NULL) return;
    (void) media_hls_playlist_kind(playlist);
    (void) media_hls_playlist_segment_count(playlist);
    (void) media_hls_playlist_duration_us(playlist);
    (void) media_hls_playlist_is_live(playlist);
    char video[TILEFINCH_URL_SERIALIZED_LIMIT];
    char audio[TILEFINCH_URL_SERIALIZED_LIMIT];
    char tiny[16];
    (void) media_hls_playlist_select_variant(playlist, 480, 272, 272,
                                             video, sizeof(video));
    (void) media_hls_playlist_select_variant(playlist, 0, 0, 0,
                                             tiny, sizeof(tiny));
    (void) media_hls_playlist_select_streams(playlist, 480, 272, 272,
                                             video, sizeof(video),
                                             audio, sizeof(audio));
    (void) media_hls_playlist_select_streams(playlist, 1920, 1080, 720,
                                             tiny, sizeof(tiny),
                                             tiny, sizeof(tiny));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 256 * 1024) return 0;
    unsigned char *bytes = malloc(size == 0 ? 1 : size);
    if (bytes == NULL) return 0;
    if (size != 0) memcpy(bytes, data, size);
    static const char url[] = "https://media.test/live/path/index.m3u8?t=1";

    Budget budget;
    budget_init(&budget, 24u * MIB);
    char error[256];
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, url, bytes, size, error, sizeof(error));
    exercise_playlist(playlist);
    media_hls_playlist_destroy(playlist);

    /* Incremental capture with chunk sizes derived from the input. */
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &budget, url, error, sizeof(error));
    if (stream != NULL) {
        size_t chunk = size == 0 ? 1 : 1 + bytes[0] % 97;
        /* An odd first byte replays the body until it exceeds the 64 KiB
           retention bound, reaching the live-window compaction path. */
        size_t repeats = size != 0 && (bytes[0] & 1) != 0
            ? 1 + (MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES + 4096) / size : 1;
        bool ok = true;
        for (size_t round = 0; ok && round < repeats; round++) {
            for (size_t offset = 0; ok && offset < size; offset += chunk) {
                size_t length = size - offset < chunk ? size - offset : chunk;
                ok = media_hls_playlist_stream_feed(
                    stream, bytes + offset, length, error, sizeof(error));
            }
        }
        (void) media_hls_playlist_stream_bytes_seen(stream);
        (void) media_hls_playlist_stream_was_compacted(stream);
        if (ok) {
            playlist = media_hls_playlist_stream_finish(stream, error,
                                                        sizeof(error));
            exercise_playlist(playlist);
            media_hls_playlist_destroy(playlist);
        }
        media_hls_playlist_stream_destroy(stream);
    }
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_hls: budget leak of %zu bytes\n",
                budget.current);
        abort();
    }
    free(bytes);
    return 0;
}
