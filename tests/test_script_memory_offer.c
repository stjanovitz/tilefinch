/* "Load without scripts?" after a script-memory stop (R5, user decision
   Q6): a census-like page (a server-rendered article whose app script runs
   its realm out of memory) is noted for its site, the site's next page
   offers to load without scripts, and only the reader's answer turns the
   site's JavaScript off. The PSP glue (src/psp_app/psp_app_heavy.c) calls
   exactly these pieces. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/navigation.h"
#include "tilefinch/psp_basic_fallback.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/psp_ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "SCRIPT-MEMORY-OFFER CHECK failed at %s:%d: %s\n",  \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

#define ARTICLE_URL "https://news.example.test/world/article-1"
#define NEXT_URL "https://news.example.test/world/article-2"

/* The shape of the census pages that end with scripts stopped (BBC
   article, Reuters, IGN): the article is in the HTML, and its app script
   builds far more than the realm may hold. */
static const char article[] =
    "<!doctype html><title>Article</title><header><a href='/'>News</a>"
    "</header><main><h1>Server-rendered headline</h1>"
    "<p>The first paragraph of the article is in the response.</p>"
    "<p>The second paragraph is there too, before any script runs.</p>"
    "<p>A third paragraph carries the reporting: the council met on Monday "
    "and voted to keep the library open late through the winter, after "
    "residents asked for a warm place to read and study in the evenings.</p>"
    "<p>A fourth paragraph quotes the people involved, and a fifth one says "
    "what happens next, so the page has an article's worth of text to read "
    "without any of its scripts.</p>"
    "<p><a href='/world/article-2'>Next article</a></p></main>"
    "<script>globalThis.store=[];for(let i=0;i<60000;i++)"
    "store.push({id:i,text:'hydration-record-'+i,tags:[i,i+1]});</script>";
static const char plain_script[] =
    "<!doctype html><title>Small</title><main><h1>Small page</h1>"
    "<p>Its script is tiny.</p></main>"
    "<script>document.title='Small page';</script>";

static BrowserEngine *create_engine_with_heap(size_t heap_limit)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.tile_capacity = 4u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = heap_limit;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (engine == NULL) {
        fprintf(stderr, "engine creation failed: %s\n", error);
        return NULL;
    }
    navigation_set_fixed_script_memory_limits(
        browser_engine_navigation(engine), true);
    return engine;
}

static BrowserEngine *create_engine(void)
{
    return create_engine_with_heap(3u * MIB);
}

static int test_engine_stop_reason(void)
{
    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    /* The article's realm runs out of memory; its page stays. */
    CHECK(browser_engine_commit_html(engine, ARTICLE_URL, article,
                                     sizeof(article) - 1u, true));
    CHECK(browser_engine_page_loaded(engine));
    if (!browser_engine_page_scripts_stopped_for_memory(engine)) {
        const NavigationSession *nav = browser_engine_navigation(engine);
        const ScriptResult *script = &nav->page.script_result;
        fprintf(stderr, "memory stop: reason=%d attempted=%zu failed=%zu memory=%d error=%s\n",
                (int) nav->page.script_stop.reason, (size_t) nav->script_attempted,
                (size_t) nav->script_failed, nav->page.script_memory_limited,
                script->error);
    }
    CHECK(browser_engine_page_scripts_stopped_for_memory(engine));
    CHECK(!browser_engine_page_is_visually_blank(engine));
    /* A page whose scripts fit, and one whose scripts the reader stopped,
       are not memory stops. */
    CHECK(browser_engine_commit_html(engine, NEXT_URL, plain_script,
                                     sizeof(plain_script) - 1u, true));
    CHECK(!browser_engine_page_scripts_stopped_for_memory(engine));
    CHECK(browser_engine_stop_page_scripts(engine));
    CHECK(!browser_engine_page_scripts_stopped_for_memory(engine));
    /* Without scripts the same article loads as its server page. */
    CHECK(browser_engine_set_javascript_enabled(engine, false));
    CHECK(browser_engine_commit_html(engine, ARTICLE_URL, article,
                                     sizeof(article) - 1u, true));
    CHECK(browser_engine_page_loaded(engine)
          && !browser_engine_page_scripts_stopped_for_memory(engine)
          && !browser_engine_page_is_visually_blank(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* The heavy-page "running low on memory" status
   (script_heavy_state_low_on_memory, src/psp_app/psp_app_heavy.c) needs a
   refusal. A page that leaves little heap but got everything it asked for
   (a bbc.co.uk article on a PSP: all 65 scripts loaded, 0.8 MB of
   admission headroom) is not warned, nor is a page refused one allocation
   with room left; a page whose script was skipped for memory is. The
   memory-stop
   note behind the "Load without scripts?" offer is separate and still
   follows a real stop (test_engine_stop_reason). */
static int test_low_memory_needs_a_refusal(void)
{
    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    ScriptHeavyState state;

    /* Little left, nothing refused: the old trigger (under 2 MiB free)
       fired here. */
    static const char filled[] =
        "<!doctype html><title>Filled</title><main><h1>Filled</h1>"
        "<p>Its script keeps what it builds.</p></main>"
        "<script>globalThis.keep=[];for(let i=0;i<2000;i++)"
        "keep.push({i:i,t:'record-'+i});</script>";
    CHECK(browser_engine_commit_html(engine, NEXT_URL, filled,
                                     sizeof(filled) - 1u, true));
    CHECK(browser_engine_heavy_page(engine, &state));
    if (state.memory_free >= SCRIPT_HEAVY_LOW_MEMORY_BYTES
        || state.memory_refusals != 0)
        fprintf(stderr, "filled: free=%zu refusals=%zu\n",
                state.memory_free, state.memory_refusals);
    CHECK(state.memory_free < SCRIPT_HEAVY_LOW_MEMORY_BYTES);
    CHECK(state.memory_refusals == 0);
    CHECK(!script_heavy_state_low_on_memory(&state));

    /* An allocation the heap refused (caught by the page) and the room to
       go on: a realm the memory rule keeps has at least 2 MiB to obtain
       (docs/engineering/SCRIPT_MEMORY.md, "Exhaustion"), so the refusal
       alone is not low memory. In the 3 MiB heap the realm would have been
       stopped instead. */
    static const char refused[] =
        "<!doctype html><title>Refused</title><main><h1>Refused</h1>"
        "<p>Its script asks for more than the heap has.</p></main>"
        "<script>try{globalThis.big=new ArrayBuffer(16*1024*1024)}"
        "catch(e){globalThis.big=null}</script>";
    browser_engine_destroy(engine);
    BrowserEngine *roomy = create_engine_with_heap(8u * MIB);
    CHECK(roomy != NULL);
    CHECK(browser_engine_commit_html(roomy, NEXT_URL, refused,
                                     sizeof(refused) - 1u, true));
    CHECK(browser_engine_heavy_page(roomy, &state));
    if (state.memory_refusals == 0)
        fprintf(stderr, "refused: free=%zu refusals=%zu\n",
                state.memory_free, state.memory_refusals);
    CHECK(state.memory_refusals != 0);
    CHECK(!script_heavy_state_low_on_memory(&state));
    browser_engine_destroy(roomy);
    engine = create_engine();
    CHECK(engine != NULL);

    /* A script skipped because its compile could not fit. */
    static const char head[] =
        "<!doctype html><title>Skipped</title><main><h1>Skipped</h1>"
        "<p>Its script is too large to compile here.</p></main><script>";
    static const char tail[] = "</script>";
    size_t filler = 2u * MIB;
    size_t length = sizeof(head) - 1u + filler + sizeof(tail) - 1u;
    char *skipped = malloc(length);
    CHECK(skipped != NULL);
    memcpy(skipped, head, sizeof(head) - 1u);
    char *at = skipped + sizeof(head) - 1u;
    for (size_t i = 0; i + 8u <= filler; i += 8u) memcpy(at + i, "a+=1;\n  ", 8);
    memcpy(at + filler, tail, sizeof(tail) - 1u);
    bool committed = browser_engine_commit_html(engine, NEXT_URL, skipped,
                                                length, true);
    free(skipped);
    CHECK(committed);
    CHECK(browser_engine_heavy_page(engine, &state));
    if (state.memory_refusals == 0)
        fprintf(stderr, "skipped: free=%zu refusals=%zu\n",
                state.memory_free, state.memory_refusals);
    CHECK(state.memory_refusals != 0);
    CHECK(script_heavy_state_low_on_memory(&state));
    browser_engine_destroy(engine);
    return 0;
}

static void page_ui(PspUiState *ui, const char *url, bool site_scripts)
{
    psp_ui_init(ui);
    psp_ui_set_page(ui, "Article", url, true);
    ui->screen = PSP_UI_SCREEN_PAGE;
    ui->javascript_enabled = 1u;
    ui->site_javascript_enabled = site_scripts ? 1u : 0u;
}

static int test_offer_flow(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    PspUiState ui;
    page_ui(&ui, ARTICLE_URL, true);
    /* A note names a visited site: with URL history off (the default)
       nothing is noted. */
    CHECK(!browser_profile_history_enabled(profile));
    CHECK(!psp_scripts_off_note_stop(profile, &ui));
    CHECK(!psp_scripts_off_offer_due(profile, &ui));
    browser_profile_set_history_enabled(profile, true);
    /* Nothing noted yet: no offer. */
    CHECK(!psp_scripts_off_offer_due(profile, &ui));
    /* The stop is noted for the site, once, and then any of its pages
       offers. */
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(!psp_scripts_off_note_stop(profile, &ui));
    page_ui(&ui, NEXT_URL, true);
    CHECK(psp_scripts_off_offer_due(profile, &ui));
    page_ui(&ui, "https://other-news.test/", true);
    CHECK(!psp_scripts_off_offer_due(profile, &ui));
    /* Scripts already off (globally or for the site): nothing to offer
       and nothing to note. */
    page_ui(&ui, NEXT_URL, true);
    browser_profile_set_javascript_enabled(profile, false);
    CHECK(!psp_scripts_off_offer_due(profile, &ui));
    browser_profile_set_javascript_enabled(profile, true);

    /* Declining forgets the note: no offer until another stop. Scripts
       stay on. */
    CHECK(psp_scripts_off_offer_answer(profile, engine, &ui, false)
          == PSP_SETTING_APPLIED);
    CHECK(!psp_scripts_off_offer_due(profile, &ui));
    CHECK(browser_profile_site_javascript_enabled(profile, NEXT_URL));
    CHECK(psp_scripts_off_offer_answer(profile, engine, &ui, false)
          == PSP_SETTING_UNCHANGED);

    /* Accepting turns the site's JavaScript off and forgets the note. */
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(psp_scripts_off_offer_answer(profile, engine, &ui, true)
          == PSP_SETTING_APPLIED);
    CHECK(!browser_profile_site_javascript_enabled(profile, NEXT_URL)
          && ui.site_javascript_enabled == 0u);
    CHECK(!browser_profile_site_script_memory_stopped(profile, NEXT_URL));
    page_ui(&ui, NEXT_URL, false);
    CHECK(!psp_scripts_off_note_stop(profile, &ui)
          && !psp_scripts_off_offer_due(profile, &ui));
    CHECK(browser_profile_set_site_javascript_enabled(profile, NEXT_URL, true));

    /* Only sites are kept, at most BROWSER_PROFILE_SCRIPT_MEMORY_SITE_LIMIT,
       the oldest dropped first; they persist, and resetting a site's
       permissions forgets its note. */
    char url[96];
    for (unsigned at = 0;
         at <= BROWSER_PROFILE_SCRIPT_MEMORY_SITE_LIMIT; at++) {
        snprintf(url, sizeof(url), "https://www.site%u-news.test/a/b?q=1", at);
        CHECK(browser_profile_set_site_script_memory_stopped(profile, url,
                                                             true));
    }
    CHECK(!browser_profile_site_script_memory_stopped(
        profile, "https://site0-news.test/"));
    CHECK(browser_profile_site_script_memory_stopped(
        profile, "https://m.site1-news.test/other"));
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-script-memory-%ld.cfg",
             (long) getpid());
    CHECK(browser_profile_save(profile, path));
    FILE *saved = fopen(path, "rb");
    CHECK(saved != NULL);
    char text[16384];
    size_t length = fread(text, 1u, sizeof(text) - 1u, saved);
    fclose(saved);
    text[length] = '\0';
    CHECK(strstr(text, "SMEM\tsite1-news.test\n") != NULL);
    CHECK(strstr(text, "/a/b") == NULL && strstr(text, "q=1") == NULL);
    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL && browser_profile_load(loaded, path));
    (void) unlink(path);
    for (unsigned at = 1;
         at <= BROWSER_PROFILE_SCRIPT_MEMORY_SITE_LIMIT; at++) {
        snprintf(url, sizeof(url), "https://site%u-news.test/", at);
        CHECK(browser_profile_site_script_memory_stopped(loaded, url));
    }
    CHECK(browser_profile_reset_site_permissions(
        loaded, "https://site3-news.test/"));
    CHECK(!browser_profile_site_script_memory_stopped(
        loaded, "https://site3-news.test/"));
    /* Forgetting a history address forgets its site's note. */
    CHECK(browser_profile_record_history(
        loaded, "https://site4-news.test/x", "x"));
    CHECK(browser_profile_forget_history(loaded, "https://site4-news.test/x"));
    CHECK(!browser_profile_site_script_memory_stopped(
        loaded, "https://site4-news.test/"));
    CHECK(browser_profile_site_script_memory_stopped(
        loaded, "https://site5-news.test/"));
    /* Turning URL history off forgets every note, and none is written. */
    browser_profile_set_history_enabled(loaded, false);
    browser_profile_set_history_enabled(loaded, true);
    CHECK(!browser_profile_site_script_memory_stopped(
        loaded, "https://site5-news.test/"));
    CHECK(browser_profile_set_site_script_memory_stopped(
        loaded, "https://site5-news.test/", true));
    browser_profile_set_history_enabled(loaded, false);
    CHECK(browser_profile_save(loaded, path));
    saved = fopen(path, "rb");
    CHECK(saved != NULL);
    length = fread(text, 1u, sizeof(text) - 1u, saved);
    fclose(saved);
    (void) unlink(path);
    text[length] = '\0';
    CHECK(strstr(text, "SMEM\t") == NULL);
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    browser_engine_destroy(engine);
    return 0;
}

/* The offer is a toast that X and O answer on the page. */
static int test_offer_toast(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t without[WIDTH * HEIGHT], with[WIDTH * HEIGHT];
    PspUiState ui;
    page_ui(&ui, ARTICLE_URL, true);
    PspUiInput idle = { .analog_x = 128, .analog_y = 128, .elapsed_ms = 16 };
    psp_ui_composite(&ui, without, WIDTH, HEIGHT, WIDTH);
    psp_ui_show_scripts_off_offer(&ui);
    CHECK(ui.toast_kind == PSP_UI_TOAST_SCRIPTS_OFF_OFFER && ui.toast_frames != 0);
    for (unsigned frame = 0; frame < 30u; frame++)
        (void) psp_ui_update(&ui, &idle);
    psp_ui_composite(&ui, with, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(without, with, sizeof(with)) != 0);
    PspUiInput cross = idle;
    cross.pressed = cross.held = PSP_UI_BUTTON_CONFIRM;
    PspUiIntent intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action == PSP_UI_ACTION_SCRIPTS_OFF_ACCEPT);
    CHECK(ui.toast_frames == 0);
    /* Once answered, Cross is the page's again. */
    intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action != PSP_UI_ACTION_SCRIPTS_OFF_ACCEPT);
    psp_ui_show_scripts_off_offer(&ui);
    PspUiInput circle = idle;
    circle.pressed = circle.held = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &circle);
    CHECK(intent.action == PSP_UI_ACTION_SCRIPTS_OFF_DECLINE
          && ui.toast_frames == 0);
    psp_ui_show_scripts_off_offer(NULL);
    return 0;
}

/* User decision 2026-11-14: X and O stay, but an offer the reader lets pass
   (it runs out, or the reader goes on to another page) is not shown for
   that site again this session. An answered offer is not "ignored". */
static int test_offer_ignored_for_session(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    browser_profile_set_history_enabled(profile, true);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    PspScriptsOffSession session = {0};
    PspUiState ui;
    page_ui(&ui, ARTICLE_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    page_ui(&ui, "https://other-news.test/a", true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    page_ui(&ui, NEXT_URL, true);
    CHECK(psp_scripts_off_session_due(&session, profile, &ui));
    psp_ui_show_scripts_off_offer(&ui);
    psp_scripts_off_session_shown(&session, &ui);
    /* Still on screen: not ignored yet. */
    psp_scripts_off_session_settle(&session, &ui, false);
    CHECK(session.ignored_count == 0);
    PspUiInput idle = { .analog_x = 128, .analog_y = 128, .elapsed_ms = 16 };
    for (unsigned frame = 0; ui.toast_frames != 0 && frame < 4096u; frame++)
        (void) psp_ui_update(&ui, &idle);
    CHECK(ui.toast_frames == 0);
    psp_scripts_off_session_settle(&session, &ui, false);
    /* The site's note stays, but no page of it offers this session. */
    CHECK(browser_profile_site_script_memory_stopped(profile, NEXT_URL));
    page_ui(&ui, ARTICLE_URL, true);
    CHECK(psp_scripts_off_offer_due(profile, &ui));
    CHECK(!psp_scripts_off_session_due(&session, profile, &ui));
    /* Going on to another page while the offer stands ignores it too. */
    page_ui(&ui, "https://other-news.test/b", true);
    CHECK(psp_scripts_off_session_due(&session, profile, &ui));
    psp_ui_show_scripts_off_offer(&ui);
    psp_scripts_off_session_shown(&session, &ui);
    CHECK(ui.toast_frames != 0);
    psp_scripts_off_session_settle(&session, &ui, true);
    CHECK(!psp_scripts_off_session_due(&session, profile, &ui));
    /* A fresh session (the next browser start) offers again. */
    PspScriptsOffSession next = {0};
    CHECK(psp_scripts_off_session_due(&next, profile, &ui));
    /* An answer is not ignoring: Circle declines, a later stop offers
       again in the same session. */
    psp_ui_show_scripts_off_offer(&ui);
    psp_scripts_off_session_shown(&next, &ui);
    PspUiInput circle = idle;
    circle.pressed = circle.held = PSP_UI_BUTTON_CANCEL;
    PspUiIntent intent = psp_ui_update(&ui, &circle);
    CHECK(intent.action == PSP_UI_ACTION_SCRIPTS_OFF_DECLINE);
    psp_scripts_off_session_answered(&next);
    CHECK(psp_scripts_off_offer_answer(profile, engine, &ui, false)
          == PSP_SETTING_APPLIED);
    psp_scripts_off_session_settle(&next, &ui, true);
    CHECK(next.ignored_count == 0);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(psp_scripts_off_session_due(&next, profile, &ui));
    browser_profile_destroy(profile);
    browser_engine_destroy(engine);
    return 0;
}

/* Basic view by default (supersedes the offer under Automatic, the
   default): a site whose page ran out of script memory opens in Basic view
   on the next visit, "Try full page" loads it once with scripts, and a
   full page that commits forgets the note until the next memory stop. The
   PSP glue (psp_app_page.c, psp_script_main.c) calls exactly these
   pieces around each navigation start, commit and failure. */
#define THIRD_URL "https://news.example.test/world/article-3"
static int test_basic_default_flow(void)
{
    /* Only Automatic opens a noted site in Basic view, and only when the
       profile allows its scripts. */
    CHECK(psp_basic_default_reason(BROWSER_BASIC_FALLBACK_AUTOMATIC, true,
                                   true) == PSP_BASIC_DEFAULT_SCRIPT_MEMORY);
    CHECK(psp_basic_default_reason(BROWSER_BASIC_FALLBACK_ASK, true, true)
          == PSP_BASIC_DEFAULT_NONE);
    CHECK(psp_basic_default_reason(BROWSER_BASIC_FALLBACK_OFF, true, true)
          == PSP_BASIC_DEFAULT_NONE);
    CHECK(psp_basic_default_reason(BROWSER_BASIC_FALLBACK_AUTOMATIC, false,
                                   true) == PSP_BASIC_DEFAULT_NONE);
    CHECK(psp_basic_default_reason(BROWSER_BASIC_FALLBACK_AUTOMATIC, true,
                                   false) == PSP_BASIC_DEFAULT_NONE);
    /* Automatic replaces the "Load without scripts?" offer; Ask and Off
       keep it. */
    static const PspBasicDefault fresh = {0};
    CHECK(!psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &fresh, NULL, ARTICLE_URL));
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_ASK, &fresh, NULL, ARTICLE_URL));
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_OFF, &fresh, NULL, ARTICLE_URL));

    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    CHECK(browser_profile_basic_fallback_mode(profile)
          == BROWSER_BASIC_FALLBACK_AUTOMATIC);
    browser_profile_set_history_enabled(profile, true);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    PspBasicDefault state = {0};
    PspUiState ui;
    bool changed = true;

    /* The first visit: scripts on, they run out of memory, the site is
       noted (psp_app_heavy.c). */
    CHECK(psp_basic_default_navigation(&state, profile, ARTICLE_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(browser_engine_set_javascript_enabled(engine, true));
    CHECK(browser_engine_commit_html(engine, ARTICLE_URL, article,
                                     sizeof(article) - 1u, true));
    CHECK(psp_basic_default_commit(&state, profile, ARTICLE_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_NONE && !changed);
    CHECK(browser_engine_page_scripts_stopped_for_memory(engine));
    page_ui(&ui, ARTICLE_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));

    /* The next page of the site loads without scripts and is presented
       in Basic view, whole. */
    CHECK(!psp_basic_default_navigation(&state, profile, NEXT_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_BASIC_LOADING
          && state.reason == PSP_BASIC_DEFAULT_SCRIPT_MEMORY);
    CHECK(browser_engine_set_javascript_enabled(engine, false));
    CHECK(browser_engine_commit_html(engine, NEXT_URL, article,
                                     sizeof(article) - 1u, true));
    CHECK(psp_basic_default_commit(&state, profile, NEXT_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC && !changed);
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(!browser_engine_page_scripts_stopped_for_memory(engine));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(browser_engine_prepare_basic_view(engine, &analysis));
    CHECK(analysis.extracted_nodes != 0 && !analysis.extraction_truncated);
    CHECK(analysis.dropped_controls == 0
          && psp_basic_default_presentable(&state, profile, NEXT_URL,
                                           &analysis));
    CHECK(browser_engine_activate_basic_view(engine));
    /* The note stays: every page of the site opens this way. */
    CHECK(browser_profile_site_script_memory_stopped(profile, NEXT_URL));

    /* A load that fails before its commit leaves nothing behind. */
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    psp_basic_default_failed(&state);
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_basic_default_commit(&state, profile, THIRD_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_NONE);

    /* Try full page is for that address only: going elsewhere drops it. */
    psp_basic_default_request_full_page(&state, NEXT_URL);
    CHECK(state.phase == PSP_BASIC_DEFAULT_FULL_REQUESTED);
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    psp_basic_default_failed(&state);
    /* A request whose reload could not start stays for the next one; the
       fragment does not make it a different page. */
    psp_basic_default_request_full_page(&state, NEXT_URL);
    psp_basic_default_failed(&state);
    CHECK(state.phase == PSP_BASIC_DEFAULT_FULL_REQUESTED);
    CHECK(psp_basic_default_navigation(&state, profile, NEXT_URL "#top", "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_FULL_LOADING);
    CHECK(browser_engine_set_javascript_enabled(engine, true));
    CHECK(browser_engine_commit_html(engine, NEXT_URL, article,
                                     sizeof(article) - 1u, true));
    /* The full page committed: the site opens normally again... */
    CHECK(psp_basic_default_commit(&state, profile, NEXT_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_FULL && changed);
    CHECK(!browser_profile_site_script_memory_stopped(profile, NEXT_URL));
    CHECK(psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    psp_basic_default_failed(&state);
    /* ...until its scripts run out of memory again, as they did here. */
    CHECK(browser_engine_page_scripts_stopped_for_memory(engine));
    page_ui(&ui, NEXT_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    psp_basic_default_failed(&state);

    /* Basic view is presented only when it keeps the page's controls: a
       converter whose amount field sits in a form Basic view cannot keep
       (POST) loads again with scripts, and its site keeps the offer for
       the rest of the session. */
    static const char converter[] =
        "<!doctype html><title>Converter</title><main><h1>1 USD to EUR</h1>"
        "<form method=post action='/convert'><input name=amount value=1>"
        "<select name=to><option>EUR</option></select></form>"
        "<p>1 USD = 0.88 EUR</p></main>";
#define CONVERTER_URL "https://www.rates-converter.test/convert"
    page_ui(&ui, CONVERTER_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(!psp_basic_default_navigation(&state, profile, CONVERTER_URL, "GET"));
    CHECK(browser_engine_set_javascript_enabled(engine, false));
    CHECK(browser_engine_commit_html(engine, CONVERTER_URL, converter,
                                     sizeof(converter) - 1u, true));
    CHECK(psp_basic_default_commit(&state, profile, CONVERTER_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC);
    memset(&analysis, 0, sizeof(analysis));
    CHECK(browser_engine_prepare_basic_view(engine, &analysis));
    if (analysis.dropped_controls != 2u)
        fprintf(stderr, "converter dropped=%u\n",
                (unsigned) analysis.dropped_controls);
    CHECK(analysis.dropped_controls == 2u
          && !psp_basic_default_presentable(&state, profile, CONVERTER_URL,
                                            &analysis));
    CHECK(!psp_basic_default_presentable(&state, profile, CONVERTER_URL,
                                         NULL));
    CHECK(!psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, profile, CONVERTER_URL));
    CHECK(psp_basic_default_keep_full_page(&state, profile, CONVERTER_URL,
                                           true));
    CHECK(!psp_basic_default_keep_full_page(&state, profile,
                                            CONVERTER_URL "?again=1", true));
    CHECK(state.full_site_count == 1u);
    CHECK(browser_profile_site_loads_in_full(profile, CONVERTER_URL));
    CHECK(psp_basic_default_navigation(&state, profile, CONVERTER_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, profile, CONVERTER_URL));
    CHECK(browser_profile_site_script_memory_stopped(profile, CONVERTER_URL));
    /* Other noted sites still open in Basic view; the list keeps the
       newest sites. */
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    psp_basic_default_failed(&state);
    char site_url[96];
    for (unsigned at = 0; at < PSP_BASIC_DEFAULT_FULL_SITE_LIMIT; at++) {
        snprintf(site_url, sizeof(site_url), "https://site%u.test/", at);
        CHECK(!psp_basic_default_keep_full_page(&state, profile, site_url,
                                                false));
    }
    CHECK(state.full_site_count == PSP_BASIC_DEFAULT_FULL_SITE_LIMIT);
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, profile,
        "https://site7.test/a"));
    /* The session list dropped the converter's site; the profile still
       remembers it loads in full. */
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, profile, CONVERTER_URL));
    CHECK(psp_basic_default_navigation(&state, profile, CONVERTER_URL, "GET"));
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, NULL, CONVERTER_URL)
          == false);

    /* Ask keeps the offer; scripts off for the site, or everywhere, is
       an ordinary script-free load, not Basic by default. */
    CHECK(browser_profile_set_basic_fallback_mode(
        profile, BROWSER_BASIC_FALLBACK_ASK));
    CHECK(psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_scripts_off_offer_due(profile, &ui));
    CHECK(browser_profile_set_basic_fallback_mode(
        profile, BROWSER_BASIC_FALLBACK_AUTOMATIC));
    browser_profile_set_javascript_enabled(profile, false);
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_basic_default_commit(&state, profile, THIRD_URL, &changed)
          == PSP_BASIC_DEFAULT_COMMIT_NONE);
    browser_profile_set_javascript_enabled(profile, true);
    /* With URL history off no site is noted, so none opens this way. */
    browser_profile_set_history_enabled(profile, false);
    CHECK(psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    browser_profile_destroy(profile);
    browser_engine_destroy(engine);
    return 0;
}

/* The guard around Basic view by default (review of 0.1.51+): only a GET
   opens in Basic view; a start the engine refuses changes nothing and one
   that fails gives the engine its JavaScript back; an app shell, a page
   whose controls the view cuts and a cross-site landing load in full,
   remembered for the requested site and, with history on, in the profile;
   a site's "always Reader" wins; the notice is answerable once per site per
   session; and Try full page belongs to the page presented. */
#define REDIRECT_URL "https://landing-elsewhere.test/welcome"
static int test_basic_default_guards(void)
{
    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    browser_profile_set_history_enabled(profile, true);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    PspUiState ui;
    page_ui(&ui, ARTICLE_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    PspBasicDefault state = {0};

    /* A form's POST result is the server's answer to it: it loads as the
       site's policy says, and leaves the state idle. */
    CHECK(psp_basic_default_navigation(&state, profile, NEXT_URL, "POST"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_basic_default_navigation(&state, profile, NEXT_URL, "post"));
    CHECK(!psp_basic_default_navigation(&state, profile, NEXT_URL, "get"));
    CHECK(!psp_basic_default_navigation(&state, profile, NEXT_URL, NULL));
    psp_basic_default_failed(&state);

    /* The engine refuses: the state is as it was (a pending Try full page
       stays pending). */
    psp_basic_default_request_full_page(&state, NEXT_URL);
    CHECK(!psp_basic_default_begin(&state, profile, NULL, NEXT_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_FULL_REQUESTED);
    psp_basic_default_failed(&state);
    psp_basic_default_request_full_page(&state, NULL);
    CHECK(!psp_basic_default_begin(&state, profile, NULL, THIRD_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    /* A Basic start that then fails gives the engine the site's policy
       back: the next page's scripts run. */
    CHECK(psp_basic_default_begin(&state, profile, engine, THIRD_URL, "GET")
          && state.phase == PSP_BASIC_DEFAULT_BASIC_LOADING);
    psp_basic_default_abandon(&state, profile, engine);
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(browser_engine_commit_html(engine, "https://small.example.test/",
                                     plain_script, sizeof(plain_script) - 1u,
                                     true));
    BrowserViewSnapshot view;
    CHECK(browser_engine_view_snapshot(engine, &view)
          && strcmp(view.title, "Small page") == 0);

    /* An app shell with nothing to read without scripts is not presented,
       nor is a page with a cut control. */
    ReaderDocumentAnalysis article_view = {
        .kind = READER_PAGE_BASIC, .prepared = true,
        .visible_text_bytes = 2048u, .extracted_nodes = 40u
    };
    ReaderDocumentAnalysis shell = article_view;
    shell.visible_text_bytes = 31u;
    ReaderDocumentAnalysis cut = article_view;
    cut.dropped_controls = 1u;
    CHECK(!psp_basic_default_navigation(&state, profile, NEXT_URL, "GET"));
    CHECK(psp_basic_default_commit(&state, profile, NEXT_URL, NULL)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC);
    CHECK(psp_basic_default_presentable(&state, profile, NEXT_URL,
                                        &article_view));
    CHECK(!psp_basic_default_presentable(&state, profile, NEXT_URL, &shell));
    CHECK(!psp_basic_default_presentable(&state, profile, NEXT_URL, &cut));

    /* The notice takes X and O on the site's first page this session;
       later pages get the plain status. Try full page belongs to the page
       presented, not to one shown after it. */
    CHECK(psp_basic_default_presented(&state, NEXT_URL));
    CHECK(psp_basic_default_page_current(&state, NEXT_URL)
          && psp_basic_default_page_current(&state, NEXT_URL "#comments")
          && !psp_basic_default_page_current(&state, THIRD_URL));
    CHECK(!psp_basic_default_navigation(&state, profile, THIRD_URL, "GET"));
    CHECK(psp_basic_default_commit(&state, profile, THIRD_URL, NULL)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC);
    CHECK(!psp_basic_default_page_current(&state, NEXT_URL));
    CHECK(!psp_basic_default_presented(&state, THIRD_URL));
    CHECK(psp_basic_default_page_current(&state, THIRD_URL));
    /* An ordinary commit presents nothing. */
    CHECK(psp_basic_default_navigation(&state, profile,
                                       "https://elsewhere.test/", "GET"));
    CHECK(psp_basic_default_commit(&state, profile, "https://elsewhere.test/",
                                   NULL) == PSP_BASIC_DEFAULT_COMMIT_NONE);
    CHECK(!psp_basic_default_page_current(&state, THIRD_URL));

    /* A site set to always open in Reader is left to Reader. */
    CHECK(!psp_basic_default_reader_wins(profile, NEXT_URL, false));
    CHECK(psp_basic_default_reader_wins(profile, NEXT_URL, true));
    CHECK(browser_profile_set_reader_site_always(profile, NEXT_URL, true));
    CHECK(psp_basic_default_reader_wins(profile, NEXT_URL, false));
    CHECK(browser_profile_set_reader_site_always(profile, NEXT_URL, false));

    /* A redirect to another site lands on a page nobody noted: it loads
       with scripts, and the requested site, not the landing one, loads in
       full from now on, in this session and the next. */
    CHECK(!psp_basic_default_navigation(&state, profile, ARTICLE_URL, "GET"));
    CHECK(psp_basic_default_commit(&state, profile, REDIRECT_URL, NULL)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC);
    CHECK(!psp_basic_default_presentable(&state, profile, REDIRECT_URL,
                                         &article_view));
    CHECK(psp_basic_default_keep_full_page(&state, profile, REDIRECT_URL,
                                           true));
    CHECK(browser_profile_site_loads_in_full(profile, ARTICLE_URL)
          && !browser_profile_site_loads_in_full(profile, REDIRECT_URL));
    CHECK(psp_basic_default_navigation(&state, profile, NEXT_URL, "GET"));
    CHECK(state.phase == PSP_BASIC_DEFAULT_IDLE);
    CHECK(psp_basic_default_scripts_off_offer_allowed(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, &state, profile, NEXT_URL));
    PspBasicDefault next_session = {0};
    CHECK(psp_basic_default_navigation(&next_session, profile, NEXT_URL,
                                       "GET"));
    /* A redirect within the site, or to another noted site, is
       presented. */
    page_ui(&ui, REDIRECT_URL, true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    page_ui(&ui, "https://www.rates-other.test/", true);
    CHECK(psp_scripts_off_note_stop(profile, &ui));
    CHECK(!psp_basic_default_navigation(&next_session, profile,
                                        "https://www.rates-other.test/",
                                        "GET"));
    CHECK(psp_basic_default_commit(&next_session, profile, REDIRECT_URL, NULL)
          == PSP_BASIC_DEFAULT_COMMIT_BASIC);
    CHECK(psp_basic_default_presentable(&next_session, profile, REDIRECT_URL,
                                        &article_view));

    /* The mark is saved with the profile, as SMEMFULL records, only with
       URL history on; Reset site settings and forgetting the address
       forget it. */
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-basic-full-%ld.cfg",
             (long) getpid());
    CHECK(browser_profile_save(profile, path));
    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL && browser_profile_load(loaded, path));
    CHECK(browser_profile_site_loads_in_full(loaded, ARTICLE_URL));
    CHECK(browser_profile_reset_site_permissions(loaded, ARTICLE_URL)
          && !browser_profile_site_loads_in_full(loaded, ARTICLE_URL));
    CHECK(browser_profile_set_site_loads_in_full(loaded, ARTICLE_URL, true));
    CHECK(browser_profile_record_history(loaded, ARTICLE_URL, "x")
          && browser_profile_forget_history(loaded, ARTICLE_URL)
          && !browser_profile_site_loads_in_full(loaded, ARTICLE_URL));
    CHECK(browser_profile_set_site_loads_in_full(loaded, ARTICLE_URL, true));
    browser_profile_set_history_enabled(loaded, false);
    CHECK(!browser_profile_site_loads_in_full(loaded, ARTICLE_URL)
          && !browser_profile_set_site_loads_in_full(loaded, ARTICLE_URL,
                                                     true));
    CHECK(browser_profile_save(loaded, path));
    FILE *saved = fopen(path, "rb");
    CHECK(saved != NULL);
    char text[16384];
    size_t length = fread(text, 1u, sizeof(text) - 1u, saved);
    fclose(saved);
    text[length] = '\0';
    (void) unlink(path);
    CHECK(strstr(text, "SMEMFULL") == NULL);
    /* At most BROWSER_PROFILE_SCRIPT_MEMORY_SITE_LIMIT, oldest out. */
    browser_profile_set_history_enabled(loaded, true);
    char url[96];
    for (unsigned at = 0; at <= BROWSER_PROFILE_SCRIPT_MEMORY_SITE_LIMIT;
         at++) {
        snprintf(url, sizeof(url), "https://full%u.test/", at);
        CHECK(browser_profile_set_site_loads_in_full(loaded, url, true));
    }
    CHECK(!browser_profile_site_loads_in_full(loaded, "https://full0.test/")
          && browser_profile_site_loads_in_full(loaded, "https://full1.test/"));
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    browser_engine_destroy(engine);
    return 0;
}

/* The notice says why the page is in Basic view; X loads the full page,
   O keeps Basic view. */
static int test_basic_default_notice(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t without[WIDTH * HEIGHT], with[WIDTH * HEIGHT];
    PspUiState ui;
    page_ui(&ui, ARTICLE_URL, true);
    PspUiInput idle = { .analog_x = 128, .analog_y = 128, .elapsed_ms = 16 };
    psp_ui_composite(&ui, without, WIDTH, HEIGHT, WIDTH);
    psp_ui_show_basic_default_notice(&ui);
    CHECK(ui.toast_kind == PSP_UI_TOAST_BASIC_DEFAULT && ui.toast_frames != 0);
    for (unsigned frame = 0; frame < 30u; frame++)
        (void) psp_ui_update(&ui, &idle);
    psp_ui_composite(&ui, with, WIDTH, HEIGHT, WIDTH);
    CHECK(memcmp(without, with, sizeof(with)) != 0);
    PspUiInput cross = idle;
    cross.pressed = cross.held = PSP_UI_BUTTON_CONFIRM;
    PspUiIntent intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action == PSP_UI_ACTION_TRY_FULL_PAGE && ui.toast_frames == 0);
    CHECK(strcmp(psp_ui_action_name(PSP_UI_ACTION_TRY_FULL_PAGE),
                 "try-full-page") == 0);
    intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action != PSP_UI_ACTION_TRY_FULL_PAGE);
    psp_ui_show_basic_default_notice(&ui);
    PspUiInput circle = idle;
    circle.pressed = circle.held = PSP_UI_BUTTON_CANCEL;
    intent = psp_ui_update(&ui, &circle);
    CHECK(intent.action == PSP_UI_ACTION_NONE && ui.toast_frames == 0);
    psp_ui_show_basic_default_notice(NULL);

    /* A commit's notice is what its status shows: "Page ready" must not
       replace it (psp_loop_finish_navigation used to show the ready status
       after the notice, so neither it nor the bot-wall notice survived). */
    static const char reason[] =
        "BASIC VIEW: SCRIPTS RAN OUT OF MEMORY LAST TIME";
    page_ui(&ui, ARTICLE_URL, true);
    psp_ui_show_commit_status(&ui, "PAGE READY", 120u, NULL, 480u,
                              PSP_UI_COMMIT_NOTICE_BASIC_DEFAULT);
    CHECK(ui.toast_kind == PSP_UI_TOAST_BASIC_DEFAULT
          && strcmp(ui.status, reason) == 0);
    intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action == PSP_UI_ACTION_TRY_FULL_PAGE);
    /* A later page of the site: the same words, no buttons. */
    page_ui(&ui, ARTICLE_URL, true);
    psp_ui_show_commit_status(&ui, "PAGE READY", 120u, NULL, 480u,
                              PSP_UI_COMMIT_NOTICE_BASIC_DEFAULT_PASSIVE);
    CHECK(ui.toast_kind == PSP_UI_TOAST_PLAIN && ui.toast_frames != 0
          && strcmp(ui.status, reason) == 0);
    intent = psp_ui_update(&ui, &cross);
    CHECK(intent.action != PSP_UI_ACTION_TRY_FULL_PAGE
          && strcmp(ui.status, reason) == 0);
    /* The bot-wall notice wins, and its keep-alive renews it. */
    page_ui(&ui, ARTICLE_URL, true);
    psp_ui_show_commit_status(&ui, "PAGE READY", 120u, "imdb.com", 480u,
                              PSP_UI_COMMIT_NOTICE_BASIC_DEFAULT);
    CHECK(strcmp(ui.status, "PAGE READY") != 0
          && strcmp(ui.status, reason) != 0
          && ui.toast_kind == PSP_UI_TOAST_PLAIN);
    CHECK(psp_ui_keep_site_blocked_status(&ui, "imdb.com", 480u));
    /* Nothing raised: the ready status. */
    page_ui(&ui, ARTICLE_URL, true);
    psp_ui_show_commit_status(&ui, "PAGE READY", 120u, "", 480u,
                              PSP_UI_COMMIT_NOTICE_NONE);
    CHECK(strcmp(ui.status, "PAGE READY") == 0);
    psp_ui_show_commit_status(NULL, "PAGE READY", 120u, NULL, 480u,
                              PSP_UI_COMMIT_NOTICE_NONE);
    return 0;
}

int main(void)
{
    if (test_engine_stop_reason() != 0) return 1;
    if (test_offer_flow() != 0) return 1;
    if (test_offer_toast() != 0) return 1;
    if (test_offer_ignored_for_session() != 0) return 1;
    if (test_low_memory_needs_a_refusal() != 0) return 1;
    if (test_basic_default_flow() != 0) return 1;
    if (test_basic_default_guards() != 0) return 1;
    if (test_basic_default_notice() != 0) return 1;
    puts("script memory offer: ok");
    return 0;
}
