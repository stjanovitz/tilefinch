/* The provider's page-data locator: every carrier YouTube has used for
   ytInitialData and ytInitialPlayerResponse yields the same document on
   every route. The JSON below is synthetic, minimized provider data written
   for these tests; the <script id="yt-initial-data" type="application/json">
   carrier follows the structure reported from a channel page served that
   way (not captured: it was not served to our requests). */

typedef enum {
    PAGE_DATA_STRING,  /* var ytInitialData = '\x7b...';  (mobile) */
    PAGE_DATA_OBJECT,  /* var ytInitialData = {...};       (desktop) */
    PAGE_DATA_WINDOW,  /* window["ytInitialData"] = {...}; */
    PAGE_DATA_ELEMENT, /* <script id="yt-initial-data" type="application/json"> */
    PAGE_DATA_FORMS
} PageDataForm;

static const char *const page_data_form_names[PAGE_DATA_FORMS] = {
    "string", "object", "window", "element"};

static bool page_data_append(char *out, size_t capacity, size_t *used,
                             const char *text, size_t length)
{
    if (length >= capacity - *used) return false;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

static bool page_data_text(char *out, size_t capacity, size_t *used,
                           const char *text)
{
    return page_data_append(out, capacity, used, text, strlen(text));
}

/* One value in one carrier. The string carrier hex-escapes JSON syntax the
   way mobile pages do; the others embed the JSON text as is. */
static bool page_data_carrier(char *out, size_t capacity, size_t *used,
    PageDataForm form, bool player, const char *json)
{
    const char *name = player ? "ytInitialPlayerResponse" : "ytInitialData";
    char head[160];
    switch (form) {
    case PAGE_DATA_STRING:
        snprintf(head, sizeof(head), "<script nonce=\"n\">var %s = '", name);
        break;
    case PAGE_DATA_OBJECT:
        snprintf(head, sizeof(head), "<script nonce=\"n\">var %s = ", name);
        break;
    case PAGE_DATA_WINDOW:
        snprintf(head, sizeof(head), "<script nonce=\"n\">window[\"%s\"] = ", name);
        break;
    default:
        snprintf(head, sizeof(head), "<script id=\"%s\" type=\"application/json\" "
            "nonce=\"n\">", player ? "yt-initial-player-response" : "yt-initial-data");
        break;
    }
    if (!page_data_text(out, capacity, used, head)) return false;
    if (form == PAGE_DATA_STRING) {
        for (const char *at = json; *at != '\0'; at++) {
            char escaped[5];
            if (strchr("{}[]\"'\\<>&=", *at) != NULL) {
                snprintf(escaped, sizeof(escaped), "\\x%02x", (unsigned char) *at);
                if (!page_data_text(out, capacity, used, escaped)) return false;
            } else if (!page_data_append(out, capacity, used, at, 1u)) {
                return false;
            }
        }
    } else if (!page_data_text(out, capacity, used, json)) {
        return false;
    }
    return page_data_text(out, capacity, used,
        form == PAGE_DATA_STRING ? "';</script>"
        : form == PAGE_DATA_ELEMENT ? "</script>" : ";</script>");
}

/* A page around the carriers, with near-miss decoys before them that a
   locator must pass over. */
static bool page_data_page(char *out, size_t capacity, size_t *used,
    PageDataForm form, const char *initial, const char *player)
{
    *used = 0;
    return page_data_text(out, capacity, used,
            "<!doctype html><html><head>"
            "<div id=\"yt-initial-data\">{\"not\":\"a script\"}</div>"
            "<script id=\"yt-initial-data-shell\" type=\"application/json\">"
            "{\"decoy\":1}</script>"
            "<script nonce=\"n\">if (window.ytInitialData) ytInitialDataSeen = {};"
            "var ytInitialPlayerResponse = null;"
            "var ytInitialDataLoaded = '{}';</script>")
        && (initial == NULL
            || page_data_carrier(out, capacity, used, form, false, initial))
        /* Mobile pages pair the string carrier with an object player
           response, which is read where it sits in the page. */
        && (player == NULL
            || page_data_carrier(out, capacity, used,
                   form == PAGE_DATA_STRING ? PAGE_DATA_OBJECT : form,
                   true, player))
        && page_data_text(out, capacity, used,
            "<script nonce=\"n\">window.ytplayer.bootstrapPlayerResponse = "
            "window['ytInitialPlayerResponse'];</script></head></html>");
}

/* Each load gets its own session, so none is answered from the provider's
   document cache: every carrier is fetched and decoded. */
static int page_data_load(Budget *budget,
    const char *url, const char *fetch_url, const char *page, size_t length,
    YoutubeLiteDocument *document)
{
    BrowserSession session_storage, *session = &session_storage;
    CHECK(browser_session_init(session, budget, 512u * 1024u));
    char fixture[128], error[256] = {0};
    CHECK(write_document_replay(fixture, fetch_url, page, length, 200, "")
          && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    YoutubeLiteLoadJob *job = youtube_lite_load_begin(budget, session, url,
        1u * MIB, 2000, error, sizeof(error));
    CHECK(job != NULL);
    YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
    for (unsigned pumps = 0; pumps < 4096u
         && status == YOUTUBE_LITE_LOAD_PENDING; pumps++)
        status = youtube_lite_load_pump(job, NULL);
    if (status != YOUTUBE_LITE_LOAD_SUCCEEDED)
        fprintf(stderr, "page data %s: status=%d error=%s\n", url, status,
                youtube_lite_load_error(job));
    YoutubeLiteLoadMetrics metrics = {0};
    bool taken = status == YOUTUBE_LITE_LOAD_SUCCEEDED
        && youtube_lite_load_metrics(job, &metrics)
        && metrics.requests_completed == 1u
        && metrics.document_cache_hits == 0u
        && youtube_lite_load_take_document(job, document);
    youtube_lite_load_destroy(job);
    fetch_trace_end();
    remove_large_youtube_replay(fixture);
    browser_session_destroy(session);
    CHECK(taken);
    return 0;
}

typedef struct {
    const char *url;
    const char *fetch_url;
    const char *initial;
    const char *player;
    const char *expected[3];
    bool synchronous;
} PageDataRoute;

static int page_data_route(const PageDataRoute *route)
{
    enum { PAGE_CAPACITY = 8192 };
    Budget budget;
    budget_init(&budget, 16u * MIB);
    char *page = malloc(PAGE_CAPACITY);
    CHECK(page != NULL);
    YoutubeLiteDocument first = {0}, first_sync = {0};
    for (unsigned form = 0; form < PAGE_DATA_FORMS; form++) {
        size_t length = 0;
        CHECK(page_data_page(page, PAGE_CAPACITY, &length, (PageDataForm) form,
                             route->initial, route->player));
        YoutubeLiteDocument document = {0};
        CHECK(page_data_load(&budget, route->url, route->fetch_url,
                             page, length, &document) == 0);
        for (size_t i = 0; i < 3 && route->expected[i] != NULL; i++) {
            if (strstr(document.html, route->expected[i]) == NULL)
                fprintf(stderr, "page data %s (%s): missing %s\n", route->url,
                        page_data_form_names[form], route->expected[i]);
            CHECK(strstr(document.html, route->expected[i]) != NULL);
        }
        if (form == 0) first = document;
        else {
            CHECK(document.result_count == first.result_count
                  && document.html_length == first.html_length
                  && memcmp(document.html, first.html, first.html_length) == 0);
            youtube_lite_document_destroy(&document);
        }
        if (route->synchronous) {
            char error[256] = {0};
            YoutubeLiteDocument sync = {0};
            CHECK(youtube_lite_build_document(&budget, route->url, page, length,
                                              &sync, error, sizeof(error)));
            CHECK(strstr(sync.html, route->expected[0]) != NULL);
            if (form == 0) first_sync = sync;
            else {
                CHECK(sync.html_length == first_sync.html_length
                      && memcmp(sync.html, first_sync.html, sync.html_length) == 0);
                youtube_lite_document_destroy(&sync);
            }
        }
    }
    youtube_lite_document_destroy(&first);
    youtube_lite_document_destroy(&first_sync);
    free(page);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

#define PAGE_DATA_VIDEO(id, title) \
    "{\"videoWithContextRenderer\":{\"headline\":{\"runs\":[{\"text\":\"" title "\"}]}," \
    "\"shortBylineText\":{\"runs\":[{\"text\":\"Format channel\"}]}," \
    "\"lengthText\":{\"runs\":[{\"text\":\"3:11\"}]}," \
    "\"shortViewCountText\":{\"runs\":[{\"text\":\"12 views\"}]}," \
    "\"videoId\":\"" id "\"}}"

static int test_youtube_page_data_carriers(void)
{
    /* Markup-significant characters reach a script element's JSON as JSON
       escapes; its raw text decodes no character references. */
    static const PageDataRoute routes[] = {
        {"https://www.youtube.com/results?search_query=format",
         "https://m.youtube.com/results?search_query=format",
         "{\"contents\":[" PAGE_DATA_VIDEO("FORMAT00001",
             "Tom \\u0026 Jerry \\u003c/script\\u003e") ","
         PAGE_DATA_VIDEO("FORMAT00002", "Second 'quoted' result") "]}",
         NULL, {"FORMAT00001", "Tom &amp; Jerry &lt;/script&gt;", "FORMAT00002"},
         true},
        {"https://www.youtube.com/@formatfixture/videos",
         "https://m.youtube.com/@formatfixture/videos",
         "{\"contents\":{\"singleColumnBrowseResultsRenderer\":{\"tabs\":[{"
         "\"tabRenderer\":{\"selected\":true,\"content\":{\"richGridRenderer\":{"
         "\"contents\":[{\"richItemRenderer\":{\"content\":{\"compactVideoRenderer\":{"
         "\"videoId\":\"FORMAT00003\",\"title\":{\"runs\":[{\"text\":"
         "\"Channel upload\"}]}}}}}]}}}}]}},\"metadata\":{\"channelMetadataRenderer\":{"
         "\"title\":\"Format \\u003cchannel\\u003e\",\"externalId\":"
         "\"UCabcdefghijklmnopqrstuv\"}}}",
         NULL, {"FORMAT00003", "Channel upload", "Format &lt;channel&gt;"}, true},
        {"https://www.youtube.com/playlist?list=PLformatfixture",
         "https://m.youtube.com/playlist?list=PLformatfixture",
         "{\"metadata\":{\"playlistMetadataRenderer\":{\"title\":\"Format list\"}},"
         "\"contents\":[{\"playlistVideoRenderer\":{\"videoId\":\"FORMAT00004\","
         "\"title\":{\"simpleText\":\"Listed entry\"}}}]}",
         NULL, {"FORMAT00004", "Listed entry", NULL}, true},
        {"https://www.youtube.com/?tilefinch_topics=1", "https://m.youtube.com/",
         "{\"items\":[{\"navigationItemViewModel\":{\"text\":{\"content\":"
         "\"Music \\u0026 Clips\"},\"onTap\":{\"commandMetadata\":{"
         "\"webCommandMetadata\":{\"url\":\"/channel/UCformatmusic\"}}}}}]}",
         NULL, {"UCformatmusic", "Music &amp; Clips", NULL}, true},
        /* The watch page: renderers in ytInitialData, the title and channel
           in the player response; the synchronous builder reads the player
           response only from its assignment, so this route checks the
           cooperative load alone. */
        {"https://www.youtube.com/watch?v=FORMAT00005",
         "https://m.youtube.com/watch?v=FORMAT00005&bpctr=9999999999&has_verified=1",
         "{\"contents\":{\"singleColumnWatchNextResults\":{\"results\":{\"results\":"
         "{\"contents\":[{\"slimVideoMetadataSectionRenderer\":{}}]}}}}}",
         "{\"responseContext\":{},\"videoDetails\":{\"videoId\":\"FORMAT00005\","
         "\"title\":\"Watch \\u0026 learn\",\"author\":\"Format studio\","
         "\"lengthSeconds\":\"61\",\"viewCount\":\"42\"},"
         "\"microformat\":{\"playerMicroformatRenderer\":{\"publishDate\":"
         "\"2024-02-03\",\"category\":\"Education\"}}}",
         {"FORMAT00005", "Watch &amp; learn", "Format studio"}, false},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        CHECK(page_data_route(&routes[i]) == 0);
    return 0;
}

/* Cut-short and absent carriers: an element whose raw text ends before its
   object does, and pages with only near misses, give no data rather than
   reading past the element. */
static int test_youtube_page_data_rejects(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    static const char *const pages[] = {
        "<script id=\"yt-initial-data\" type=\"application/json\">"
        "{\"contents\":[" PAGE_DATA_VIDEO("FORMAT00009", "Cut") "</script>"
        "<script>]}</script>",
        "<div id=\"yt-initial-data\">{\"contents\":["
        PAGE_DATA_VIDEO("FORMAT00009", "Not a script") "]}</div>",
        "<script>var ytInitialData = null; x = ytInitialData == {};</script>"
        "<script>var notytInitialData = {\"contents\":["
        PAGE_DATA_VIDEO("FORMAT00009", "Other name") "]};</script>",
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        YoutubeLiteDocument document = {0};
        char error[256] = {0};
        CHECK(youtube_lite_build_document(&budget,
            "https://www.youtube.com/results?search_query=format",
            pages[i], strlen(pages[i]), &document, error, sizeof(error)));
        CHECK(document.result_count == 0
              && strstr(document.html, "FORMAT00009") == NULL);
        youtube_lite_document_destroy(&document);
    }
    CHECK(budget.current == 0);
    return 0;
}
