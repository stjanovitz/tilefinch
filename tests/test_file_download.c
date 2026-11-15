#include "tilefinch/file_download.h"
#include "tilefinch/psp_offline_store.h"
#include "tilefinch/sha256.h"
#include "tilefinch/platform.h"
#include "tilefinch/text_encoding.h"

#include "supervisor_view_probe.h"

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
    /* A path without a file name is a page, not download.bin. */
    CHECK(!file_download_url_candidate("https://downloads.example.test/"));
    CHECK(!file_download_url_candidate("https://downloads.example.test/settings/"));
    CHECK(!file_download_url_candidate("https://downloads.example.test/forum/?page=2"));
    CHECK(!file_download_url_candidate("https://downloads.example.test/.bin"));
    CHECK(file_download_url_candidate("https://downloads.example.test/firmware.bin?x=1"));
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
    /* A supervisor scope begun over the sheet (a pending load's pump) shows
       it; declining frees the offer, so the copy lets go of it first. */
    PspUiState live; psp_ui_init(&live);
    const PspUiFileDownloadPreview *sheet = psp_offline_store_file_preview(&store);
    psp_ui_show_file_download(&live, sheet);
    view_probe_begin(&store, &live);
    psp_offline_store_discard_file(&store);
    CHECK(view_probe_present(sheet) && view_probe.dropped == 1u);
    view_probe_end(&store);
    CHECK(psp_offline_store_file_preview(&store) == NULL && store.library.count == 0);
    CHECK(!strcmp(navigation_current(browser_engine_navigation(engine))->url, "https://downloads.example.test/"));
    CHECK(psp_offline_store_offer_file(&store, engine, URL));
    CHECK(trace(corpus, 42, 0, 200, "\"one\"", NULL, true));
    /* Accepting consumes the offer the same way. */
    sheet = psp_offline_store_file_preview(&store);
    psp_ui_show_file_download(&live, sheet);
    view_probe_begin(&store, &live);
    CHECK(psp_offline_store_accept_file(&store) && store.library.count == 1);
    CHECK(view_probe_present(sheet) && view_probe.dropped == 1u);
    view_probe_end(&store);
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

/* Pressing X on a link: only a file-like address, or a same-origin link
   marked download, asks first. A folder-style address such as /settings/
   is a page, though its saved name would fall back to download.bin. */
static bool link_activation(void)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] = "<!doctype html><title>Links</title>"
        "<a href='/settings/'>Settings</a> <a href='/'>Home</a> "
        "<a href='/forum/?page=2'>Forum</a> <a href='/about'>About</a> "
        "<a href='" URL "'>Archive</a> <a href='/get/' download>Saved</a> "
        "<a href='https://other.example.test/get/' download>Elsewhere</a>";
    CHECK(browser_engine_commit_html(engine, "https://downloads.example.test/",
                                     page, sizeof(page) - 1, true));
    static const struct { const char *url; bool download; } links[] = {
        {"https://downloads.example.test/settings/", false},
        {"https://downloads.example.test/", false},
        {"https://downloads.example.test/forum/?page=2", false},
        {"https://downloads.example.test/about", false},
        {URL, true},
        {"https://downloads.example.test/get/", true},
        {"https://other.example.test/get/", false}
    };
    BrowserController *controller = browser_engine_controller(engine);
    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        CHECK(controller_focus_next(controller));
        ControllerAction action;
        CHECK(controller_activate(controller, &action));
        if (action.type != CONTROLLER_ACTION_NAVIGATE
            || strcmp(action.url, links[i].url)
            || action.prefer_download != links[i].download)
            fprintf(stderr, "link %zu: type=%d url=%s download=%d\n", i,
                    (int) action.type, action.url, (int) action.prefer_download);
        CHECK(action.type == CONTROLLER_ACTION_NAVIGATE
              && !strcmp(action.url, links[i].url)
              && action.prefer_download == links[i].download);
    }
    /* The download link's saved name is the legitimate fallback. */
    char name[FILE_DOWNLOAD_FILENAME_LIMIT];
    CHECK(file_download_filename(links[5].url, NULL, name)
          && !strcmp(name, "download.bin"));
    browser_engine_destroy(engine);
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
    /* A nameless attachment at a folder-style address is still a file; it
       is offered under the download.bin fallback. */
    fetch_trace_end();
    trace_url = "https://downloads.example.test/files/";
    CHECK(trace(corpus, 42, 0, 200, "\"one\"", "content-disposition: attachment", true));
    CHECK(browser_engine_begin_navigation_url(engine, trace_url, 1048576, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (unsigned i = 0; i < 1000 && status == BROWSER_NAVIGATION_JOB_PENDING; i++)
        status = browser_engine_pump_navigation(engine, NULL);
    CHECK(status == BROWSER_NAVIGATION_JOB_CANCELLED);
    CHECK(browser_engine_take_download_offer(engine, url, sizeof(url), name, sizeof(name)));
    CHECK(!strcmp(url, trace_url) && !strcmp(name, "download.bin"));
    fetch_trace_end(); trace_url = URL; browser_engine_destroy(engine);
    return true;
}

/* A navigation that answers MPEG audio is handed to the native audio
   player: no body is parsed, no history entry is made, and it is not a
   file to save. */
static bool audio_navigation(const char *corpus)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] = "<!doctype html><title>Radio</title><a href='/listen'>Listen</a>";
    CHECK(browser_engine_commit_html(engine, "https://radio.example.test/", page, sizeof(page)-1, true));
    size_t history = browser_engine_navigation(engine)->history_count;
    trace_url = "https://radio.example.test/listen";
    CHECK(trace(corpus, 4096, 0, 200, "\"one\"", "content-type: audio/mpeg", false));
    CHECK(browser_engine_begin_navigation_url(engine, trace_url, 1048576, 1000, true));
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (unsigned i = 0; i < 1000 && status == BROWSER_NAVIGATION_JOB_PENDING; i++)
        status = browser_engine_pump_navigation(engine, NULL);
    CHECK(status == BROWSER_NAVIGATION_JOB_CANCELLED);
    CHECK(browser_engine_navigation(engine)->history_count == history);
    CHECK(!strcmp(navigation_current(browser_engine_navigation(engine))->url, "https://radio.example.test/"));
    char url[2048], name[128];
    CHECK(!browser_engine_take_download_offer(engine, url, sizeof(url), name, sizeof(name)));
    CHECK(browser_engine_take_media_audio_offer(engine, url, sizeof(url))
          && !strcmp(url, trace_url));
    CHECK(!browser_engine_take_media_audio_offer(engine, url, sizeof(url)));
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

/* Byte length of a well-formed UTF-8 string, 0 when it is not. */
static size_t utf8_checked_length(const char *text)
{
    size_t length = strlen(text);
    return tilefinch_utf8_valid((const unsigned char *) text, length) ? length : 0;
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t length = strlen(text), tail = strlen(suffix);
    return length >= tail && !strcmp(text + length - tail, suffix);
}

static bool display_and_stored(const char *header, const char *title,
                               const char *stored)
{
    char name[FILE_DOWNLOAD_FILENAME_LIMIT], disk[FILE_DOWNLOAD_FILENAME_LIMIT];
    CHECK(file_download_filename(URL, header, name));
    if (strcmp(name, title) != 0) fprintf(stderr, "title \"%s\" != \"%s\"\n", name, title);
    CHECK(!strcmp(name, title) && file_download_filename_valid(name));
    CHECK(file_download_stored_name(name, disk));
    if (strcmp(disk, stored) != 0) fprintf(stderr, "stored \"%s\" != \"%s\"\n", disk, stored);
    CHECK(!strcmp(disk, stored) && file_download_stored_name_valid(disk));
    return true;
}

/* A long name keeps its extension, is well-formed UTF-8 and fits; so does
   its Memory Stick name. */
static bool long_name(const char *header, const char *extension, char title[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    char disk[FILE_DOWNLOAD_FILENAME_LIMIT];
    CHECK(file_download_filename(URL, header, title));
    if (!file_download_filename_valid(title) || !ends_with(title, extension)) fprintf(stderr, "long \"%s\" %zu valid=%d\n", title, strlen(title), file_download_filename_valid(title));
    CHECK(file_download_filename_valid(title) && ends_with(title, extension));
    CHECK(utf8_checked_length(title) > 100u && strlen(title) < FILE_DOWNLOAD_FILENAME_LIMIT);
    CHECK(file_download_stored_name(title, disk) && ends_with(disk, extension)
          && file_download_stored_name_valid(disk));
    return true;
}

static bool display_names(const char *directory)
{
    /* Arabic, Hebrew, CJK and emoji names keep their characters as the
       title; the Memory Stick name is printable ASCII. */
    CHECK(display_and_stored(
        "attachment; filename*=UTF-8''%D8%B5%D9%88%D8%B1%D8%A9%20%D8%A7%D9%84%D8%B4%D9%85%D8%B3.jpg",
        "\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9 \xd8\xa7\xd9\x84\xd8\xb4\xd9\x85\xd8\xb3.jpg",
        "%D8%B5%D9%88%D8%B1%D8%A9 %D8%A7%D9%84%D8%B4%D9%85%D8%B3.jpg"));
    CHECK(display_and_stored("attachment; filename*=utf-8'he'%D7%A7%D7%95%D7%91%D7%A5.pdf",
        "\xd7\xa7\xd7\x95\xd7\x91\xd7\xa5.pdf", "%D7%A7%D7%95%D7%91%D7%A5.pdf"));
    CHECK(display_and_stored(
        "attachment; filename*=UTF-8''%E6%97%A5%E6%9C%AC%E8%AA%9E.zip",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.zip", "%E6%97%A5%E6%9C%AC%E8%AA%9E.zip"));
    /* An emoji and a joined family stay whole. */
    CHECK(display_and_stored(
        "attachment; filename*=UTF-8''%F0%9F%8E%AE%20game%20%F0%9F%91%A8%E2%80%8D%F0%9F%91%A9%E2%80%8D%F0%9F%91%A7.png",
        "\xf0\x9f\x8e\xae game \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7.png",
        "%F0%9F%8E%AE game %F0%9F%91%A8%E2%80%8D%F0%9F%91%A9%E2%80%8D%F0%9F%91%A7.png"));
    /* A raw UTF-8 filename= (common, though not RFC 6266) is kept too. */
    CHECK(display_and_stored("attachment; filename=\"\xd7\xa7\xd7\x95\xd7\x91\xd7\xa5.txt\"",
        "\xd7\xa7\xd7\x95\xd7\x91\xd7\xa5.txt", "%D7%A7%D7%95%D7%91%D7%A5.txt"));

    /* NFD (as macOS servers name files) becomes NFC: one title, and one
       Memory Stick name, for both spellings; Hangul jamo compose too. */
    CHECK(display_and_stored("attachment; filename*=UTF-8''Cafe%CC%81.txt",
        "Caf\xc3\xa9.txt", "Caf%C3%A9.txt"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''Caf%C3%A9.txt",
        "Caf\xc3\xa9.txt", "Caf%C3%A9.txt"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''%E1%84%92%E1%85%A1%E1%86%AB.zip",
        "\xed\x95\x9c.zip", "%ED%95%9C.zip"));
    /* A mark with no precomposed form stays after its letter. */
    CHECK(display_and_stored("attachment; filename*=UTF-8''%D8%A8%D9%91.zip",
        "\xd8\xa8\xd9\x91.zip", "%D8%A8%D9%91.zip"));

    /* Traversal, controls, reserved and invisible characters. */
    CHECK(display_and_stored("attachment; filename*=UTF-8''..%2F..%2F%D8%B5.zip",
        "\xd8\xb5.zip", "%D8%B5.zip"));
    CHECK(display_and_stored("attachment; filename=\"..\\\\..\\\\%D8%B5.zip\"",
        "%D8%B5.zip", "%D8%B5.zip"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''%C0%AF%C0%AE.zip", "____.zip", "____.zip"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''a%01b%C2%85c.zip", "a_b_c.zip", "a_b_c.zip"));
    /* A right-to-left override would draw "invoice[U+202E]gpj.exe" as
       "invoiceexe.jpg". */
    CHECK(display_and_stored("attachment; filename*=UTF-8''invoice%E2%80%AEgpj.exe",
        "invoice_gpj.exe", "invoice_gpj.exe"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''a%E2%81%A6b%E2%80%8Fc%EF%BB%BF.zip",
        "a_b_c_.zip", "a_b_c_.zip"));
    /* A fullwidth solidus is a character, never a separator on disk. */
    CHECK(display_and_stored("attachment; filename*=UTF-8''a%EF%BC%8Fb.zip",
        "a\xef\xbc\x8f" "b.zip", "a%EF%BC%8Fb.zip"));
    /* Bytes that are not UTF-8 (a Latin-1 name) each become '_'. */
    CHECK(display_and_stored("attachment; filename=\"caf\xe9.zip\"", "caf_.zip", "caf_.zip"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''CON.zip", "_ON.zip", "_ON.zip"));
    CHECK(display_and_stored("attachment; filename*=UTF-8''%D8%B5%D9%88%D8%B1%D8%A9.", "\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9",
        "%D8%B5%D9%88%D8%B1%D8%A9"));
    CHECK(!file_download_filename_valid("a\xe2\x80\xae" "b.zip"));
    CHECK(!file_download_filename_valid("caf\xe9.zip"));
    CHECK(!file_download_filename_valid("Cafe\xcc\x81.txt")); /* NFD */
    CHECK(!file_download_stored_name_valid("\xd8\xb5.zip")
          && !file_download_stored_name_valid("a/b.zip")
          && !file_download_stored_name_valid("..")
          && file_download_stored_name_valid("%D8%B5.zip"));

    /* Over-long names keep their extension and are cut between whole
       characters: never inside a letter, between a letter and its mark,
       inside a joined emoji or between a flag's two letters. */
    char header[1400], title[FILE_DOWNLOAD_FILENAME_LIMIT];
    size_t used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''");
    for (unsigned i = 0; i < 100; i++) used += (size_t) snprintf(header + used, sizeof(header) - used, "%%D8%%B5");
    snprintf(header + used, sizeof(header) - used, ".zip");
    CHECK(long_name(header, ".zip", title) && strlen(title) == 2u * 61u + 4u);
    used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''");
    for (unsigned i = 0; i < 40; i++) used += (size_t) snprintf(header + used, sizeof(header) - used, "%%D8%%A8%%D9%%91");
    snprintf(header + used, sizeof(header) - used, ".pdf");
    CHECK(long_name(header, ".pdf", title));
    CHECK(ends_with(title, "\xd8\xa8\xd9\x91.pdf") && strlen(title) == 4u * 30u + 4u);
    used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''x");
    for (unsigned i = 0; i < 15; i++)
        used += (size_t) snprintf(header + used, sizeof(header) - used,
            "%%F0%%9F%%91%%A8%%E2%%80%%8D%%F0%%9F%%91%%A9%%E2%%80%%8D%%F0%%9F%%91%%A7");
    snprintf(header + used, sizeof(header) - used, ".png");
    CHECK(long_name(header, ".png", title));
    CHECK(ends_with(title, "\xf0\x9f\x91\xa7.png") && (strlen(title) - 5u) % 18u == 0);
    used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''wxyz");
    for (unsigned i = 0; i < 20; i++)
        used += (size_t) snprintf(header + used, sizeof(header) - used, "%%F0%%9F%%87%%AF%%F0%%9F%%87%%B5");
    snprintf(header + used, sizeof(header) - used, ".gif");
    CHECK(long_name(header, ".gif", title) && strlen(title) == 4u + 8u * 14u + 4u);
    /* A name with no extension of ASCII letters is cut as a whole. */
    used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''");
    for (unsigned i = 0; i < 100; i++) used += (size_t) snprintf(header + used, sizeof(header) - used, "%%E6%%97%%A5");
    CHECK(file_download_filename(URL, header, title) && file_download_filename_valid(title)
          && strlen(title) == 3u * 42u);
    char disk[FILE_DOWNLOAD_FILENAME_LIMIT];
    CHECK(file_download_stored_name(title, disk) && strlen(disk) == 9u * 14u);
    /* The Memory Stick name of a long title is cut between whole
       characters, before its kept extension. */
    used = (size_t) snprintf(header, sizeof(header), "attachment; filename*=UTF-8''");
    for (unsigned i = 0; i < 60; i++) used += (size_t) snprintf(header + used, sizeof(header) - used, "%%D8%%B5");
    snprintf(header + used, sizeof(header) - used, ".jpeg");
    CHECK(long_name(header, ".jpeg", title));
    CHECK(file_download_stored_name(title, disk) && !strcmp(disk + 6u * 20u, ".jpeg"));

    /* Old saved names are printable ASCII, and each is its own Memory
       Stick name, so files saved before this scheme are found where they
       are. */
    static const char *const old[] = {"homebrew.zip", "____ ____.jpg",
        "a;b.zip", "_ON.zip", "100%.zip", "x.tar.gz", "a..zip", "download.bin"};
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++)
        CHECK(file_download_filename_valid(old[i]) && file_download_stored_name(old[i], disk)
              && !strcmp(disk, old[i]));

    /* The library: titles round-trip through the index, and paths use the
       Memory Stick name. Two files with one name get two IDs, so two
       paths; publishing never overwrites. */
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    char root[512]; snprintf(root, sizeof(root), "%s/names", directory);
    CHECK(mkdir(root, 0700) == 0);
    OfflineLibrary library; offline_library_init(&library, &budget, root);
    CHECK(offline_library_load(&library));
    char error[128] = {0}; uint32_t first = 0, second = 0, legacy = 0;
    static const char arabic[] = "\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9.jpg";
    CHECK(file_download_enqueue(&library, "https://files.test/%D8%B5%D9%88%D8%B1%D8%A9.jpg", NULL,
                                &first, error, sizeof(error)));
    CHECK(file_download_enqueue(&library, URL, arabic, &second, error, sizeof(error)));
    CHECK(file_download_enqueue(&library, URL, "____.jpg", &legacy, error, sizeof(error)));
    CHECK(!strcmp(offline_library_find(&library, first)->title, arabic)
          && !strcmp(offline_library_find(&library, second)->title, arabic));
    char path[3][FILE_DOWNLOAD_PATH_LIMIT], expected[FILE_DOWNLOAD_PATH_LIMIT];
    const uint32_t ids[3] = {first, second, legacy};
    for (size_t i = 0; i < 3; i++) {
        OfflineLibraryItem *item = offline_library_find_mutable(&library, ids[i]);
        item->state = OFFLINE_ITEM_READY;
        item->content_bytes = 3;
        CHECK(file_download_path(&library, ids[i], path[i], sizeof(path[i])));
        FILE *file = fopen(path[i], "wb");
        CHECK(file != NULL && fputs("abc", file) >= 0 && fclose(file) == 0);
    }
    snprintf(expected, sizeof(expected), "%s/%08x-%%D8%%B5%%D9%%88%%D8%%B1%%D8%%A9.jpg", root, (unsigned) first);
    CHECK(!strcmp(path[0], expected) && strcmp(path[0], path[1]) != 0);
    snprintf(expected, sizeof(expected), "%s/%08x-____.jpg", root, (unsigned) legacy);
    CHECK(!strcmp(path[2], expected));
    CHECK(offline_library_save(&library));
    /* A file title that is not printable ASCII writes index version 7,
       which a release reading at most version 6 leaves alone rather than
       look for the file under its title. */
    unsigned char header_bytes[12] = {0};
    snprintf(expected, sizeof(expected), "%s/library.bin", root);
    FILE *index = fopen(expected, "rb");
    CHECK(index != NULL && fread(header_bytes, 1, sizeof(header_bytes), index) == sizeof(header_bytes)
          && fclose(index) == 0 && header_bytes[8] == 7u);
    OfflineLibrary reread; offline_library_init(&reread, &budget, root);
    CHECK(offline_library_load(&reread) && reread.count == 3);
    CHECK(!strcmp(offline_library_find(&reread, first)->title, arabic)
          && offline_library_find(&reread, first)->state == OFFLINE_ITEM_READY
          && offline_library_find(&reread, legacy)->state == OFFLINE_ITEM_READY);
    /* The saved-file page and Downloads show the title, and the page also
       the path a USB copy finds. */
    char *html = NULL; size_t length = 0;
    CHECK(offline_library_build_file_page(&reread, &budget, first, &html, &length));
    CHECK(strstr(html, "<p dir=auto>\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9.jpg</p>") != NULL
          && strstr(html, "-%D8%B5%D9%88%D8%B1%D8%A9.jpg</p>") != NULL);
    budget_free(&budget, html);
    CHECK(offline_library_build_page(&reread, &budget, &html, &length));
    CHECK(strstr(html, "<strong>\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9.jpg</strong>") != NULL);
    budget_free(&budget, html);
    /* Delete removes the file under its Memory Stick name. */
    CHECK(offline_library_remove(&reread, first) && access(path[0], F_OK) != 0
          && access(path[1], F_OK) == 0);
    CHECK(offline_library_remove(&reread, second) && offline_library_remove(&reread, legacy)
          && access(path[1], F_OK) != 0 && access(path[2], F_OK) != 0);
    /* With only ASCII titles left, the index is version 6 again. */
    CHECK(file_download_enqueue(&reread, URL, NULL, &legacy, error, sizeof(error)));
    index = fopen(expected, "rb");
    CHECK(index != NULL && fread(header_bytes, 1, sizeof(header_bytes), index) == sizeof(header_bytes)
          && fclose(index) == 0 && header_bytes[8] == 6u);
    CHECK(offline_library_remove(&reread, legacy));
    clean(root);
    return true;
}

/* A download whose response names it in Arabic is published under the
   percent-encoded name and keeps the Arabic title. */
static bool arabic_transfer(const char *directory, const char *corpus)
{
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session; CHECK(browser_session_init(&session, &budget, 65536));
    char root[512]; snprintf(root, sizeof(root), "%s/arabic", directory);
    CHECK(mkdir(root, 0700) == 0);
    OfflineLibrary library; offline_library_init(&library, &budget, root);
    CHECK(offline_library_load(&library));
    OfflineDownloadManager manager;
    offline_download_manager_init(&manager, &budget, &session, &library);
    char error[128] = {0}; uint32_t id = 0;
    CHECK(file_download_enqueue(&library, URL, NULL, &id, error, sizeof(error)));
    CHECK(trace(corpus, 4096, 0, 200, "\"one\"",
        "content-disposition: attachment; filename*=UTF-8''%D8%B5%D9%88%D8%B1%D8%A9.jpg", true));
    CHECK(offline_download_manager_start(&manager, id) && complete(&manager));
    const OfflineLibraryItem *item = offline_library_find(&library, id);
    CHECK(item->state == OFFLINE_ITEM_READY
          && !strcmp(item->title, "\xd8\xb5\xd9\x88\xd8\xb1\xd8\xa9.jpg"));
    char expected[FILE_DOWNLOAD_PATH_LIMIT];
    snprintf(expected, sizeof(expected), "%s/%08x-%%D8%%B5%%D9%%88%%D8%%B1%%D8%%A9.jpg", root, (unsigned) id);
    CHECK(access(expected, F_OK) == 0 && exact_file(&library, id, 4096));
    CHECK(offline_library_remove(&library, id) && access(expected, F_OK) != 0);
    offline_download_manager_destroy(&manager);
    browser_session_destroy(&session);
    clean(root);
    return true;
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
        : argc == 1 && filenames() && display_names(directory)
            && arabic_transfer(directory, corpus) && transfers(directory, corpus)
            && restart_speed(directory, corpus) && confirmation(directory, corpus)
            && link_activation() && extensionless_navigation(corpus)
            && audio_navigation(corpus);
    fetch_trace_end(); clean(directory); clean(corpus);
    return okay ? 0 : 1;
}
