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
#include "tilefinch/fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The app variant uses the real authenticated package writer. Its explicitly
   listed, flat resource inventory is local-only; it never crawls or fetches. */
static unsigned char *read_app_file(
    Budget *budget, const char *directory, const char *name, size_t *length)
{
    *length = 0;
    if (name[0] == '\0' || strchr(name, '/') != NULL
        || strchr(name, '\\') != NULL || strcmp(name, ".") == 0
        || strcmp(name, "..") == 0) return NULL;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    struct stat info;
    if (n < 0 || (size_t) n >= sizeof(path) || lstat(path, &info) != 0
        || !S_ISREG(info.st_mode) || info.st_size <= 0
        || (uint64_t) info.st_size > OFFLINE_LIBRARY_APP_RESOURCE_LIMIT)
        return NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    size_t size = (size_t) info.st_size;
    unsigned char *bytes = budget_malloc(budget, size + 1u);
    bool okay = bytes != NULL && fread(bytes, 1, size, file) == size
        && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) okay = false;
    if (!okay) {
        budget_free(budget, bytes);
        return NULL;
    }
    *length = size;
    bytes[size] = 0;
    return bytes;
}

static const char *app_content_type(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension != NULL && strcmp(extension, ".js") == 0)
        return "text/javascript";
    if (extension != NULL && strcmp(extension, ".css") == 0)
        return "text/css";
    if (extension != NULL && strcmp(extension, ".svg") == 0)
        return "image/svg+xml";
    if (extension != NULL && strcmp(extension, ".webmanifest") == 0)
        return "application/manifest+json";
    return "application/octet-stream";
}

static TilefinchRequestContext app_request(
    const char *page, const char *target, const char *name)
{
    const char *mime = app_content_type(name);
    return (TilefinchRequestContext) {
        .target_url = target, .initiator_url = page, .top_level_url = page,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = strcmp(mime, "text/javascript") == 0
            ? TILEFINCH_DESTINATION_SCRIPT : strcmp(mime, "text/css") == 0
            ? TILEFINCH_DESTINATION_STYLE : strcmp(mime, "image/svg+xml") == 0
            ? TILEFINCH_DESTINATION_IMAGE : TILEFINCH_DESTINATION_OTHER
    };
}

static int make_app(int argc, char **argv)
{
    if (argc < 6 || argc > 38) {
        fprintf(stderr, "usage: %s --web-app|--web-app-append DIR "
                        "SOURCE_URL SOURCE_DIR RESOURCE [RESOURCE...]\n",
                argv[0]);
        return 2;
    }
    const char *directory = argv[2], *url = argv[3], *source = argv[4];
    Budget budget;
    budget_init(&budget, 24u * 1024u * 1024u);
    if (!budget_install_lexbor(&budget)) return 1;
    BrowserSession session = {0};
    PocDocument document = {0};
    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    unsigned char *html = NULL, *json = NULL;
    size_t html_length = 0, json_length = 0;
    char error[256] = {0}, manifest_url[OFFLINE_LIBRARY_URL_LIMIT];
    TilefinchWebAppManifest manifest = {0};
    uint32_t id = 0;
    /* --web-app-append adds one more app to an existing fixture library
       (for example a second Saved row); --web-app requires a new one. */
    bool append = strcmp(argv[1], "--web-app-append") == 0;
    struct stat existing;
    bool okay = (append
                     ? stat(directory, &existing) == 0
                           && S_ISDIR(existing.st_mode)
                     : mkdir(directory, 0700) == 0)
        && browser_session_init(&session, &budget, 8u * 1024u * 1024u)
        && offline_library_load(&library)
        && (append ? library.count != 0 : library.count == 0);
    if (okay) {
        html = read_app_file(&budget, source, "index.html", &html_length);
        json = read_app_file(
            &budget, source, "manifest.webmanifest", &json_length);
        okay = html != NULL && json != NULL
            && fetch_resolve_url(url, "manifest.webmanifest", manifest_url,
                                 sizeof(manifest_url))
            && tilefinch_web_app_manifest_parse(
                (const char *) json, json_length, manifest_url, url,
                &manifest, error, sizeof(error))
            && document_parse(&document, &budget, (const char *) html,
                              html_length, OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT);
    }
    for (int at = 5; okay && at < argc; at++) {
        size_t length;
        unsigned char *bytes = read_app_file(&budget, source, argv[at], &length);
        char target[OFFLINE_LIBRARY_URL_LIMIT];
        okay = bytes != NULL
            && fetch_resolve_url(url, argv[at], target, sizeof(target));
        BrowserSharedBody *body = okay
            ? browser_shared_body_take(&budget, bytes, length) : NULL;
        if (body == NULL) { budget_free(&budget, bytes); okay = false; }
        if (okay) {
            TilefinchRequestContext context = app_request(url, target, argv[at]);
            TilefinchResourceGrant grant = {
                .destination = context.destination, .mode = context.mode,
                .credentials = context.credentials,
                .corp = TILEFINCH_CORP_UNSPECIFIED,
                .final_same_origin = true, .final_same_site = true,
                .mime_validated = true
            };
            /* Generic URL cache entries deliberately do not authorize script,
               style or image consumers. Preserve their real request grants. */
            okay = browser_session_cache_put_http_shared_resource(
                &session, target, body, "", "", app_content_type(argv[at]),
                "public,max-age=31536000", "", 1u, &context, &grant);
        }
        browser_shared_body_release(body);
    }
    if (okay) okay = offline_library_save_web_app(
        &library, &document, &session, url, &manifest, NULL, 0,
        &id, error, sizeof(error));
    if (okay) {
        browser_session_destroy(&session);
        okay = browser_session_init(&session, &budget, 8u * 1024u * 1024u);
        char *restored = NULL;
        size_t restored_length = 0;
        okay = okay && offline_library_read_web_app(
            &library, &budget, &session, id, &restored, &restored_length,
            error, sizeof(error)) && restored_length != 0;
        budget_free(&budget, restored);
    }
    for (int at = 5; okay && at < argc; at++) {
        char target[OFFLINE_LIBRARY_URL_LIMIT];
        okay = fetch_resolve_url(url, argv[at], target, sizeof(target));
        if (!okay) break;
        TilefinchRequestContext context = app_request(url, target, argv[at]);
        const BrowserCacheEntry *entry = NULL;
        okay = browser_session_cache_match_resource(
            &session, target, &context, 1u, &entry) == BROWSER_CACHE_FRESH
            && entry != NULL && entry->length != 0;
    }
    if (okay) printf("offline app id=%u url=https://tilefinch.local/offline/app?id=%u\n",
                     (unsigned) id, (unsigned) id);
    else fprintf(stderr, "offline app fixture failed: %s\n", error);
    document_destroy(&document);
    browser_session_destroy(&session);
    budget_free(&budget, html);
    budget_free(&budget, json);
    (void) budget_uninstall_lexbor(&budget);
    return okay && budget.current == 0 ? 0 : 1;
}

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
    if (argc > 1 && (strcmp(argv[1], "--web-app") == 0
                     || strcmp(argv[1], "--web-app-append") == 0))
        return make_app(argc, argv);
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
