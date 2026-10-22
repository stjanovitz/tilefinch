#include "tilefinch/file_download.h"
#include "tilefinch/psp_offline_store.h"
#include "tilefinch/sha256.h"
#include "tilefinch/platform.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return false; } } while (0)
#define URL "https://downloads.example.test/releases/homebrew.zip"
static const char *trace_url = URL;

/* Distinguishes two entities served at the same URL. */
static unsigned entity_salt;
static unsigned char byte_at(size_t index) { return (unsigned char) (index * 37u + 11u + entity_salt); }

/* Synthetic response corpus, never an account or a live download. */
static bool trace(const char *directory, size_t total, size_t offset,
                  unsigned status, const char *etag, const char *extra,
                  bool content_length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *file = fopen(path, "wb"); CHECK(file != NULL);
    CHECK(fputs("psp-http-trace-clock=1\norigin-ms=1700000000000\ncapture-complete=yes\nrecord-count=1\n", file) >= 0
        && fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    file = fopen(path, "wb"); CHECK(file != NULL);
    for (size_t i = offset; i < total; i++) CHECK(fputc(byte_at(i), file) != EOF);
    CHECK(fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    file = fopen(path, "wb"); CHECK(file != NULL);
    const char *content_type = extra != NULL && !strncmp(extra, "content-type: ", 14)
        ? extra + 14 : "application/zip";
    fprintf(file, "psp-http-trace=3\nmethod=GET\nurl=%s\nsuccess=1\n"
        "async-delay-pumps=0\nexternal-cancel=0\ntransport-timeout=0\nerror=\n"
        "request-body-length=0\nrequest-body-hash=cbf29ce484222325\n"
        "request-cookie-bytes=0\nrequest-extra-header-bytes=0\n"
        "request-send-client-hints=0\nrequest-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
        "status=%u\nlength=%zu\neffective-url=%s\n"
        "content-type=%s\netag=%s\nlast-modified=\n"
        "response-header-count=%u\nset-cookie-count=0\n"
        "response-header-0=content-type: application/zip\n",
        trace_url, status, total - offset, trace_url, content_type, etag,
        1u + (content_length ? 1u : 0u) + (extra != NULL ? 1u : 0u));
    unsigned header = 1;
    if (content_length) fprintf(file, "response-header-%u=content-length: %zu\n", header++, total - offset);
    if (extra != NULL) fprintf(file, "response-header-%u=%s\n", header, extra);
    CHECK(fclose(file) == 0);
    char error[256] = {0};
    bool begun = fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error));
    if (!begun) fprintf(stderr, "Synthetic replay setup: %s\n", error);
    CHECK(begun);
    return true;
}

static bool complete(OfflineDownloadManager *manager)
{
    for (unsigned i = 0; i < 1024 && offline_download_manager_active(manager, NULL); i++)
        (void) offline_download_manager_pump(manager);
    CHECK(!offline_download_manager_active(manager, NULL));
    fetch_trace_end();
    return true;
}

static bool exact_file(const OfflineLibrary *library, uint32_t id, size_t length)
{
    char path[512]; CHECK(file_download_path(library, id, path, sizeof(path)));
    FILE *file = fopen(path, "rb"); CHECK(file != NULL);
    for (size_t i = 0; i < length; i++) CHECK(fgetc(file) == byte_at(i));
    CHECK(fgetc(file) == EOF && !ferror(file) && fclose(file) == 0);
    return true;
}

static bool filenames(void)
{
    FetchResult response = {.status_code = 200};
    snprintf(response.content_type, sizeof(response.content_type), "Application/ZIP; charset=binary");
    CHECK(file_download_response_candidate(&response));
    snprintf(response.content_type, sizeof(response.content_type), "text/html");
    CHECK(!file_download_response_candidate(&response));
    snprintf(response.content_type, sizeof(response.content_type), "application/json");
    CHECK(!file_download_response_candidate(&response));
    snprintf(response.content_type, sizeof(response.content_type), "application/octet-stream");
    response.status_code = 403;
    CHECK(!file_download_response_candidate(&response));
    static const struct { const char *header, *expected; } cases[] = {
        {NULL, "homebrew.zip"},
        {"attachment; filename=game.pbp", "game.pbp"},
        {"attachment; filename=\"folder/../game.zip\"", "game.zip"},
        {"attachment; filename=\"a;b.zip\"", "a;b.zip"},
        {"attachment; filename=x.zip; filename*=UTF-8''new%20name.zip", "new name.zip"},
        {"attachment; filename=good.zip; filename*=UTF-8''bad%00.zip", "good.zip"},
        {"attachment; filename=CON.zip", "_ON.zip"},
        {"attachment; filename=\"../..\"", "download.bin"},
        {"attachment; filename=\"a<b>c.zip\"", "a_b_c.zip"},
        {"attachment; filename=\"bad\r\nname\"", "homebrew.zip"}
    };
    char name[FILE_DOWNLOAD_FILENAME_LIMIT];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(file_download_filename(URL "?token=not-a-filename", cases[i].header, name));
        CHECK(!strcmp(name, cases[i].expected) && file_download_filename_valid(name));
    }
    CHECK(!file_download_url_supported("http://downloads.example.test/game.zip"));
    CHECK(!file_download_url_supported("file:///game.zip"));
    CHECK(!file_download_url_supported("https://user:password@downloads.example.test/game.zip"));
    CHECK(!file_download_filename_valid("../game.zip"));
    CHECK(!file_download_filename_valid("CON"));
    CHECK(file_download_url_candidate(URL));
    CHECK(!file_download_url_candidate("https://downloads.example.test/release"));
    CHECK(file_download_filename("https://downloads.example.test/a%2fb.zip", NULL, name)
        && !strcmp(name, "b.zip"));
    return true;
}

static bool transfers(const char *directory, const char *corpus)
{
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session; CHECK(browser_session_init(&session, &budget, 65536));
    OfflineLibrary library; offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    OfflineDownloadManager manager;
    offline_download_manager_init(&manager, &budget, &session, &library);
    char error[128] = {0}; uint32_t id = 0;
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 512u * 1024u, 0, 200, "\"one\"",
        "content-disposition: attachment; filename=package.zip", true));
    bool started = offline_download_manager_start(&manager, id);
    if (!started) fprintf(stderr, "Start failed: %s / %s; Budget=%zu/%zu\n", manager.error,
        offline_library_find(&library, id)->failure_reason, budget.current, budget.limit);
    CHECK(started);
    CHECK(complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(manager.file == NULL); /* Idle downloads retain no second transport lane. */
    CHECK(!strcmp(offline_library_find(&library, id)->title, "package.zip"));
    CHECK(exact_file(&library, id, 512u * 1024u));
    CHECK(budget.peak <= budget.limit); /* Replay also retains its fixture body. */
    char recovered[512], part[512];
    CHECK(file_download_path(&library, id, recovered, sizeof(recovered)));
    CHECK(offline_library_item_path(&library, id, ".file.part", part, sizeof(part)));
    CHECK(rename(recovered, part) == 0);
    library.loaded = false;
    CHECK(offline_library_load(&library));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 512u * 1024u)); /* Power loss before publication. */
    CHECK(offline_library_remove(&library, id));

    /* Pause, reload the index, then resume verified bytes with If-Range. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    OfflineDownloadSnapshot snapshot = {0};
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes != 0) break;
    }
    CHECK(snapshot.active && snapshot.downloaded_bytes > 0 && snapshot.downloaded_bytes < 262144);
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    size_t offset = (size_t) offline_library_find(&library, id)->downloaded_bytes;
    offline_download_manager_destroy(&manager);
    library.loaded = false; CHECK(offline_library_load(&library));
    offline_download_manager_init(&manager, &budget, &session, &library);
    char range[128]; snprintf(range, sizeof(range), "content-range: bytes %zu-262143/262144", offset);
    CHECK(trace(corpus, 262144, offset, 206, "\"one\"", range, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 262144));
    CHECK(offline_library_remove(&library, id));

    /* A server that ignores Range starts fresh; it must not double the file. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes) break;
    }
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    CHECK(trace(corpus, 12345, 0, 200, "\"two\"", NULL, false));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 12345));
    CHECK(offline_library_remove(&library, id));

    /* A changed validator must not authorize appending even a valid range. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes) break;
    }
    CHECK(snapshot.active && snapshot.downloaded_bytes > 0);
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    offset = (size_t) offline_library_find(&library, id)->downloaded_bytes;
    snprintf(range, sizeof(range), "content-range: bytes %zu-262143/262144", offset);
    CHECK(trace(corpus, 262144, offset, 206, "\"two\"", range, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
    CHECK(offline_library_find(&library, id)->downloaded_bytes == offset);
    CHECK(trace(corpus, 42, 0, 200, "\"two\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 42) && offline_library_remove(&library, id));

    /* Every byte reached the partial file but the item was never published
       (power loss before the READY save). The resume is answered 416 with
       the same validator and exactly the partial length: finish it. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes) break;
    }
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    CHECK(offline_library_item_path(&library, id, ".file.part", part, sizeof(part)));
    {
        struct stat status; CHECK(stat(part, &status) == 0 && status.st_size > 0);
        FILE *rest = fopen(part, "ab"); CHECK(rest != NULL);
        for (size_t i = (size_t) status.st_size; i < 262144u; i++) CHECK(fputc(byte_at(i), rest) != EOF);
        CHECK(fclose(rest) == 0);
    }
    CHECK(trace(corpus, 262144, 262144, 416, "\"one\"", "content-range: bytes */262144", true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(offline_library_find(&library, id)->content_bytes == 262144u);
    CHECK(exact_file(&library, id, 262144) && access(part, F_OK) != 0);
    CHECK(offline_library_remove(&library, id));

    /* A 416 naming another length (or validator) restarts from zero. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes) break;
    }
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    CHECK(trace(corpus, 17, 17, 416, "\"one\"", "content-range: bytes */17", true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
    CHECK(trace(corpus, 42, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 42) && offline_library_remove(&library, id));

    /* The entity changed (200 with a new validator) but the partial file of
       the old one could not be reopened. The new validator must not be
       persisted over the old bytes, or a later resume splices the two. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 20; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
        if (snapshot.downloaded_bytes) break;
    }
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    CHECK(offline_library_item_path(&library, id, ".file.part", part, sizeof(part)));
    struct stat partial; CHECK(stat(part, &partial) == 0 && partial.st_size > 0);
    offset = (size_t) partial.st_size;
    CHECK(chmod(part, 0444) == 0);
    entity_salt = 1;
    CHECK(trace(corpus, 262144, 0, 200, "\"two\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
    /* The open failure is named, not reported as an interrupted transfer. */
    CHECK(!strcmp(offline_library_find(&library, id)->failure_reason,
                  "Download file could not be opened on the Memory Stick"));
    CHECK(chmod(part, 0644) == 0);
    snprintf(range, sizeof(range), "content-range: bytes %zu-262143/262144", offset);
    CHECK(trace(corpus, 262144, offset, 206, "\"two\"", range, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
    CHECK(trace(corpus, 262144, 0, 200, "\"two\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 262144) && offline_library_remove(&library, id));
    entity_salt = 0;

    /* Reject an unsolicited/incorrect partial response, HTML, and HTTP errors. */
    static const struct { unsigned status; const char *extra; } bad[] = {
        {206, "content-range: bytes 1-16/17"},
        {200, "content-encoding: gzip"},
        {200, "content-type: text/html; charset=utf-8"},
        {403, NULL},
        {200, "content-length: 99"},
        {200, "content-length: 536870913"}
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
        CHECK(trace(corpus, 17, 0, bad[i].status, "\"one\"", bad[i].extra, false));
        CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
        CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
        char path[512]; CHECK(file_download_path(&library, id, path, sizeof(path)));
        CHECK(access(path, F_OK) != 0);
        CHECK(offline_library_remove(&library, id));
    }

    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 0, 0, 200, "", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&library, id, 0) && offline_library_remove(&library, id));

    /* A final destination is never overwritten. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    char path[512]; CHECK(file_download_path(&library, id, path, sizeof(path)));
    FILE *file = fopen(path, "wb"); CHECK(file != NULL);
    CHECK(fputs("keep", file) >= 0 && fclose(file) == 0);
    CHECK(trace(corpus, 17, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    file = fopen(path, "rb"); CHECK(file != NULL);
    char keep[5] = {0}; CHECK(fread(keep, 1, 4, file) == 4 && !strcmp(keep, "keep") && fclose(file) == 0);
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_PAUSED);
    CHECK(offline_library_remove(&library, id));
    CHECK(access(path, F_OK) == 0 && unlink(path) == 0);

    /* An external library deletion cannot leave a dangling asynchronous job. */
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    CHECK(offline_library_remove(&library, id));
    (void) offline_download_manager_pump(&manager);
    CHECK(!offline_download_manager_active(&manager, NULL));
    fetch_trace_end();

    /* Every early allocation refusal leaves an idle, destructible owner. */
    offline_download_manager_destroy(&manager);
    for (size_t failure = 0; failure < 16; failure++) {
        CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
        CHECK(trace(corpus, 17, 0, 200, "\"one\"", NULL, true));
        offline_download_manager_init(&manager, &budget, &session, &library);
        size_t before = budget.current;
        budget_inject_failure_after(&budget, failure);
        (void) offline_download_manager_start(&manager, id);
        budget_clear_failure_injection(&budget);
        offline_download_manager_destroy(&manager); fetch_trace_end();
        CHECK(budget.current == before);
        CHECK(offline_library_remove(&library, id));
    }
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

/* A clock the test moves on by hand, so a rate sample is due every pump. */
static uint64_t clock_skew_us;
static uint64_t skewed_clock_us(void *context)
{
    (void) context;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * UINT64_C(1000000)
        + (uint64_t) now.tv_nsec / 1000u + clock_skew_us;
}

/* A resume answered with a fresh 200 restarts the byte count from zero. The
   transfer rate restarts with it: a sample against the abandoned partial
   length must not wrap into a 4 GiB/s reading. */
static bool restart_speed(const char *directory, const char *corpus)
{
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session; CHECK(browser_session_init(&session, &budget, 65536));
    OfflineLibrary library; offline_library_init(&library, &budget, directory);
    CHECK(offline_library_load(&library));
    OfflineDownloadManager manager;
    offline_download_manager_init(&manager, &budget, &session, &library);
    char error[128] = {0}; uint32_t id = 0;
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 262144, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&manager, id));
    OfflineDownloadSnapshot snapshot = {0};
    /* Keep more than one restarted pump's worth of bytes. */
    for (unsigned i = 0; i < 40 && snapshot.downloaded_bytes < 131072; i++) {
        (void) offline_download_manager_pump(&manager);
        CHECK(offline_download_manager_snapshot(&manager, id, &snapshot));
    }
    CHECK(snapshot.downloaded_bytes >= 131072 && snapshot.downloaded_bytes < 262144);
    CHECK(offline_download_manager_pause(&manager, id)); fetch_trace_end();
    TilefinchPlatformServices services = {.monotonic_time_us = skewed_clock_us};
    tilefinch_platform_set_services(&services);
    CHECK(trace(corpus, 262144, 0, 200, "\"two\"", NULL, true));
    bool started = offline_download_manager_start(&manager, id);
    uint32_t fastest = 0;
    for (unsigned i = 0; started && i < 64 && snapshot.downloaded_bytes < 262144; i++) {
        clock_skew_us += 300000u;
        (void) offline_download_manager_pump(&manager);
        if (!offline_download_manager_snapshot(&manager, id, &snapshot)
            || !snapshot.active) break;
        if (snapshot.bytes_per_second > fastest) fastest = snapshot.bytes_per_second;
    }
    tilefinch_platform_set_services(NULL);
    CHECK(started && complete(&manager));
    CHECK(offline_library_find(&library, id)->state == OFFLINE_ITEM_READY);
    if (fastest == 0 || fastest > 262144u * 4u)
        fprintf(stderr, "Restarted download rate: %u bytes/s\n", (unsigned) fastest);
    /* At most 262,144 bytes arrive in 300 ms steps. */
    CHECK(fastest != 0 && fastest <= 262144u * 4u);
    CHECK(exact_file(&library, id, 262144) && offline_library_remove(&library, id));
    offline_download_manager_destroy(&manager); browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

static bool confirmation(const char *directory, const char *corpus)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] = "<!doctype html><title>Release</title><a href='" URL "'>Download</a>";
    CHECK(browser_engine_commit_html(engine, "https://downloads.example.test/", page, sizeof(page) - 1, true));
    BrowserController *controller = browser_engine_controller(engine);
    CHECK(controller_focus_next(controller));
    char url[2048]; CHECK(controller_focused_download_url(controller, url, sizeof(url)) && !strcmp(url, URL));
    ControllerAction action; CHECK(controller_activate(controller, &action) && action.prefer_download);
    PspOfflineStore store; psp_offline_store_init(&store, browser_engine_budget(engine), browser_engine_session(engine), directory);
    CHECK(psp_offline_store_handle_url(&store, engine, NULL,
        "https://tilefinch.local/offline/confirm-file", NULL, true) == PSP_OFFLINE_ROUTE_ERROR);
    CHECK(psp_offline_store_offer_file(&store, engine, URL));
    CHECK(store.library.count == 0 && !offline_download_manager_active(&store.download, NULL));
    CHECK(psp_offline_store_file_preview(&store) != NULL);
    psp_offline_store_discard_file(&store);
    CHECK(psp_offline_store_file_preview(&store) == NULL && store.library.count == 0);
    CHECK(!strcmp(navigation_current(browser_engine_navigation(engine))->url, "https://downloads.example.test/"));
    CHECK(psp_offline_store_offer_file(&store, engine, URL));
    CHECK(trace(corpus, 42, 0, 200, "\"one\"", NULL, true));
    CHECK(psp_offline_store_accept_file(&store) && store.library.count == 1);
    CHECK(!psp_offline_store_accept_file(&store)); /* Confirmation is consumed. */
    for (unsigned i = 0; i < 100 && offline_download_manager_active(&store.download, NULL); i++)
        (void) psp_offline_store_pump(&store);
    CHECK(store.library.items[0].state == OFFLINE_ITEM_READY);
    CHECK(exact_file(&store.library, store.library.items[0].id, 42));
    CHECK(!strcmp(navigation_current(browser_engine_navigation(engine))->url, "https://downloads.example.test/"));
    CHECK(browser_engine_controller(engine)->focus_kind == CONTROLLER_FOCUS_LINK);
    fetch_trace_end();
    CHECK(offline_library_remove(&store.library, store.library.items[0].id));
    psp_offline_store_destroy(&store); browser_engine_destroy(engine);
    return true;
}

static bool extensionless_navigation(const char *corpus)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] = "<!doctype html><title>Catalog</title><a href='/get.php?id=7'>Download</a>";
    CHECK(browser_engine_commit_html(engine, "https://downloads.example.test/catalog", page, sizeof(page)-1, true));
    CHECK(controller_focus_next(browser_engine_controller(engine)));
    size_t history = browser_engine_navigation(engine)->history_count;
    trace_url = "https://downloads.example.test/get.php?id=7";
    CHECK(trace(corpus, 42, 0, 200, "\"one\"",
        "content-disposition: attachment; filename=package.zip", true));
    CHECK(browser_engine_begin_navigation_url(engine, trace_url, 1048576, 1000, true));
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (unsigned i = 0; i < 1000 && status == BROWSER_NAVIGATION_JOB_PENDING; i++)
        status = browser_engine_pump_navigation(engine, NULL);
    CHECK(status == BROWSER_NAVIGATION_JOB_CANCELLED);
    CHECK(browser_engine_navigation(engine)->history_count == history);
    CHECK(!strcmp(navigation_current(browser_engine_navigation(engine))->url, "https://downloads.example.test/catalog"));
    CHECK(browser_engine_controller(engine)->focus_kind == CONTROLLER_FOCUS_LINK);
    char url[2048], name[128];
    CHECK(browser_engine_take_download_offer(engine, url, sizeof(url), name, sizeof(name)));
    CHECK(!strcmp(url, trace_url) && !strcmp(name, "package.zip"));
    CHECK(!browser_engine_take_download_offer(engine, url, sizeof(url), name, sizeof(name)));
    /* A one-shot offer is neither a history entry nor an HTML document. The
       same handoff also works through the synchronous facade. */
    fetch_trace_end();
    CHECK(trace(corpus, 42, 0, 200, "\"one\"", NULL, true));
    CHECK(!browser_engine_load_url_with_limits(engine, trace_url, 1048576, 1000, true));
    CHECK(browser_engine_take_download_offer(engine, url, sizeof(url), name, sizeof(name)));
    CHECK(!strcmp(url, trace_url));
    fetch_trace_end(); trace_url = URL; browser_engine_destroy(engine);
    return true;
}

static void clean(const char *directory)
{
    DIR *dir = opendir(directory); if (dir == NULL) return;
    struct dirent *entry;
    for (unsigned i = 0; i < 100 && (entry = readdir(dir)) != NULL; i++) {
        if (entry->d_name[0] == '.') continue;
        char path[512]; snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        (void) unlink(path);
    }
    closedir(dir); (void) rmdir(directory);
}

static int compare_time(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void timing(const char *label, uint64_t *samples, size_t count)
{
    if (!count) return;
    qsort(samples, count, sizeof(*samples), compare_time);
    printf("%s n=%zu median=%lluus p95=%lluus max=%lluus\n", label, count,
        (unsigned long long)samples[count/2], (unsigned long long)samples[(count-1)*95/100],
        (unsigned long long)samples[count-1]);
}

static bool responsiveness(const char *directory, const char *corpus)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[128]; BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine);
    char html[16384]; size_t used = (size_t)snprintf(html, sizeof(html),
        "<!doctype html><style>a{display:block;padding:8px}a:focus{background:#fc0}</style>");
    for (unsigned i = 0; i < 100; i++) used += (size_t)snprintf(html+used, sizeof(html)-used,
        "<a href='/item%u'>Download test row %u</a>", i, i);
    CHECK(browser_engine_commit_html(engine, "https://downloads.example.test/catalog", html, used, true));
    CHECK(browser_engine_render_frame(engine, NULL));
    uint64_t baseline[64], pumps[1024], response[1024]; size_t count = 0;
    for (unsigned i = 0; i < 64; i++) {
        uint64_t start = tilefinch_platform_monotonic_time_us();
        CHECK(browser_engine_focus_move(engine, true) && browser_engine_render_frame(engine, NULL));
        baseline[i] = tilefinch_platform_monotonic_time_us() - start;
    }
    PspOfflineStore store;
    psp_offline_store_init(&store, browser_engine_budget(engine), browser_engine_session(engine), directory);
    uint32_t id;
    CHECK(file_download_enqueue(&store.library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 8u*1024u*1024u, 0, 200, "\"one\"", NULL, true));
    CHECK(offline_download_manager_start(&store.download, id));
    for (; count < 1024 && offline_download_manager_active(&store.download, NULL); count++) {
        uint64_t before = store.library.items[0].downloaded_bytes;
        uint64_t press = tilefinch_platform_monotonic_time_us();
        (void) psp_offline_store_pump(&store);
        pumps[count] = tilefinch_platform_monotonic_time_us() - press;
        CHECK(store.library.items[0].downloaded_bytes - before <= 32768);
        if (count % 2) {
            CHECK(browser_engine_focus_move(engine, true));
        } else (void) browser_engine_scroll_by(engine, count % 4 ? -16 : 32);
        CHECK(browser_engine_render_frame(engine, NULL));
        response[count] = tilefinch_platform_monotonic_time_us() - press;
    }
    CHECK(count && !offline_download_manager_active(&store.download, NULL));
    CHECK(store.library.items[0].state == OFFLINE_ITEM_READY);
    timing("warm focus-to-frame, idle", baseline, 64);
    timing("download owner pump", pumps, count);
    timing("simulated press before pump -> focus/scroll frame ready", response, count);
    CHECK(offline_library_remove(&store.library, id));
    fetch_trace_end(); psp_offline_store_destroy(&store); browser_engine_destroy(engine);
    return true;
}

static bool live_download(const char *directory, const char *url, const char *expected,
                          const char *keep_path)
{
    CHECK(strlen(expected) == 64);
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session; CHECK(browser_session_init(&session, &budget, 65536));
    OfflineLibrary library; offline_library_init(&library, &budget, directory);
    uint32_t id = 0; char error[128] = {0};
    CHECK(file_download_enqueue(&library, url, NULL, &id, error, sizeof(error)));
    OfflineDownloadManager manager;
    offline_download_manager_init(&manager, &budget, &session, &library);
    CHECK(offline_download_manager_start(&manager, id));
    for (unsigned i = 0; i < 30000 && offline_download_manager_active(&manager, NULL); i++) {
        (void) offline_download_manager_pump(&manager);
        usleep(1000);
    }
    const OfflineLibraryItem *item = offline_library_find(&library, id);
    if (item->state != OFFLINE_ITEM_READY) fprintf(stderr, "Live download: %s\n", item->failure_reason);
    CHECK(item->state == OFFLINE_ITEM_READY);
    char path[512]; CHECK(file_download_path(&library, id, path, sizeof(path)));
    FILE *file = fopen(path, "rb"); CHECK(file != NULL);
    TilefinchSha256 hash; tilefinch_sha256_init(&hash);
    unsigned char buffer[4096], digest[32]; size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) != 0)
        CHECK(tilefinch_sha256_update(&hash, buffer, read));
    CHECK(!ferror(file) && fclose(file) == 0 && tilefinch_sha256_final(&hash, digest));
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, expected)) fprintf(stderr, "Public download SHA-256: %s; bytes=%llu\n", hex,
        (unsigned long long) item->content_bytes);
    CHECK(!strcmp(hex, expected));
    if (keep_path != NULL) {
        FILE *source = fopen(path, "rb"), *copy = fopen(keep_path, "wbx");
        CHECK(source && copy);
        while ((read = fread(buffer, 1, sizeof(buffer), source)))
            CHECK(fwrite(buffer, 1, read, copy) == read);
        CHECK(!ferror(source) && !fclose(source) && !fclose(copy));
    }
    printf("Verified public download: %llu bytes; Budget peak %zu bytes\n",
        (unsigned long long) item->content_bytes, budget.peak);
    offline_download_manager_destroy(&manager); browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

static bool live_source_journey(const char *directory, const char *page_url,
                               const char *download_url, const char *expected)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error)); CHECK(engine);
    CHECK(browser_engine_load_url_with_limits(engine, page_url, 2u*1024u*1024u, 30000, true));
    BrowserController *controller = browser_engine_controller(engine);
    bool found = false;
    for (unsigned i = 0; i < 2048 && controller_focus_next(controller); i++) {
        char url[NAVIGATION_URL_LIMIT];
        if (controller_focused_download_url(controller, url, sizeof(url)) && !strcmp(url, download_url)) {
            found = true; break;
        }
    }
    CHECK(found);
    ControllerAction action; CHECK(controller_activate(controller, &action));
    size_t history = browser_engine_navigation(engine)->history_count;
    lxb_dom_node_t *focused = controller_focused_node(controller);
    PspOfflineStore store;
    psp_offline_store_init(&store, browser_engine_budget(engine), browser_engine_session(engine), directory);
    if (action.prefer_download) CHECK(psp_offline_store_offer_file(&store, engine, action.url));
    else {
        CHECK(browser_engine_begin_navigation_url(engine, action.url, 1048576, 30000, true));
        BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
        for (unsigned i = 0; i < 30000 && status == BROWSER_NAVIGATION_JOB_PENDING; i++) {
            status = browser_engine_pump_navigation(engine, NULL); usleep(1000);
        }
        CHECK(status == BROWSER_NAVIGATION_JOB_CANCELLED && psp_offline_store_take_download_offer(&store, engine));
    }
    CHECK(store.library.count == 0 && psp_offline_store_file_preview(&store));
    psp_offline_store_discard_file(&store);
    CHECK(browser_engine_navigation(engine)->history_count == history && controller_focused_node(controller) == focused);
    psp_offline_store_destroy(&store); browser_engine_destroy(engine);
    return live_download(directory, download_url, expected, NULL);
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/tilefinch-file-test-XXXXXX";
    char corpus[] = "/tmp/tilefinch-file-trace-XXXXXX";
    if (mkdtemp(directory) == NULL || mkdtemp(corpus) == NULL) return 1;
    bool okay = argc == 2 && !strcmp(argv[1], "--profile") ? responsiveness(directory, corpus)
        : ((argc == 4 && !strcmp(argv[1], "--fetch-url"))
        || (argc == 5 && !strcmp(argv[1], "--fetch-keep")))
        ? live_download(directory, argv[2], argv[3], argc == 5 ? argv[4] : NULL)
        : argc == 5 && !strcmp(argv[1], "--source-journey")
        ? live_source_journey(directory, argv[2], argv[3], argv[4])
        : argc == 1 && filenames() && transfers(directory, corpus)
            && restart_speed(directory, corpus) && confirmation(directory, corpus)
            && extensionless_navigation(corpus);
    fetch_trace_end(); clean(directory); clean(corpus);
    return okay ? 0 : 1;
}
