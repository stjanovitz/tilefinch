/* The offline YouTube download's resolve step (review 1109 pass 2, NW-04
   question 4). The download is a frame pump that starts while the user
   reads a page, so its resolve must run as the worker-transport resolver
   job playback uses, one bounded step per pump, never as the synchronous
   resolver (up to 30 s of client ladder and watch page) inside one pump.

   Requests are answered by a response-keyed replay, whose media lane
   stands in for the PSP worker transport on the host. */

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/offline_download.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/platform.h"
#include "tilefinch/session.h"
#include "tilefinch/youtube_lite.h"
#include "tilefinch/youtube_resolver.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

typedef struct {
    const char *method;
    const char *url;
    const char *content_type;
    const char *body;
    size_t body_length;
} ReplayRecord;

static bool write_replay(char directory[128], const ReplayRecord *records,
                         size_t count)
{
    snprintf(directory, 128, "/tmp/tilefinch-offline-resolve-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%04zu.body", directory, i);
        FILE *body = fopen(path, "wb");
        ok = body != NULL
            && fwrite(records[i].body, 1, records[i].body_length, body)
                   == records[i].body_length;
        if (body != NULL && fclose(body) != 0) ok = false;
        snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i);
        FILE *meta = ok ? fopen(path, "wb") : NULL;
        ok = meta != NULL && fprintf(
            meta,
            "psp-http-trace=1\nmethod=%s\nurl=%s\n"
            "success=1\nasync-delay-pumps=0\nexternal-cancel=0\n"
            "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
            "effective-url=%s\ncontent-type=%s\netag=\nlast-modified=\n"
            "cf-mitigated=\naccept-ch=\ncritical-ch=\n"
            "server=fixture-youtube\ncf-ray=\nset-cookie-count=0\n",
            records[i].method, records[i].url, records[i].body_length,
            records[i].url, records[i].content_type) > 0;
        if (meta != NULL && fclose(meta) != 0) ok = false;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *trace = ok ? fopen(path, "wb") : NULL;
    ok = trace != NULL && fprintf(
        trace, "psp-http-trace-clock=1\norigin-ms=1700000000000\n"
               "capture-complete=yes\nrecord-count=%zu\n", count) > 0;
    if (trace != NULL && fclose(trace) != 0) ok = false;
    return ok;
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        (void) unlink(path);
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        char child[400];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        remove_tree(child);
    }
    (void) closedir(directory);
    (void) rmdir(path);
}

static char *read_fixture(const char *name, size_t *length)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/tests/fixtures/%s",
             TILEFINCH_TEST_SOURCE_DIR, name);
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *data = NULL;
    long end = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (end >= 0 && fseek(file, 0, SEEK_SET) == 0
        && (data = malloc((size_t) end + 1u)) != NULL
        && fread(data, 1, (size_t) end, file) == (size_t) end) {
        data[end] = '\0';
        *length = (size_t) end;
    } else {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

static size_t replay_requests(void)
{
    FetchTraceReplayStats stats = {0};
    return fetch_trace_replay_stats(&stats) ? stats.request_count : 0;
}

/* A player response the enriched client accepts: one progressive AVC/AAC
   format on a googlevideo host. */
static const char player_response[] =
    "{\"playabilityStatus\":{\"status\":\"OK\"},"
    "\"videoDetails\":{\"videoId\":\"%s\",\"title\":\"Fixture\"},"
    "\"streamingData\":{\"expiresInSeconds\":\"21540\",\"formats\":["
    "{\"itag\":18,\"width\":320,\"height\":240,\"bitrate\":200,"
    "\"contentLength\":\"2000\",\"approxDurationMs\":\"18950\","
    "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, mp4a.40.2\\\"\","
    "\"url\":\"https://rr2.googlevideo.com/videoplayback?"
    "expire=4102444800\\u0026itag=18\"}]}}";

static const char refusal[] =
    "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\","
    "\"reason\":\"Fixture refusal\"}}";

static const char watch_page[] =
    "<!doctype html><html><head><script>ytcfg.set({"
    "\"VISITOR_DATA\":\"CgtGaXh0dXJlVmlzaXQ%3D\","
    "\"INNERTUBE_API_KEY\":\"fixture-key\",\"STS\":20125});"
    "</script></head><body></body></html>";

typedef struct {
    Budget budget;
    BrowserSession session;
    OfflineLibrary library;
    OfflineDownloadManager download;
    char root[160];
    char replay[128];
    size_t replay_count;
    uint32_t id;
} Fixture;

static int fixture_open(Fixture *fixture, const char *video_id,
                        const ReplayRecord *records, size_t count)
{
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->root, sizeof(fixture->root),
             "/tmp/tilefinch-offline-resolve-%ld-%s", (long) getpid(),
             video_id);
    remove_tree(fixture->root);
    CHECK(mkdir(fixture->root, 0700) == 0);
    budget_init(&fixture->budget, 16u * MIB);
    budget_install_lexbor(&fixture->budget);
    char error[256] = {0};
    CHECK(write_replay(fixture->replay, records, count));
    fixture->replay_count = count;
    CHECK(browser_session_init(&fixture->session, &fixture->budget,
                               512u * 1024u)
          && fetch_trace_replay_begin_response_keyed(
              fixture->replay, error, sizeof(error)));
    offline_library_init(&fixture->library, &fixture->budget,
                         fixture->root);
    CHECK(offline_library_load(&fixture->library));
    char watch[128];
    snprintf(watch, sizeof(watch), "https://www.youtube.com/watch?v=%s",
             video_id);
    CHECK(offline_library_enqueue_youtube(
        &fixture->library, watch, "Fixture video", &fixture->id,
        error, sizeof(error)));
    offline_download_manager_init(&fixture->download, &fixture->budget,
                                  &fixture->session, &fixture->library);
    return 0;
}

static int fixture_close(Fixture *fixture)
{
    offline_download_manager_destroy(&fixture->download);
    fetch_trace_end();
    browser_session_destroy(&fixture->session);
    remove_tree(fixture->replay);
    remove_tree(fixture->root);
    if (fixture->budget.current != 0)
        fprintf(stderr, "budget still holds %zu bytes\n",
                (size_t) fixture->budget.current);
    CHECK(fixture->budget.current == 0
          && fixture->budget.external_reserved == 0);
    return 0;
}

/* Pumps the RESOLVE phase to its end, recording the most replay requests
   any one pump made. */
static size_t pump_resolve(OfflineDownloadManager *download,
                           size_t *most_requests_per_pump)
{
    size_t pumps = 0;
    *most_requests_per_pump = 0;
    while (pumps < 4096 && download->active_id != 0
           && download->phase == OFFLINE_DOWNLOAD_RESOLVE) {
        size_t before = replay_requests();
        (void) offline_download_manager_pump(download);
        size_t made = replay_requests() - before;
        if (made > *most_requests_per_pump) *most_requests_per_pump = made;
        pumps++;
    }
    return pumps;
}

/* The full ladder: three refused clients, the watch page, the enriched
   request. Each of those is a request on the worker, and a frame pump
   starts at most one of them; the synchronous resolver made all five
   inside the first pump. */
static int test_resolve_spans_pumps(void)
{
    char player[sizeof(player_response) + 16];
    int player_length = snprintf(player, sizeof(player), player_response,
                                 "TFTEST00002");
    ReplayRecord records[] = {
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
         "application/json", refusal, sizeof(refusal) - 1u},
        {"GET",
         "https://www.youtube.com/watch?v=TFTEST00002"
         "&bpctr=9999999999&has_verified=1",
         "text/html; charset=utf-8", watch_page, sizeof(watch_page) - 1u},
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
         "&prettyPrint=false",
         "application/json", player, (size_t) player_length}
    };
    Fixture fixture;
    if (fixture_open(&fixture, "TFTEST00002", records, 3) != 0) return 1;
    OfflineDownloadManager *download = &fixture.download;
    CHECK(offline_download_manager_start(download, fixture.id)
          && download->phase == OFFLINE_DOWNLOAD_RESOLVE);
    size_t most = 0;
    size_t pumps = pump_resolve(download, &most);
    size_t requests = replay_requests();
    const OfflineLibraryItem *item =
        offline_library_find(&fixture.library, fixture.id);
    if (most > 1u || pumps < 10u)
        fprintf(stderr, "resolve: %zu pumps, %zu requests, at most %zu in"
                " one pump, phase %d, error \"%s\"\n", pumps, requests,
                most, (int) download->phase,
                item == NULL ? "" : item->failure_reason);
    /* Five requests, never two in one pump, each over several pumps
       (start, poll, parse). */
    CHECK(requests == 5u && most == 1u && pumps >= 10u);
    /* The resolved stream reached the transfer unchanged. */
    CHECK(download->active_id == fixture.id
          && download->phase == OFFLINE_DOWNLOAD_VIDEO
          && download->stream.itag == 18
          && download->stream.content_length == 2000u
          && download->stream.client_attempts == 4u
          && item != NULL && item->state == OFFLINE_ITEM_DOWNLOADING
          && item->content_bytes == 2000u);
    CHECK(offline_download_manager_pause(download, fixture.id)
          && !offline_download_manager_active(download, NULL)
          && item->state == OFFLINE_ITEM_PAUSED);
    return fixture_close(&fixture);
}

/* YT-01: the provider's cached API identity also serves the download, so
   its resolve is the one enriched request. */
static int test_resolve_uses_cached_identity(void)
{
    size_t page_length = 0, next_length = 0;
    char *page = read_fixture("http-site-adapter-youtube/0000.body",
                              &page_length);
    char *next = read_fixture("http-site-adapter-youtube/0001.body",
                              &next_length);
    CHECK(page != NULL && next != NULL);
    char player[sizeof(player_response) + 16];
    int player_length = snprintf(player, sizeof(player), player_response,
                                 "TFTEST00001");
    ReplayRecord records[] = {
        {"GET",
         "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999"
         "&has_verified=1",
         "text/html; charset=utf-8", page, page_length},
        {"POST",
         "https://m.youtube.com/youtubei/v1/next?key=fixture-key"
         "&prettyPrint=false",
         "application/json", next, next_length},
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
         "&prettyPrint=false",
         "application/json", player, (size_t) player_length}
    };
    Fixture fixture;
    if (fixture_open(&fixture, "TFTEST00001", records, 3) != 0) return 1;
    char error[256] = {0};
    YoutubeLiteDocument document = {0};
    YoutubeLiteResolverIdentity identity = {0};
    CHECK(youtube_lite_load(
              &fixture.budget, &fixture.session,
              "https://www.youtube.com/watch?v=TFTEST00001"
              "&tilefinch_view=comments",
              1u * MIB, 1000, &document, error, sizeof(error))
          && youtube_lite_resolver_identity_get(&fixture.session,
                                                &identity));
    youtube_lite_document_destroy(&document);
    OfflineDownloadManager *download = &fixture.download;
    CHECK(offline_download_manager_start(download, fixture.id));
    size_t before = replay_requests(), most = 0;
    size_t pumps = pump_resolve(download, &most);
    size_t made = replay_requests() - before;
    const OfflineLibraryItem *item =
        offline_library_find(&fixture.library, fixture.id);
    if (made != 1u || download->phase != OFFLINE_DOWNLOAD_VIDEO)
        fprintf(stderr, "cached resolve: %zu requests over %zu pumps,"
                " phase %d, error \"%s\"\n", made, pumps,
                (int) download->phase,
                item == NULL ? "" : item->failure_reason);
    CHECK(made == 1u && pumps >= 3u
          && download->phase == OFFLINE_DOWNLOAD_VIDEO
          && download->stream.itag == 18
          && download->stream.client_attempts == 1u);
    free(page);
    free(next);
    return fixture_close(&fixture);
}

/* Pause in the middle of the resolve (the request is on the worker):
   the job and its buffers go back, and nothing more is requested. */
static int test_pause_during_resolve(void)
{
    char player[sizeof(player_response) + 16];
    int player_length = snprintf(player, sizeof(player), player_response,
                                 "TFTEST00003");
    ReplayRecord records[] = {
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
         "application/json", refusal, sizeof(refusal) - 1u},
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
         "&prettyPrint=false",
         "application/json", player, (size_t) player_length}
    };
    Fixture fixture;
    if (fixture_open(&fixture, "TFTEST00003", records, 2) != 0) return 1;
    OfflineDownloadManager *download = &fixture.download;
    CHECK(offline_download_manager_start(download, fixture.id));
    (void) offline_download_manager_pump(download);
    CHECK(download->phase == OFFLINE_DOWNLOAD_RESOLVE
          && replay_requests() == 1u);
    CHECK(offline_download_manager_pause(download, fixture.id)
          && !offline_download_manager_active(download, NULL));
    const OfflineLibraryItem *item =
        offline_library_find(&fixture.library, fixture.id);
    CHECK(item != NULL && item->state == OFFLINE_ITEM_PAUSED
          && download->phase == OFFLINE_DOWNLOAD_IDLE);
    for (unsigned pump = 0; pump < 8u; pump++)
        CHECK(!offline_download_manager_pump(download));
    CHECK(replay_requests() == 1u);
    /* A retry resolves again from the start; tearing the manager down in
       the middle of it returns the job too (fixture_close checks the
       Budget). */
    CHECK(offline_download_manager_start(download, fixture.id)
          && download->phase == OFFLINE_DOWNLOAD_RESOLVE);
    (void) offline_download_manager_pump(download);
    CHECK(download->phase == OFFLINE_DOWNLOAD_RESOLVE
          && replay_requests() == 2u);
    return fixture_close(&fixture);
}

typedef struct {
    unsigned polls;
    unsigned cancel_after;
} CancelProbe;

static bool cancel_after_polls(void *opaque)
{
    CancelProbe *probe = opaque;
    return ++probe->polls > probe->cancel_after;
}

/* O during the resolve: the frame pump's cancel callback is polled by the
   job, which stops with the resolver's cancel message, and the PSP loop
   then pauses the item. */
static int test_cancel_during_resolve(void)
{
    char player[sizeof(player_response) + 16];
    int player_length = snprintf(player, sizeof(player), player_response,
                                 "TFTEST00004");
    ReplayRecord records[] = {
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
         "application/json", refusal, sizeof(refusal) - 1u},
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
         "&prettyPrint=false",
         "application/json", player, (size_t) player_length}
    };
    Fixture fixture;
    if (fixture_open(&fixture, "TFTEST00004", records, 2) != 0) return 1;
    OfflineDownloadManager *download = &fixture.download;
    CancelProbe probe = {.cancel_after = 2u};
    offline_download_manager_set_cancel(download, cancel_after_polls,
                                        &probe);
    CHECK(offline_download_manager_start(download, fixture.id));
    size_t most = 0;
    size_t pumps = pump_resolve(download, &most);
    const OfflineLibraryItem *item =
        offline_library_find(&fixture.library, fixture.id);
    CHECK(item != NULL);
    if (strstr(item->failure_reason, "cancelled") == NULL)
        fprintf(stderr, "cancel: %zu pumps, state %d, error \"%s\"\n",
                pumps, (int) item->state, item->failure_reason);
    CHECK(!offline_download_manager_active(download, NULL)
          && item->state == OFFLINE_ITEM_FAILED
          && strcmp(item->failure_reason,
                    "YouTube resolution cancelled") == 0
          && strcmp(download->error, "YouTube resolution cancelled") == 0
          && pumps == 3u && replay_requests() <= 1u);
    /* psp_loop_pump_offline_download pauses a cancelled item. */
    CHECK(offline_download_manager_pause(download, fixture.id)
          && item->state == OFFLINE_ITEM_PAUSED);
    return fixture_close(&fixture);
}

/* A failed resolve keeps the resolver's message as the item's failure
   reason, as before. */
static int test_failed_resolve_message(void)
{
    ReplayRecord records[] = {
        {"POST",
         "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
         "application/json", refusal, sizeof(refusal) - 1u},
        {"GET",
         "https://www.youtube.com/watch?v=TFTEST00005"
         "&bpctr=9999999999&has_verified=1",
         "text/html; charset=utf-8", "<html></html>", 13u}
    };
    Fixture fixture;
    if (fixture_open(&fixture, "TFTEST00005", records, 2) != 0) return 1;
    OfflineDownloadManager *download = &fixture.download;
    CHECK(offline_download_manager_start(download, fixture.id));
    size_t most = 0;
    (void) pump_resolve(download, &most);
    const OfflineLibraryItem *item =
        offline_library_find(&fixture.library, fixture.id);
    CHECK(item != NULL);
    if (strstr(item->failure_reason,
               "watch: page omitted bounded player configuration"
               " after 3 client attempts") == NULL)
        fprintf(stderr, "failed resolve: state %d, error \"%s\"\n",
                (int) item->state, item->failure_reason);
    CHECK(!offline_download_manager_active(download, NULL)
          && item->state == OFFLINE_ITEM_FAILED && most <= 1u
          && strstr(item->failure_reason,
                    "watch: page omitted bounded player configuration"
                    " after 3 client attempts") != NULL);
    return fixture_close(&fixture);
}

int main(void)
{
    if (test_resolve_spans_pumps() != 0
        || test_resolve_uses_cached_identity() != 0
        || test_pause_during_resolve() != 0
        || test_cancel_during_resolve() != 0
        || test_failed_resolve_message() != 0)
        return 1;
    printf("offline download resolve tests passed\n");
    return 0;
}
