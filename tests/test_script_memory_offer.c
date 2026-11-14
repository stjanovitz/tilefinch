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
    "<p><a href='/world/article-2'>Next article</a></p></main>"
    "<script>globalThis.store=[];for(let i=0;i<60000;i++)"
    "store.push({id:i,text:'hydration-record-'+i,tags:[i,i+1]});</script>";
static const char plain_script[] =
    "<!doctype html><title>Small</title><main><h1>Small page</h1>"
    "<p>Its script is tiny.</p></main>"
    "<script>document.title='Small page';</script>";

static BrowserEngine *create_engine(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.tile_capacity = 4u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 3u * MIB;
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

static int test_engine_stop_reason(void)
{
    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    /* The article's realm runs out of memory; its page stays. */
    CHECK(browser_engine_commit_html(engine, ARTICLE_URL, article,
                                     sizeof(article) - 1u, true));
    CHECK(browser_engine_page_loaded(engine));
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
   admission headroom) is not warned; a page whose allocation the heap
   refused, or whose script was skipped for memory, is. The memory-stop
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

    /* An allocation the heap refused (caught by the page). */
    static const char refused[] =
        "<!doctype html><title>Refused</title><main><h1>Refused</h1>"
        "<p>Its script asks for more than the heap has.</p></main>"
        "<script>try{globalThis.big=new ArrayBuffer(16*1024*1024)}"
        "catch(e){globalThis.big=null}</script>";
    CHECK(browser_engine_commit_html(engine, NEXT_URL, refused,
                                     sizeof(refused) - 1u, true));
    CHECK(browser_engine_heavy_page(engine, &state));
    if (state.memory_refusals == 0)
        fprintf(stderr, "refused: free=%zu refusals=%zu\n",
                state.memory_free, state.memory_refusals);
    CHECK(state.memory_refusals != 0);
    CHECK(script_heavy_state_low_on_memory(&state));

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

int main(void)
{
    if (test_engine_stop_reason() != 0) return 1;
    if (test_offer_flow() != 0) return 1;
    if (test_offer_toast() != 0) return 1;
    if (test_offer_ignored_for_session() != 0) return 1;
    if (test_low_memory_needs_a_refusal() != 0) return 1;
    puts("script memory offer: ok");
    return 0;
}
