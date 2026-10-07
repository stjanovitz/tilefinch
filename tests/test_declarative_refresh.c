/* <meta http-equiv="refresh"> and the Refresh response header: the parser
   against the HTML Standard's examples, then the engine with page
   navigations deferred, as the PSP app runs it (the frontend takes a due
   refresh and begins an ordinary navigation job). The lab's synchronous
   path is covered end to end by tests/test_declarative_refresh_lab.py. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/declarative_refresh.h"
#include "tilefinch/fetch.h"
#include "tilefinch/navigation.h"

#include <stdio.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)
#define FIXTURE TILEFINCH_TEST_SOURCE_DIR \
    "/tests/fixtures/http-declarative-refresh"
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "REFRESH CHECK failed at %s:%d: %s\n",              \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

typedef struct {
    const char *input;
    bool valid;
    uint32_t seconds;
    bool has_url;
    const char *url;
} ParseCase;

static int test_parse(void)
{
    static const ParseCase cases[] = {
        /* The HTML Standard's own examples. */
        {"300", true, 300, false, NULL},
        {"20; URL=page4.html", true, 20, true, "page4.html"},
        {"0; URL='http://example.com/'", true, 0, true,
         "http://example.com/"},
        /* Separators: ';', ',', or whitespace alone. */
        {"0;url=next", true, 0, true, "next"},
        {"0,url=next", true, 0, true, "next"},
        {"0 url=next", true, 0, true, "next"},
        {"5 ; URL = 'next'", true, 5, true, "next"},
        {"  7\t;\n url=next", true, 7, true, "next"},
        /* Quotes: either kind, unterminated, the other kind inside. */
        {"0; url=\"a'b\"", true, 0, true, "a'b"},
        {"0; url='next", true, 0, true, "next"},
        {"0; 'next'", true, 0, true, "next"},
        {"0; url='next' trailing", true, 0, true, "next"},
        /* No "url=": the whole remainder, even a partial "url". */
        {"0; next", true, 0, true, "next"},
        {"0; url next", true, 0, true, "url next"},
        {"0; uri=next", true, 0, true, "uri=next"},
        {"0; urlx=next", true, 0, true, "urlx=next"},
        /* Fractions are ignored; a bare fraction is zero seconds. */
        {"1.5", true, 1, false, NULL},
        {"1.9.9; url=x", true, 1, true, "x"},
        {".5", true, 0, false, NULL},
        {"3.", true, 3, false, NULL},
        /* A separator with nothing after it is a reload. */
        {"0;", true, 0, false, NULL},
        {"0 ; ", true, 0, false, NULL},
        {"0; url=", true, 0, true, ""},
        /* Huge times saturate rather than wrap. */
        {"99999999999999999999", true, UINT32_MAX, false, NULL},
        /* Not a refresh: no time, a sign, junk after the time. */
        {"", false, 0, false, NULL},
        {"   ", false, 0, false, NULL},
        {"url=next", false, 0, false, NULL},
        {"-1", false, 0, false, NULL},
        {"+1", false, 0, false, NULL},
        {"5x", false, 0, false, NULL},
        {"5;url=x", true, 5, true, "x"},
        {"\v5", false, 0, false, NULL},
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        const ParseCase *expected = &cases[at];
        DeclarativeRefresh parsed;
        bool valid = declarative_refresh_parse(
            expected->input, strlen(expected->input), &parsed);
        bool ok = valid == expected->valid;
        if (ok && valid) {
            ok = parsed.seconds == expected->seconds
                && parsed.has_url == expected->has_url;
            if (ok && expected->has_url) {
                ok = parsed.url_length == strlen(expected->url)
                    && memcmp(expected->input + parsed.url_offset,
                              expected->url, parsed.url_length) == 0;
            }
        }
        if (!ok) {
            fprintf(stderr, "parse case %zu \"%s\": valid=%d seconds=%u "
                    "url=\"%.*s\"\n", at, expected->input, (int) valid,
                    (unsigned) parsed.seconds, (int) parsed.url_length,
                    expected->input + parsed.url_offset);
        }
        CHECK(ok);
    }
    return 0;
}

static BrowserEngine *create_engine(bool javascript)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = javascript;
    config.javascript.document_scripts_enabled = javascript;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    if (engine == NULL) {
        fprintf(stderr, "engine creation failed: %s\n", error);
        return NULL;
    }
    /* The PSP app's mode: page navigations are jobs the frontend begins. */
    browser_engine_set_defer_script_navigation(engine, true);
    return engine;
}

static bool commit(BrowserEngine *engine, const char *url, const char *html)
{
    return browser_engine_commit_html(engine, url, html, strlen(html), true);
}

/* Advance the page clock by `milliseconds` in 16 ms turns. */
static void advance(BrowserEngine *engine, unsigned milliseconds)
{
    if (milliseconds == 0) {
        (void) browser_engine_advance_runtime(engine, 0, 4, NULL);
        return;
    }
    while (milliseconds != 0) {
        unsigned step = milliseconds < 16 ? milliseconds : 16;
        (void) browser_engine_advance_runtime(engine, step, 4, NULL);
        milliseconds -= step;
    }
}

static NavigationRefreshTake take(BrowserEngine *engine, char *url)
{
    return browser_engine_take_refresh_navigation(
        engine, url, NAVIGATION_URL_LIMIT);
}

static BrowserNavigationJobStatus pump_navigation(BrowserEngine *engine)
{
    BrowserNavigationJobStatus status =
        browser_engine_navigation_status(engine);
    for (unsigned pump = 0;
         pump < 512 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
        status = browser_engine_pump_navigation(engine, NULL);
    return status;
}

static int finish_navigation(BrowserEngine *engine)
{
    BrowserNavigationJobStatus status = pump_navigation(engine);
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED)
        fprintf(stderr, "navigation status=%d error=%s\n", (int) status,
                browser_engine_last_error(engine));
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    return 0;
}

static int load(BrowserEngine *engine, const char *url, bool record_history)
{
    CHECK(browser_engine_begin_navigation_url(engine, url, 1u * MIB, 5000,
                                              record_history));
    return finish_navigation(engine);
}

static const char *current_url(BrowserEngine *engine)
{
    const NavigationEntry *entry =
        navigation_current(browser_engine_navigation(engine));
    return entry == NULL ? "" : entry->url;
}

/* A zero-delay redirect comes due on the first turn after load and is
   handed to the frontend once, attributed to the document. */
static int test_zero_delay_redirect(void)
{
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    CHECK(commit(engine, "https://refresh.test/start",
                 "<!doctype html><title>Start</title><meta "
                 "http-equiv=\"refresh\" content=\"0;url=/next\"><p>x</p>"));
    char url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/next") == 0);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(strcmp(navigation->pending_navigation_referer,
                 "https://refresh.test/start") == 0
          && !navigation->pending_navigation_user_activated);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    browser_engine_destroy(engine);
    return 0;
}

/* The delay is whole seconds on the page clock, counted from load. */
static int test_delayed_redirect(void)
{
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    CHECK(commit(engine, "https://refresh.test/dir/start",
                 "<!doctype html><meta http-equiv=\"Refresh\" "
                 "content=\"2.9; URL='later?x=1#top'\"><p>x</p>"));
    char url[NAVIGATION_URL_LIMIT] = {0};
    advance(engine, 0); /* load complete: the timer starts */
    advance(engine, 1999);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    advance(engine, 1);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/dir/later?x=1#top") == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* No URL: refresh the document itself (an auto-reload). */
static int test_same_url_reload(void)
{
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    CHECK(commit(engine, "https://refresh.test/news?page=1",
                 "<!doctype html><meta http-equiv=refresh content=300>"
                 "<p>News</p>"));
    char url[NAVIGATION_URL_LIMIT] = {0};
    advance(engine, 0);
    advance(engine, 299990);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_PENDING
          && navigation->page.refresh.reload);
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/news?page=1") == 0);
    browser_engine_destroy(engine);
    return 0;
}

typedef struct {
    const char *name;
    const char *markup;
    /* NULL: the document declares no refresh it will follow. */
    const char *expected;
} DocumentCase;

/* Which meta counts: the first one whose content parses, anywhere in the
   document, never one inside <template> or (while scripting is on)
   <noscript>; relative URLs resolve against the document base. */
static int test_document_cases(void)
{
    static const DocumentCase cases[] = {
        {"first wins",
         "<meta http-equiv=refresh content='0;url=/one'>"
         "<meta http-equiv=refresh content='0;url=/two'>",
         "https://refresh.test/one"},
        {"unparsable skipped",
         "<meta http-equiv=refresh content='soon'>"
         "<meta http-equiv=refresh content=''>"
         "<meta http-equiv=refresh>"
         "<meta http-equiv=refresh content='0;url=/two'>",
         "https://refresh.test/two"},
        {"case-insensitive name",
         "<META HTTP-EQUIV=REFRESH CONTENT='0; URL=/upper'>",
         "https://refresh.test/upper"},
        {"other http-equiv",
         "<meta http-equiv='refresh ' content='0;url=/space'>"
         "<meta http-equiv=content-type content='0;url=/type'>", NULL},
        {"body meta", "<p>x</p><div><meta http-equiv=refresh "
         "content='0,url=/body'></div>", "https://refresh.test/body"},
        {"template", "<template><meta http-equiv=refresh "
         "content='0;url=/template'></template>", NULL},
        {"noscript with scripting", "<noscript><meta http-equiv=refresh "
         "content='0;url=/noscript'></noscript>", NULL},
        {"base", "<base href='https://other.test/sub/'>"
         "<meta http-equiv=refresh content=\"0; url='page'\">",
         "https://other.test/sub/page"},
        {"empty url", "<meta http-equiv=refresh content='0; url=\"\"'>",
         "https://refresh.test/doc"},
        {"absolute http", "<meta http-equiv=refresh "
         "content='0;url=http://plain.test/'>", "http://plain.test/"},
        {"tab in url", "<meta http-equiv=refresh "
         "content='0;url=/ta&#9;b'>", "https://refresh.test/tab"},
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        BrowserEngine *engine = create_engine(false);
        CHECK(engine != NULL);
        char markup[1024];
        snprintf(markup, sizeof(markup),
                 "<!doctype html><html><head>%s</head><body><p>doc</p>"
                 "</body></html>", cases[at].markup);
        CHECK(commit(engine, "https://refresh.test/doc", markup));
        advance(engine, 16);
        char url[NAVIGATION_URL_LIMIT] = {0};
        NavigationRefreshTake taken = take(engine, url);
        bool ok = cases[at].expected == NULL
            ? taken == NAVIGATION_REFRESH_TAKE_NONE
            : taken == NAVIGATION_REFRESH_TAKE_NAVIGATE
                && strcmp(url, cases[at].expected) == 0;
        if (!ok)
            fprintf(stderr, "document case \"%s\": take=%d url=%s\n",
                    cases[at].name, (int) taken, url);
        CHECK(ok);
        browser_engine_destroy(engine);
    }
    return 0;
}

/* javascript:, data: and other non-HTTP(S) targets are never navigated to,
   and they still are the document's one refresh. */
static int test_refused_schemes(void)
{
    static const char *const targets[] = {
        "javascript:document.title='owned'", "JavaScript:alert(1)",
        "data:text/html,owned", "file:///etc/passwd", "about:blank",
        "vbscript:x", "mailto:a@b.test"
    };
    for (size_t at = 0; at < sizeof(targets) / sizeof(targets[0]); at++) {
        BrowserEngine *engine = create_engine(true);
        CHECK(engine != NULL);
        char markup[512];
        snprintf(markup, sizeof(markup),
                 "<!doctype html><meta http-equiv=refresh content=\"0;"
                 "url=%s\"><meta http-equiv=refresh content='0;url=/after'>"
                 "<title>kept</title>", targets[at]);
        CHECK(commit(engine, "https://refresh.test/doc", markup));
        char url[NAVIGATION_URL_LIMIT] = {0};
        bool record_history = true;
        for (unsigned tick = 0; tick < 8; tick++) {
            advance(engine, 16);
            CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
            CHECK(!browser_engine_take_script_navigation(
                engine, url, sizeof(url), &record_history));
        }
        NavigationSession *navigation = browser_engine_navigation(engine);
        CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_REFUSED);
        CHECK(strcmp(navigation->page.document.title, "kept") == 0);
        CHECK(navigation->refresh.refused == 1);
        browser_engine_destroy(engine);
    }
    return 0;
}

/* A page that refreshes itself with no delay is followed
   NAVIGATION_REFRESH_CHAIN_LIMIT times, then kept, with one notice; a
   button press lets the next document refresh again. */
static int test_loop_guard(void)
{
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    static const char page[] =
        "<!doctype html><meta http-equiv=refresh content=0><p>again</p>";
    char url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(commit(engine, "https://refresh.test/loop", page));
    for (unsigned hop = 0; hop < NAVIGATION_REFRESH_CHAIN_LIMIT; hop++) {
        advance(engine, 16);
        CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
        CHECK(strcmp(url, "https://refresh.test/loop") == 0);
        /* The frontend's reload commits the same document again. */
        CHECK(browser_engine_commit_html(engine, url, page,
                                         sizeof(page) - 1u, false));
    }
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_STOPPED);
    for (unsigned tick = 0; tick < 64; tick++) {
        advance(engine, 1000);
        CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    }
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_STOPPED
          && navigation->refresh.stopped == 1
          && navigation->refresh.followed == NAVIGATION_REFRESH_CHAIN_LIMIT);
    browser_engine_note_user_input(engine);
    CHECK(browser_engine_commit_html(engine, url, page, sizeof(page) - 1u,
                                     false));
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    browser_engine_destroy(engine);
    return 0;
}

/* A navigation the user starts ends the pending refresh even when it fails
   and the page stays; editing a form field does too. */
static int test_cancellation(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    char url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(load(engine, "https://refresh.test/cancel", true) == 0);
    advance(engine, 500);
    /* Not in the replay: the load fails and the incumbent stays. */
    CHECK(browser_engine_begin_navigation_url(
        engine, "https://refresh.test/missing", 1u * MIB, 5000, true));
    CHECK(pump_navigation(engine) != BROWSER_NAVIGATION_JOB_SUCCEEDED);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.loaded
          && strcmp(current_url(engine), "https://refresh.test/cancel") == 0);
    advance(engine, 5000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_CANCELLED);

    CHECK(load(engine, "https://refresh.test/form", true) == 0);
    advance(engine, 500);
    CHECK(browser_engine_focus_move(engine, true));
    CHECK(browser_engine_insert_text(engine, "psp", 3));
    advance(engine, 5000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_CANCELLED);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

/* The PSP path end to end: the header beats a meta, the frontend begins
   the taken URL without recording history, and the refresh replaces the
   current history entry instead of adding one. */
static int test_header_and_history_replacement(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    char url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(load(engine, "https://refresh.test/other", true) == 0);
    CHECK(load(engine, "https://refresh.test/header", true) == 0);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->history_count == 2);
    CHECK(navigation->page.refresh.from_header);
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/target-header") == 0);
    CHECK(load(engine, url, false) == 0);
    CHECK(strcmp(navigation->page.document.title, "Header target") == 0);
    CHECK(navigation->history_count == 2
          && strcmp(current_url(engine),
                    "https://refresh.test/target-header") == 0
          && strcmp(navigation->history[0].url,
                    "https://refresh.test/other") == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

/* A refresh URL longer than a URL may be declares no refresh, whether it
   comes in the Refresh header or in a meta; the header is not cut short
   into some other URL. */
static int test_oversized_refresh(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    char url[NAVIGATION_URL_LIMIT] = {0};
    static const char *const pages[] = {
        "https://refresh.test/long-header", "https://refresh.test/long-meta"
    };
    for (size_t at = 0; at < 2; at++) {
        CHECK(load(engine, pages[at], true) == 0);
        advance(engine, 16);
        CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_NONE);
        CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    }
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

/* VK's redirector: a script redirect with a <noscript> meta refresh. With
   JavaScript off the meta is followed; with it on the meta is inert text
   and the script's navigation is the one taken. */
static int test_noscript_redirector(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    char url[NAVIGATION_URL_LIMIT] = {0};
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    CHECK(load(engine, "https://refresh.test/vk-away", true) == 0);
    advance(engine, 16);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/target") == 0);
    browser_engine_destroy(engine);

    engine = create_engine(true);
    CHECK(engine != NULL);
    CHECK(load(engine, "https://refresh.test/vk-away", true) == 0);
    bool record_history = true;
    bool script = false;
    for (unsigned tick = 0; tick < 8 && !script; tick++) {
        advance(engine, 16);
        CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
        script = browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history);
    }
    CHECK(script && strcmp(url, "https://refresh.test/target?via=script")
                        == 0);
    CHECK(browser_engine_navigation(engine)->page.refresh.state
          == NAVIGATION_REFRESH_NONE);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

/* A refresh meta that page script inserts after load counts, timed from
   its insertion. */
static int test_script_inserted_meta(void)
{
    BrowserEngine *engine = create_engine(true);
    CHECK(engine != NULL);
    CHECK(commit(engine, "https://refresh.test/dynamic",
                 "<!doctype html><title>Dynamic</title><p>x</p><script>"
                 "setTimeout(function(){var m=document.createElement('meta');"
                 "m.httpEquiv='refresh';m.content='1;url=/inserted';"
                 "document.head.appendChild(m);},100);</script>"));
    char url[NAVIGATION_URL_LIMIT] = {0};
    advance(engine, 160);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_PENDING);
    advance(engine, 900);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    advance(engine, 200);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/inserted") == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* A refresh inside a child frame never navigates the top-level page,
   sandboxed or not. */
static int test_frames_do_not_navigate_the_top(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    static const char *const tops[] = {
        "https://refresh.test/sandbox-top", "https://refresh.test/frame-top"
    };
    for (size_t at = 0; at < 2; at++) {
        BrowserEngine *engine = create_engine(true);
        CHECK(engine != NULL);
        CHECK(load(engine, tops[at], true) == 0);
        NavigationSession *navigation = browser_engine_navigation(engine);
        char url[NAVIGATION_URL_LIMIT] = {0};
        bool record_history = true;
        for (unsigned tick = 0; tick < 16; tick++) {
            advance(engine, 16);
            CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
            CHECK(!browser_engine_take_script_navigation(
                engine, url, sizeof(url), &record_history));
        }
        CHECK(navigation->page.frame_count == 1
              && navigation->page.frames[0].loaded);
        CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_NONE
              && strcmp(current_url(engine), tops[at]) == 0);
        browser_engine_destroy(engine);
    }
    fetch_trace_end();
    return 0;
}

/* A same-URL auto-reload is followed while the page is untouched; once the
   user has pressed a button on it, it is offered once instead (the PSP
   shows PSP_UI_STATUS_RELOAD_OFFER). A refresh to another URL is a redirect
   and is still followed. */
static int test_reload_offered_after_input(void)
{
    static const char reload_page[] =
        "<!doctype html><meta http-equiv=refresh content=1><p>News</p>";
    char url[NAVIGATION_URL_LIMIT] = {0};
    BrowserEngine *engine = create_engine(false);
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);

    /* Untouched: reloads. */
    CHECK(commit(engine, "https://refresh.test/news", reload_page));
    advance(engine, 0);
    advance(engine, 1000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/news") == 0);

    /* Touched: offered once, never followed. */
    CHECK(commit(engine, "https://refresh.test/news", reload_page));
    advance(engine, 0);
    CHECK(!navigation->page.refresh.user_touched);
    browser_engine_note_user_input(engine);
    CHECK(navigation->page.refresh.user_touched);
    advance(engine, 1000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_OFFERED);
    for (unsigned tick = 0; tick < 32; tick++) {
        advance(engine, 1000);
        CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NONE);
    }
    CHECK(navigation->page.refresh.state == NAVIGATION_REFRESH_OFFERED
          && navigation->refresh.offered == 1);

    /* A press before load completes counts too (the page is live). */
    CHECK(commit(engine, "https://refresh.test/news", reload_page));
    browser_engine_note_user_input(engine);
    advance(engine, 0);
    advance(engine, 1000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_OFFERED);

    /* Touched, but a redirect to another URL: followed. */
    CHECK(commit(engine, "https://refresh.test/news",
                 "<!doctype html><meta http-equiv=refresh "
                 "content='1;url=/elsewhere'><p>Moved</p>"));
    advance(engine, 0);
    browser_engine_note_user_input(engine);
    advance(engine, 1000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_NAVIGATE);
    CHECK(strcmp(url, "https://refresh.test/elsewhere") == 0);

    /* A same-document target with only a new fragment is not "another
       URL": still a reload of this document. */
    CHECK(commit(engine, "https://refresh.test/news",
                 "<!doctype html><meta http-equiv=refresh "
                 "content='1;url=#top'><p>Fragment</p>"));
    advance(engine, 0);
    browser_engine_note_user_input(engine);
    advance(engine, 1000);
    CHECK(take(engine, url) == NAVIGATION_REFRESH_TAKE_OFFERED);
    browser_engine_destroy(engine);
    return 0;
}

/* location.replace() replaces the current history entry; location.assign(),
   href assignment and location.reload() do not; history.replaceState() is a
   same-document change that loads nothing. Back skips a replaced page. */
static int test_location_replace_history(void)
{
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(FIXTURE, error,
                                                  sizeof(error)));
    char url[NAVIGATION_URL_LIMIT] = {0};
    bool record_history = true;
    BrowserEngine *engine = create_engine(true);
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);

    /* VK-style script redirect: replace(). */
    CHECK(load(engine, "https://refresh.test/other", true) == 0);
    CHECK(load(engine, "https://refresh.test/vk-away", true) == 0);
    CHECK(navigation->history_count == 2);
    bool script = false;
    for (unsigned tick = 0; tick < 8 && !script; tick++) {
        advance(engine, 16);
        script = browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history);
    }
    CHECK(script && !record_history && navigation->history_replace_armed);
    CHECK(load(engine, url, record_history) == 0);
    CHECK(strcmp(navigation->page.document.title, "Script target") == 0);
    CHECK(navigation->history_count == 2 && !navigation->history_replace_armed
          && strcmp(current_url(engine),
                    "https://refresh.test/target?via=script") == 0);
    CHECK(browser_engine_begin_navigation_history(engine, false, 1u * MIB,
                                                  5000));
    CHECK(finish_navigation(engine) == 0);
    CHECK(strcmp(current_url(engine), "https://refresh.test/other") == 0
          && strcmp(navigation->page.document.title, "Other") == 0);

    /* assign() and href= push a new entry. */
    static const char *const pushes[] = {
        "https://refresh.test/assign", "https://refresh.test/href"
    };
    for (size_t at = 0; at < 2; at++) {
        CHECK(load(engine, pushes[at], true) == 0);
        size_t count = navigation->history_count;
        script = false;
        for (unsigned tick = 0; tick < 8 && !script; tick++) {
            advance(engine, 16);
            script = browser_engine_take_script_navigation(
                engine, url, sizeof(url), &record_history);
        }
        CHECK(script && record_history && !navigation->history_replace_armed);
        CHECK(load(engine, url, record_history) == 0);
        CHECK(navigation->history_count == count + 1
              && strcmp(current_url(engine), "https://refresh.test/target")
                     == 0
              && strcmp(navigation->history[count - 1].url, pushes[at])
                     == 0);
    }

    /* replaceState() renames the entry in place and arms nothing. */
    CHECK(load(engine, "https://refresh.test/replace-state", true) == 0);
    size_t count = navigation->history_count;
    advance(engine, 16);
    CHECK(!browser_engine_take_script_navigation(
              engine, url, sizeof(url), &record_history)
          && !navigation->history_replace_armed
          && navigation->history_count == count);
    browser_engine_destroy(engine);

    /* reload() keeps its entry (and its scroll position). */
    engine = create_engine(true);
    CHECK(engine != NULL);
    navigation = browser_engine_navigation(engine);
    CHECK(commit(engine, "https://refresh.test/again",
                 "<!doctype html><p>x</p><script>setTimeout(function(){"
                 "location.reload()},0)</script>"));
    script = false;
    for (unsigned tick = 0; tick < 8 && !script; tick++) {
        advance(engine, 16);
        script = browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history);
    }
    CHECK(script && !record_history && !navigation->history_replace_armed);
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

/* A site adapter's page (the built-in search page, which needs no network)
   is committed directly rather than loaded: location.replace() to it still
   replaces the current entry. An armed replace ends with the navigation
   that starts next, whichever it is. */
static int test_adapter_replace_history(void)
{
    static const char away[] =
        "<!doctype html><title>Away</title><p>x</p><script>"
        "setTimeout(function(){location.replace("
        "'https://www.google.com/search?q=tilefinch')},0)</script>";
    char url[NAVIGATION_URL_LIMIT] = {0};
    bool record_history = true;
    BrowserEngine *engine = create_engine(true);
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(commit(engine, "https://refresh.test/first",
                 "<!doctype html><title>First</title><p>x</p>"));
    CHECK(commit(engine, "https://refresh.test/away", away));
    CHECK(navigation->history_count == 2);
    bool script = false;
    for (unsigned tick = 0; tick < 8 && !script; tick++) {
        advance(engine, 16);
        script = browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history);
    }
    CHECK(script && !record_history && navigation->history_replace_armed);
    CHECK(strcmp(url, "https://www.google.com/search?q=tilefinch") == 0);
    CHECK(load(engine, url, record_history) == 0);
    CHECK(navigation->history_count == 2 && !navigation->history_replace_armed
          && strcmp(current_url(engine), url) == 0
          && strcmp(navigation->history[0].url,
                    "https://refresh.test/first") == 0);

    /* The user goes elsewhere before the frontend starts the replace: that
       navigation ends the arming, and a later non-recording load of the
       armed URL keeps the user's entry. */
    CHECK(commit(engine, "https://refresh.test/away", away));
    size_t count = navigation->history_count;
    script = false;
    for (unsigned tick = 0; tick < 8 && !script; tick++) {
        advance(engine, 16);
        script = browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history);
    }
    CHECK(script && !record_history && navigation->history_replace_armed);
    CHECK(commit(engine, "https://refresh.test/elsewhere",
                 "<!doctype html><title>Elsewhere</title><p>x</p>"));
    CHECK(!navigation->history_replace_armed
          && navigation->history_count == count + 1);
    CHECK(load(engine, url, false) == 0);
    CHECK(navigation->history_count == count + 1
          && strcmp(current_url(engine), "https://refresh.test/elsewhere")
                 == 0);
    browser_engine_destroy(engine);
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= test_parse();
    failed |= test_zero_delay_redirect();
    failed |= test_delayed_redirect();
    failed |= test_same_url_reload();
    failed |= test_document_cases();
    failed |= test_refused_schemes();
    failed |= test_loop_guard();
    failed |= test_cancellation();
    failed |= test_header_and_history_replacement();
    failed |= test_oversized_refresh();
    failed |= test_noscript_redirector();
    failed |= test_script_inserted_meta();
    failed |= test_frames_do_not_navigate_the_top();
    failed |= test_reload_offered_after_input();
    failed |= test_location_replace_history();
    failed |= test_adapter_replace_history();
    if (failed == 0) puts("declarative refresh tests passed");
    return failed;
}
