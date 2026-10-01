/*
 * Build a one-item offline library that plays like a downloaded YouTube
 * video, from a local split video/audio pair.
 *
 *   tilefinch-offline-library-fixture DIR VIDEO.mp4 AUDIO.mp4
 *       WIDTH HEIGHT DURATION_MS TITLE
 *
 * DIR becomes the "offline" directory beside the EBOOT (library.bin plus the
 * item's media files). The item is stored as ready, so
 * https://tilefinch.local/offline lists it and its Play link opens the
 * native player with no network at all. Used by
 * tools/make-offline-youtube-fixture.sh; see docs/engineering/PERF_JOURNEYS.md.
 */
#include "tilefinch/offline_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static long file_size(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (long) info.st_size : -1;
}

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (in == NULL) return 1;
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return 1;
    }
    char buffer[65536];
    size_t n;
    int failed = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            failed = 1;
            break;
        }
    }
    if (ferror(in)) failed = 1;
    fclose(in);
    if (fclose(out) != 0) failed = 1;
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s DIR VIDEO.mp4 AUDIO.mp4 WIDTH HEIGHT "
                "DURATION_MS TITLE\n", argv[0]);
        return 2;
    }
    const char *directory = argv[1];
    long video_bytes = file_size(argv[2]);
    long audio_bytes = file_size(argv[3]);
    int width = atoi(argv[4]);
    int height = atoi(argv[5]);
    long duration_ms = atol(argv[6]);
    const char *title = argv[7];
    if (video_bytes <= 0 || audio_bytes <= 0 || width <= 0 || height <= 0
        || duration_ms <= 0) {
        fprintf(stderr, "missing media or invalid geometry/duration\n");
        return 2;
    }
    if (mkdir(directory, 0700) != 0) {
        struct stat info;
        if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) {
            fprintf(stderr, "cannot create %s\n", directory);
            return 1;
        }
    }
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    (void) offline_library_load(&library);
    if (library.count != 0) {
        fprintf(stderr, "%s already holds a library; use an empty directory\n",
                directory);
        return 1;
    }
    char error[256] = {0};
    uint32_t id = 0;
    /* A syntactically valid watch URL whose id cannot collide with a real
       video; the offline player never resolves it. */
    if (!offline_library_enqueue_youtube(
            &library, "https://www.youtube.com/watch?v=TFOFFLINE01",
            title, &id, error, sizeof(error))) {
        fprintf(stderr, "enqueue: %s\n", error);
        return 1;
    }
    YoutubeStream stream = {
        .content_length = (size_t) video_bytes,
        .audio_content_length = (size_t) audio_bytes,
        .duration_ms = (uint64_t) duration_ms,
        .width = width,
        .height = height,
        /* The YouTube itags this shape corresponds to: 134 is 360p AVC
           video, 140 is 128 kbit/s AAC audio. */
        .itag = height > 240 ? 134 : 133,
        .audio_itag = 140,
        .split_streams = true
    };
    snprintf(stream.title, sizeof(stream.title), "%s", title);
    if (!offline_library_apply_youtube_stream(&library, id, &stream)) {
        fprintf(stderr, "could not record the stream\n");
        return 1;
    }
    OfflineLibraryItem *item = offline_library_find_mutable(&library, id);
    if (item == NULL) return 1;
    item->state = OFFLINE_ITEM_READY;
    item->downloaded_bytes = (uint64_t) (video_bytes + audio_bytes);
    if (!offline_library_save(&library)) {
        fprintf(stderr, "could not save the library\n");
        return 1;
    }
    char video_path[512], audio_path[512];
    if (!offline_library_item_path(&library, id, ".video.mp4", video_path,
                                   sizeof(video_path))
        || !offline_library_item_path(&library, id, ".audio.mp4",
                                      audio_path, sizeof(audio_path))
        || copy_file(argv[2], video_path) != 0
        || copy_file(argv[3], audio_path) != 0) {
        fprintf(stderr, "could not copy the media into %s\n", directory);
        return 1;
    }
    printf("offline item id=%u url=https://tilefinch.local/offline/video?id=%u\n",
           (unsigned) id, (unsigned) id);
    return 0;
}
