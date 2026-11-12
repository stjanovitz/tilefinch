/* YouTube provider and resolver costs: request counts, retained bytes and
   paging, measured on synthetic replays (review 1109 pass 2). Included by
   test_browser_engine.c after the adapter suite, whose helpers it uses. */

typedef struct {
    const char *method;
    const char *url;
    const char *content_type;
    const char *body;
    size_t body_length;
} YoutubeReplayRecord;

/* A response-keyed replay of the given records, written to a fresh
   directory: requests are answered by method and URL, and a request with no
   record fails closed, so a test can count the requests a journey makes. */
static bool write_youtube_records_replay(
    char directory[128], const YoutubeReplayRecord *records, size_t count)
{
    snprintf(directory, 128, "/tmp/tilefinch-youtube-records-XXXXXX");
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

static void remove_youtube_records_replay(const char *directory,
                                          size_t count)
{
    char path[256];
    for (size_t i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/%04zu.body", directory, i);
        (void) unlink(path);
        snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i);
        (void) unlink(path);
    }
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    (void) unlink(path);
    (void) rmdir(directory);
}

static YoutubeReplayRecord youtube_fixture_record(
    const char *method, const char *url, const char *content_type,
    const char *file, char **owned)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/tests/fixtures/%s",
             TILEFINCH_TEST_SOURCE_DIR, file);
    size_t length = 0;
    *owned = read_test_file(path, &length);
    return (YoutubeReplayRecord) {
        method, url, content_type, *owned != NULL ? *owned : "", length
    };
}

/* A player response the enriched client accepts: one progressive AVC/AAC
   format on a googlevideo host. */
static const char youtube_costs_player_response[] =
    "{\"playabilityStatus\":{\"status\":\"OK\"},"
    "\"videoDetails\":{\"videoId\":\"TFTEST00001\",\"title\":\"Fixture\"},"
    "\"streamingData\":{\"expiresInSeconds\":\"21540\",\"formats\":["
    "{\"itag\":18,\"width\":320,\"height\":240,\"bitrate\":200,"
    "\"contentLength\":\"2000\",\"approxDurationMs\":\"18950\","
    "\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001e, mp4a.40.2\\\"\","
    "\"url\":\"https://rr2.googlevideo.com/videoplayback?"
    "expire=4102444800\\u0026itag=18\"}]}}";

static YoutubeResolveJobStatus youtube_costs_resolve(
    YoutubeResolveJob *job)
{
    YoutubeResolveJobStatus status = YOUTUBE_RESOLVE_JOB_PENDING;
    for (size_t i = 0; i < 4096 && status == YOUTUBE_RESOLVE_JOB_PENDING;
         i++)
        status = youtube_resolve_job_pump(job);
    return status;
}

/* YT-01: the provider caches the API identity it read from the watch page;
   pressing play must start from it on any system language. The resolver
   rewrites PREF to hl=en first, and a whole-jar fingerprint made the cache
   miss on every non-English system, so each play ran the three-client
   direct ladder instead of one enriched request. */
static int test_youtube_resolver_reuses_provider_identity(void)
{
    char *page = NULL, *next = NULL;
    YoutubeReplayRecord records[3];
    records[0] = youtube_fixture_record(
        "GET",
        "https://m.youtube.com/watch?v=TFTEST00001&bpctr=9999999999"
        "&has_verified=1",
        "text/html; charset=utf-8", "http-site-adapter-youtube/0000.body",
        &page);
    records[1] = youtube_fixture_record(
        "POST",
        "https://m.youtube.com/youtubei/v1/next?key=fixture-key"
        "&prettyPrint=false",
        "application/json", "http-site-adapter-youtube/0001.body", &next);
    records[2] = (YoutubeReplayRecord) {
        "POST",
        "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
        "&prettyPrint=false",
        "application/json", youtube_costs_player_response,
        sizeof(youtube_costs_player_response) - 1u
    };
    char directory[128];
    CHECK(page != NULL && next != NULL
          && write_youtube_records_replay(directory, records, 3));
    TilefinchPlatformServices services = {
        .preferred_language = test_youtube_preferred_language
    };
    tilefinch_platform_set_services(&services);
    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    char error[256] = {0};
    YoutubeLiteDocument document = {0};
    YoutubeLiteResolverIdentity identity = {0};
    CHECK(browser_session_init(&session, &budget, 512u * 1024u)
          && fetch_trace_replay_begin_response_keyed(
              directory, error, sizeof(error))
          && youtube_lite_load(
              &budget, &session,
              "https://www.youtube.com/watch?v=TFTEST00001"
              "&tilefinch_view=comments",
              1u * MIB, 1000, &document, error, sizeof(error))
          && youtube_lite_resolver_identity_get(&session, &identity));
    for (unsigned play = 0; play < 2; play++) {
        YoutubeResolveJob *job = youtube_resolve_job_begin(
            &budget, &session,
            "https://www.youtube.com/watch?v=TFTEST00001",
            360, 30000, NULL, NULL);
        YoutubeResolveJobMetrics begun = {0}, finished = {0};
        CHECK(job != NULL && youtube_resolve_job_metrics(job, &begun)
              && begun.cached_identity
              && strcmp(begun.phase, "enriched-start") == 0);
        YoutubeResolveJobStatus status = youtube_costs_resolve(job);
        YoutubeStream stream = {0};
        CHECK(youtube_resolve_job_metrics(job, &finished));
        if (status != YOUTUBE_RESOLVE_JOB_COMPLETE)
            fprintf(stderr, "resolver: %s attempts=%u phase=%s client=%zu"
                    " status=%ld bytes=%zu admission=%d st=%d pumps=%zu chunks=%zu\n",
                    youtube_resolve_job_error(job), finished.attempts,
                    finished.phase, finished.client_index,
                    finished.response_status, finished.response_bytes,
                    finished.last_admission_status, (int) status, finished.request_pumps, finished.request_chunks);
        /* One player request, answered from the replay. */
        CHECK(status == YOUTUBE_RESOLVE_JOB_COMPLETE
              && finished.attempts == 1u
              && youtube_resolve_job_take(job, &stream)
              && stream.itag == 18 && stream.client_attempts == 1u);
        youtube_resolve_job_destroy(job);
    }
    /* Cookies the identity does not depend on keep it; a new visitor
       drops it. */
    CHECK(browser_session_cookie_set(
              &session, "https://fixture.test/",
              "unrelated=1; Path=/; Secure")
          && browser_session_cookie_set(
              &session, "https://m.youtube.com/",
              "PREF=hl=de&tz=UTC; Domain=.youtube.com; Path=/")
          && youtube_lite_resolver_identity_get(&session, &identity));
    CHECK(browser_session_cookie_set(
              &session, "https://m.youtube.com/",
              "VISITOR_INFO1_LIVE=fixture-new-visitor; Domain=.youtube.com;"
              " Path=/; Secure")
          && !youtube_lite_resolver_identity_get(&session, &identity));
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    youtube_lite_document_destroy(&document);
    browser_session_destroy(&session);
    remove_youtube_records_replay(directory, 3);
    free(page);
    free(next);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

/* YT-02: a finished provider load hands its HTML to the engine, which
   parses and lays it out before it destroys the job. The decoded
   ytInitialData copy (sized to the rest of the page) must not stay charged
   to the Budget through that commit. */
static int test_youtube_finished_load_releases_decoded_data(void)
{
    char directory[128];
    size_t source_bytes = 0;
    CHECK(write_large_youtube_replay(directory, &source_bytes));
    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    char error[256] = {0};
    CHECK(browser_session_init(&session, &budget, 512u * 1024u)
          && fetch_trace_replay_begin(directory, error, sizeof(error)));
    YoutubeLiteLoadJob *job = youtube_lite_load_begin(
        &budget, &session,
        "https://m.youtube.com/results?search_query=ratchet", 1u * MIB,
        5000, error, sizeof(error));
    CHECK(job != NULL);
    YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
    for (size_t i = 0; i < 4096 && status == YOUTUBE_LITE_LOAD_PENDING; i++)
        status = youtube_lite_load_pump(job, NULL);
    YoutubeLiteDocument document = {0};
    CHECK(status == YOUTUBE_LITE_LOAD_SUCCEEDED
          && youtube_lite_load_take_document(job, &document)
          && document.result_count == 4u);
    /* Page-sized allocations still charged at hand-over. The strict host
       replay keeps a response buffer of the request's maximum (1 MiB here)
       until the job is destroyed; the PSP's worker transport has none. */
    char report[4096] = {0};
    FILE *stream = fmemopen(report, sizeof(report) - 1u, "w");
    CHECK(stream != NULL);
    budget_report_active(&budget, BUDGET_CATEGORY_RESOURCE, 64u * 1024u,
                         stream);
    CHECK(fclose(stream) == 0);
    size_t page_sized = 0;
    for (const char *at = strstr(report, "size="); at != NULL;
         at = strstr(at + 5, "size=")) {
        size_t size = (size_t) strtoull(at + 5, NULL, 10);
        if (size != 1u * MIB) page_sized += size;
    }
    if (page_sized != 0) fprintf(stderr, "%s", report);
    CHECK(page_sized == 0);
    youtube_lite_load_destroy(job);
    youtube_lite_document_destroy(&document);
    fetch_trace_end();
    browser_session_destroy(&session);
    remove_large_youtube_replay(directory);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

/* A synthetic provider page: 20 entries of one renderer kind and the
   continuation for the items after them. */
static char *youtube_costs_list_page(const char *renderer, const char *id_key,
                                     const char *id_format,
                                     const char *title_key,
                                     const char *title_format, size_t *length)
{
    size_t capacity = 16u * 1024u, used = 0;
    char *page = malloc(capacity);
    if (page == NULL) return NULL;
    used += (size_t) snprintf(page + used, capacity - used,
        "<!doctype html><script>var ytInitialData = {\"contents\":{"
        "\"sectionListRenderer\":{\"contents\":[");
    for (int i = 0; i < 20; i++) {
        char id[32], title[48];
        snprintf(id, sizeof(id), id_format, i);
        snprintf(title, sizeof(title), title_format, i);
        used += (size_t) snprintf(page + used, capacity - used,
            "{\"%s\":{\"%s\":\"%s\",\"%s\":{\"runs\":[{\"text\":"
            "\"%s\"}]}}},", renderer, id_key, id, title_key, title);
    }
    used += (size_t) snprintf(page + used, capacity - used,
        "{\"continuationItemRenderer\":{\"continuationEndpoint\":{"
        "\"continuationCommand\":{\"token\":\"TOKEN_AFTER_20\"}}}}"
        "]}}};</script>");
    *length = used;
    return page;
}

/* Builds one slice and checks which of the 20 entries it shows and where
   its "Load more" link leads. */
static int youtube_costs_check_slice(
    Budget *budget, const char *url, const char *source, size_t length,
    const char *title_format, int first, int last, const char *more)
{
    YoutubeLiteDocument document = {0};
    char error[256] = {0};
    CHECK(youtube_lite_build_document(budget, url, source, length,
                                      &document, error, sizeof(error)));
    for (int i = 0; i < 20; i++) {
        char needle[64], title[48];
        snprintf(title, sizeof(title), title_format, i);
        snprintf(needle, sizeof(needle), "%s<", title);
        bool shown = strstr(document.html, needle) != NULL;
        if (shown != (i >= first && i <= last))
            fprintf(stderr, "%s: entry %d shown=%d\n", url, i, shown);
        CHECK(shown == (i >= first && i <= last));
    }
    const char *link = strstr(document.html, "<p><a class=more href=\"");
    char href[512] = {0};
    if (link != NULL) {
        link += strlen("<p><a class=more href=\"");
        const char *end = strchr(link, '"');
        if (end != NULL && (size_t) (end - link) < sizeof(href))
            memcpy(href, link, (size_t) (end - link));
    }
    if (strcmp(href, more) != 0)
        fprintf(stderr, "%s: more=\"%s\" expected \"%s\"\n", url, href, more);
    CHECK(strcmp(href, more) == 0);
    youtube_lite_document_destroy(&document);
    return 0;
}

/* YT-03: a response holds more entries than one 12-row slice. "Load more"
   must continue with entry 13 of the same response, and take YouTube's
   continuation token (the items after the whole response) only after the
   response's last slice. */
static int test_youtube_list_slices_reach_every_entry(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    size_t search_length = 0, playlists_length = 0;
    char *search = youtube_costs_list_page("videoWithContextRenderer",
        "videoId", "TFTEST000%02d", "headline", "Result %02d",
        &search_length);
    char *playlists = youtube_costs_list_page("playlistRenderer",
        "playlistId", "PLTESTPLAYLIST%02d", "title", "My playlist %02d",
        &playlists_length);
    CHECK(search != NULL && playlists != NULL);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/results?search_query=test",
        search, search_length, "Result %02d", 0, 11,
        "https://www.youtube.com/results?search_query=test&amp;"
        "tilefinch_row=12") == 0);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/results?search_query=test&tilefinch_row=12",
        search, search_length, "Result %02d", 12, 19,
        "https://www.youtube.com/results?search_query=test&amp;"
        "tilefinch_token=TOKEN_AFTER_20") == 0);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/feed/playlists?tilefinch_save=TFTEST00001",
        playlists, playlists_length, "My playlist %02d", 0, 11,
        "https://www.youtube.com/feed/playlists?tilefinch_save=TFTEST00001"
        "&amp;tilefinch_row=12") == 0);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/feed/playlists?tilefinch_save=TFTEST00001"
        "&tilefinch_row=12",
        playlists, playlists_length, "My playlist %02d", 12, 19,
        "https://www.youtube.com/feed/playlists?tilefinch_save=TFTEST00001"
        "&amp;tilefinch_token=TOKEN_AFTER_20") == 0);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/feed/history",
        search, search_length, "Result %02d", 0, 11,
        "https://www.youtube.com/feed/history?tilefinch_row=12") == 0);
    CHECK(youtube_costs_check_slice(&budget,
        "https://www.youtube.com/feed/history?tilefinch_row=12",
        search, search_length, "Result %02d", 12, 19,
        "https://www.youtube.com/feed/history?tilefinch_token="
        "TOKEN_AFTER_20") == 0);
    free(search);
    free(playlists);
    CHECK(budget.current == 0);
    return 0;
}

/* A watch page carrying the visitor and API key near its start and, when
   with_sts, the signature timestamp only at its end, after about 200 KiB:
   the prefix scan sees it in the last chunk. */
static char *youtube_costs_watch_page(bool with_sts, size_t *length)
{
    enum { FILLER = 200u * 1024u };
    char *page = malloc(FILLER + 512u);
    if (page == NULL) return NULL;
    int head = snprintf(
        page, 512u,
        "<!doctype html><html><head><script>ytcfg.set({"
        "\"VISITOR_DATA\":\"CgtGaXh0dXJlVmlzaXQ%%3D\","
        "\"INNERTUBE_API_KEY\":\"fixture-key\"});</script></head><body>");
    size_t used = (size_t) head;
    for (size_t i = 0; i < FILLER; i++) page[used++] = (i % 64u) ? 'x' : ' ';
    int tail = snprintf(
        page + used, 256u, "<script>ytcfg.set({%s});</script></body></html>",
        with_sts ? "\"STS\":20125" : "\"OTHER\":1");
    used += (size_t) tail;
    *length = used;
    return page;
}

/* YT-08: a start deferred by transport admission offers the request it
   built again instead of rebuilding it (a 19 KiB clear, the cookie header
   and three formats per pump), and the watch-page parse does not search
   again the bytes the prefix scan already searched. */
static int test_youtube_resolver_start_and_watch_work(void)
{
    static const char refusal[] =
        "{\"playabilityStatus\":{\"status\":\"UNPLAYABLE\","
        "\"reason\":\"Fixture refusal\"}}";
    size_t complete_length = 0, partial_length = 0;
    char *complete = youtube_costs_watch_page(true, &complete_length);
    char *partial = youtube_costs_watch_page(false, &partial_length);
    CHECK(complete != NULL && partial != NULL);
    for (unsigned round = 0; round < 2; round++) {
        bool with_sts = round == 0;
        YoutubeReplayRecord records[3] = {
            {"POST",
             "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
             "application/json", refusal, sizeof(refusal) - 1u},
            {"GET",
             "https://www.youtube.com/watch?v=TFTEST00002"
             "&bpctr=9999999999&has_verified=1",
             "text/html; charset=utf-8",
             with_sts ? complete : partial,
             with_sts ? complete_length : partial_length},
            {"POST",
             "https://www.youtube.com/youtubei/v1/player?key=fixture-key"
             "&prettyPrint=false",
             "application/json", youtube_costs_player_response,
             sizeof(youtube_costs_player_response) - 1u}
        };
        char directory[128];
        CHECK(write_youtube_records_replay(directory, records, 3));
        Budget budget;
        budget_init(&budget, 16u * MIB);
        BrowserSession session;
        char error[256] = {0};
        CHECK(browser_session_init(&session, &budget, 512u * 1024u)
              && fetch_trace_replay_begin_response_keyed(
                  directory, error, sizeof(error)));
        YoutubeResolveJob *job = youtube_resolve_job_begin(
            &budget, &session,
            "https://www.youtube.com/watch?v=TFTEST00002", 360, 30000,
            NULL, NULL);
        CHECK(job != NULL);
        /* Hold every replay transport slot so the first start defers. */
        uint64_t held[16] = {0};
        size_t held_count = 0;
        FetchRequest filler = {.method = "POST", .body = "{}",
                               .body_length = 2u,
                               .content_type = "application/json"};
        for (size_t i = 0; i < 16u; i++) {
            FetchBackgroundEnqueueStatus admission =
                FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
            uint64_t id =
                fetch_background_transport_enqueue_media_stream_diagnosed(
                    records[0].url, &filler, 4096u, 30000, &admission);
            if (id == 0) break;
            held[held_count++] = id;
        }
        CHECK(held_count != 0);
        for (unsigned pump = 0; pump < 5u; pump++)
            CHECK(youtube_resolve_job_pump(job)
                  == YOUTUBE_RESOLVE_JOB_PENDING);
        YoutubeResolveJobMetrics deferred = {0}, finished = {0};
        CHECK(youtube_resolve_job_metrics(job, &deferred));
        if (deferred.request_builds != 1u)
            fprintf(stderr, "resolver: %zu requests built over %zu admission"
                    " deferrals\n", deferred.request_builds,
                    deferred.admission_deferrals);
        CHECK(deferred.admission_deferrals == 5u
              && deferred.request_builds == 1u
              && strcmp(deferred.phase, "direct-start") == 0);
        for (size_t i = 0; i < held_count; i++)
            (void) fetch_background_transport_cancel(held[i], "test");
        YoutubeResolveJobStatus status = youtube_costs_resolve(job);
        CHECK(youtube_resolve_job_metrics(job, &finished));
        size_t watch_length = with_sts ? complete_length : partial_length;
        if (finished.watch_scanned_bytes > 2u * watch_length
            || finished.request_builds != (with_sts ? 4u : 3u))
            fprintf(stderr, "resolver: searched %zu watch bytes for a"
                    " %zu-byte page, built %zu requests\n",
                    finished.watch_scanned_bytes, watch_length,
                    finished.request_builds);
        /* Three refused direct clients and one enriched request: one
           build each. Each key is searched for in the bytes received until
           it is found (the first chunk holds two of them), never again by
           the parse: about 1.5 times the page here, 4.5 before. */
        CHECK(finished.request_builds == (with_sts ? 4u : 3u)
              && finished.watch_scanned_bytes <= 2u * watch_length);
        if (with_sts) {
            YoutubeStream stream = {0};
            CHECK(status == YOUTUBE_RESOLVE_JOB_COMPLETE
                  && youtube_resolve_job_take(job, &stream)
                  && stream.itag == 18 && stream.client_attempts == 4u);
        } else {
            CHECK(status == YOUTUBE_RESOLVE_JOB_FAILED
                  && strstr(youtube_resolve_job_error(job),
                            "watch: page omitted bounded player"
                            " configuration after 3 client attempts")
                         != NULL);
        }
        youtube_resolve_job_destroy(job);
        fetch_trace_end();
        browser_session_destroy(&session);
        remove_youtube_records_replay(directory, 3);
    }
    free(complete);
    free(partial);
    return 0;
}

static YoutubeLiteLoadStatus youtube_costs_feed_finish(YoutubeLiteFeedJob *job)
{
    FetchPumpQuota quota = {.maximum_body_bytes = 4096,
        .maximum_body_callbacks = 4, .maximum_time_us = 1000};
    YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
    for (unsigned turn = 0; turn < 512u && status == YOUTUBE_LITE_LOAD_PENDING;
         turn++)
        status = youtube_lite_feed_pump(job, &quota);
    return status;
}

/* YT-09: Like, Subscribe and the feeds used to download the whole mobile
   home page before every API request to read an identity the session had
   already cached. A strict replay serves its records once each, in order:
   the second rating must go straight to its API request, and a refused
   cached identity must fall back to the home page once. */
static int test_youtube_rating_reuses_cached_identity(void)
{
    static const char bootstrap[] = "<script>ytcfg.set({"
        "\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\","
        "\"INNERTUBE_CLIENT_NAME\":\"MWEB\","
        "\"VISITOR_DATA\":\"fixture-visitor\",\"LOGGED_IN\":true});</script>";
    static const char liked[] =
        "{\"responseContext\":{},\"likeStatus\":\"LIKE\"}";
    static const char removed[] =
        "{\"responseContext\":{},\"likeStatus\":\"INDIFFERENT\"}";
    static const char refused[] = "{\"error\":{\"code\":401}}";
    static const char like_url[] =
        "https://m.youtube.com/youtubei/v1/like/like?key=fixture-key"
        "&prettyPrint=false";
    static const char remove_url[] =
        "https://m.youtube.com/youtubei/v1/like/removelike?key=fixture-key"
        "&prettyPrint=false";
    GoogleIdentityRecord records[] = {
        {.url = "https://m.youtube.com/", .status = 200,
         .content_type = "text/html", .body = bootstrap},
        {.url = like_url, .status = 200, .content_type = "application/json",
         .body = liked, .method = "POST"},
        {.url = remove_url, .status = 200,
         .content_type = "application/json", .body = removed,
         .method = "POST"},
        {.url = like_url, .status = 401, .content_type = "application/json",
         .body = refused, .method = "POST"},
        {.url = "https://m.youtube.com/", .status = 200,
         .content_type = "text/html", .body = bootstrap},
        {.url = like_url, .status = 200, .content_type = "application/json",
         .body = liked, .method = "POST"}
    };
    enum { RECORDS = sizeof(records) / sizeof(records[0]) };
    char fixture[128], error[256] = {0};
    CHECK(write_google_identity_replay(fixture, records, RECORDS)
          && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 512u * 1024u)
          && browser_session_cookie_set_http(
              &session, "https://m.youtube.com/",
              "SAPISID=mock-session; Secure; HttpOnly; Path=/"));
    static const struct { bool like; long bootstrap_status; } presses[] = {
        {true, 200},  /* reads the home page */
        {false, 0},   /* signs with the cached identity */
        {true, 200}   /* refused, so reads the home page after all */
    };
    for (size_t press = 0; press < 3u; press++) {
        YoutubeLiteFeedJob *job = youtube_lite_rating_begin(
            &budget, &session, "TFTEST00001", presses[press].like, 1000);
        CHECK(job != NULL);
        YoutubeLiteLoadStatus status = youtube_costs_feed_finish(job);
        YoutubeLiteRatingDiagnostics diagnostics = {0};
        CHECK(youtube_lite_rating_diagnostics(job, &diagnostics));
        if (status != YOUTUBE_LITE_LOAD_SUCCEEDED
            || diagnostics.bootstrap_status
                   != presses[press].bootstrap_status)
            fprintf(stderr, "rating %zu: status=%d bootstrap=%ld api=%ld"
                    " phase=%u\n", press, (int) status,
                    diagnostics.bootstrap_status, diagnostics.api_status,
                    diagnostics.phase);
        CHECK(status == YOUTUBE_LITE_LOAD_SUCCEEDED
              && diagnostics.bootstrap_status
                     == presses[press].bootstrap_status);
        youtube_lite_feed_destroy(job);
    }
    browser_session_destroy(&session);
    fetch_trace_end();
    remove_google_identity_replay(fixture, RECORDS);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

/* perf-1110 R1: the PSP loop pumps a provider navigation again within one
   frame only while its progress counter moves, and its watchdog cancels a
   load whose counter stops. Both read load.progress_units, which a provider
   navigation left at zero: the watchdog then cancelled any provider load
   30 s after it started, however steadily its bytes arrived. The counter
   must move on every pump that receives body bytes or transforms, and stay
   put on a pump that only waited for the (replayed) network. */
static int test_youtube_navigation_reports_progress(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.resources.enabled = false;
    config.resources.web_fonts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-site-adapter-youtube",
        error, sizeof(error)));
    CHECK(browser_engine_begin_navigation_url(engine,
        "https://www.youtube.com/watch?v=TFTEST00001", 1u * MIB, 1000, true));
    BrowserNavigationJobQuota quota = {
        .load = {.fetch = {.maximum_body_bytes = 16u * 1024u,
                           .maximum_body_callbacks = 1,
                           .maximum_time_us = 2000}}
    };
    BrowserNavigationJobMetrics before = {0}, after = {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &before));
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    size_t progressed = 0, waited = 0, pumps = 0;
    /* The watchdog sees one pump every 8 s: the load spans more than its
       30 s stall, while no run of waiting pumps reaches it. */
    PspNavigationWatchdog watchdog;
    psp_navigation_watchdog_start(&watchdog, 0u, before.load.progress_units);
    bool expired = false;
    while (status == BROWSER_NAVIGATION_JOB_PENDING && pumps++ < 512u) {
        status = browser_engine_pump_navigation(engine, &quota);
        if (status != BROWSER_NAVIGATION_JOB_PENDING) break;
        CHECK(browser_engine_navigation_job_metrics(engine, &after));
        bool worked = after.load.body_bytes != before.load.body_bytes
            || after.transform_slices != before.transform_slices;
        bool moved = after.load.progress_units != before.load.progress_units;
        if (worked && !moved)
            fprintf(stderr, "provider pump %zu worked (body %zu, slices %zu) "
                    "without moving progress %zu\n", pumps,
                    after.load.body_bytes, after.transform_slices,
                    after.load.progress_units);
        CHECK(!worked || moved);
        if (moved) progressed++;
        else waited++;
        expired |= psp_navigation_watchdog_expired(
            &watchdog, (uint64_t) pumps * 8000000u,
            after.load.progress_units);
        before = after;
    }
    if (progressed < 2u || waited == 0u || expired || pumps * 8u <= 30u)
        fprintf(stderr, "provider progress: pumps=%zu progressed=%zu "
                "waited=%zu expired=%d status=%d\n",
                pumps, progressed, waited, expired, (int) status);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED && progressed >= 2u
          && waited != 0u && pumps * 8u > 30u && !expired);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_youtube_provider_costs(void)
{
    CHECK(test_youtube_navigation_reports_progress() == 0);
    CHECK(test_youtube_list_slices_reach_every_entry() == 0);
    CHECK(test_youtube_finished_load_releases_decoded_data() == 0);
    CHECK(test_youtube_resolver_reuses_provider_identity() == 0);
    CHECK(test_youtube_resolver_start_and_watch_work() == 0);
    CHECK(test_youtube_rating_reuses_cached_identity() == 0);
    return 0;
}
