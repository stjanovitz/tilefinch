#include "tilefinch/psp_tabs.h"
#include "tilefinch/captive_portal.h"
#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "tab lifecycle: %d: %s\n", __LINE__, #value); return 1; \
} } while (0)

static int test_native_surface_capture(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    CHECK(psp_tabs_document_visible(&ui));
    psp_ui_show_home(&ui);
    CHECK(!psp_tabs_document_visible(&ui));
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    CHECK(!psp_tabs_document_visible(&ui));
    psp_ui_show_collections(&ui, PSP_UI_COLLECTION_BOOKMARKS);
    CHECK(!psp_tabs_document_visible(&ui));
    psp_ui_leave_native_surface(&ui);
    CHECK(psp_tabs_document_visible(&ui));
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    CHECK(psp_tabs_document_visible(&ui));
    ui.captive_portal_active = true;
    CHECK(!psp_tabs_document_visible(&ui));
    CHECK(!psp_tabs_document_visible(NULL));
    return 0;
}

static int test_retained_tab_selection_and_failure(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * 1024u * 1024u;
    config.javascript.enabled = false;
    config.resources.enabled = false;
    config.fonts.enabled = false;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (!engine) fprintf(stderr, "tab engine: %s\n", error);
    CHECK(engine != NULL);
    static const char original[] = "https://ordinary.example/article";
    static const char html[] = "<!doctype html><title>Article</title>"
        "<a href='/next'>Next</a><div style='height:1600px'>Readable page</div>";
    CHECK(browser_engine_commit_html(engine, original, html, sizeof(html) - 1u, true));
    CHECK(browser_engine_focus_move(engine, true));
    CHECK(browser_engine_restore_view(engine, 120, CONTROLLER_FOCUS_LINK, 0));
    BrowserFrontendSnapshot view;
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    Budget *budget = browser_engine_budget(engine);
    BrowserTabs *tabs = browser_tabs_create(budget, view.navigation);
    CHECK(tabs != NULL && browser_tabs_count(tabs) == 1);
    CHECK(browser_tabs_set_active_find_query(tabs, "Readable"));
    static const char closed[] = "<!doctype html><title>Closed</title>";
    CHECK(browser_engine_commit_html(engine, TILEFINCH_HOMEPAGE_URL,
        closed, sizeof(closed) - 1u, false));
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    PspUiState ui;
    psp_ui_init(&ui);
    /* A failed return retires the ephemeral tab but leaves its closing
       document live. Home and its menus must not capture it on exit. */
    size_t portal = 0;
    CHECK(browser_tabs_add(tabs, "http://portal.example/sign-in", "Temporary", &portal));
    CHECK(!tilefinch_captive_portal_restore_tabs(tabs,
        PSP_CAPTIVE_PORTAL_RETURNING, portal, 0));
    psp_ui_show_home(&ui);
    ui.screen = PSP_UI_SCREEN_OPTIONS;
    if (psp_tabs_document_visible(&ui))
        CHECK(browser_tabs_capture_active(tabs, view.navigation));
    CHECK(browser_tabs_count(tabs) == 1
        && !strcmp(browser_tabs_active(tabs)->history[0].url, original));
    char destination[NAVIGATION_URL_LIMIT] = {0};
    PspTabTransition transition = {0};
    /* Home displays retained facts, not the engine's empty backing document.
       Opening its already-selected tab must navigate, not report a no-op. */
    CHECK(psp_tabs_request(tabs, view.navigation, false, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_LOAD);
    CHECK(transition.pending && !strcmp(destination, original));
    PspTabTransition retained = transition;
    CHECK(psp_tabs_request(tabs, view.navigation, false, PSP_UI_ACTION_NEW_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_REFUSED);
    CHECK(!memcmp(&transition, &retained, sizeof(transition)));
    CHECK(psp_tabs_finish(tabs, engine, &transition, false, false, NULL));
    CHECK(!transition.pending && browser_tabs_count(tabs) == 1
          && !strcmp(browser_tabs_active(tabs)->history[0].url, original));
    CHECK(!psp_tabs_finish(tabs, engine, &transition, false, false, NULL));
    CHECK(psp_tabs_request(tabs, view.navigation, false, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_LOAD);
    CHECK(browser_engine_commit_html(engine, original, html, sizeof(html) - 1u, false));
    CHECK(psp_tabs_finish(tabs, engine, &transition, true, false, NULL));
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    int scroll_y = 0;
    BrowserHistoryEntryText current;
    CHECK(browser_engine_current_entry_text(engine, &current, &scroll_y));
    CHECK(!transition.pending && scroll_y == 120
          && view.controller->focus_kind == CONTROLLER_FOCUS_LINK
          && view.controller->focus_index == 0
          && !strcmp(browser_tabs_active_find_query(tabs), "Readable"));
    CHECK(psp_tabs_request(tabs, view.navigation, true, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_COMPLETE && !transition.pending);
    /* The refreshed page can be shorter and insert a link before the one
       the user selected. Keep semantic focus and do not leave hit testing
       below the page the renderer can actually display. */
    CHECK(browser_tabs_active(tabs)->history[0].focus_identity != 0);
    CHECK(psp_tabs_request(tabs, view.navigation, false, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_LOAD);
    static const char changed[] = "<!doctype html><a href='/new'>New</a><br>"
        "<a href='/next'>Next</a>";
    CHECK(browser_engine_commit_html(engine, original, changed, sizeof(changed) - 1u, false));
    CHECK(psp_tabs_finish(tabs, engine, &transition, true, false, NULL));
    CHECK(browser_engine_frontend_snapshot(engine, &view)
          && browser_engine_current_entry_text(engine, &current, &scroll_y)
          && scroll_y == 0 && view.controller->focus_kind == CONTROLLER_FOCUS_LINK
          && view.controller->focus_index == 1);
    const LinkRegion *first = &view.navigation->page.layout.links[0];
    const ViewportContext *viewport = &view.navigation->viewport;
    CHECK(browser_engine_focus_at(engine,
        viewport_css_to_device(viewport, first->x + 1),
        viewport_css_to_device(viewport, first->y + 1)));
    browser_tabs_destroy(tabs);
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    return 0;
}

static int test_session_storage_follows_tab_identity(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 32u * 1024u * 1024u;
    config.javascript.enabled = true;
    config.resources.enabled = false;
    config.fonts.enabled = false;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char url[] = "https://tabs.test/first";
    static const char first[] = "<!doctype html><script>"
        "sessionStorage.setItem('tab','first');localStorage.setItem('shared','yes')"
        "</script>";
    static const char next[] = "<!doctype html><title>Second</title>";
    CHECK(browser_engine_commit_html(engine, url, first, sizeof(first)-1, true));
    BrowserFrontendSnapshot view;
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    Budget *budget = browser_engine_budget(engine);
    BrowserTabs *tabs = browser_tabs_create(budget, view.navigation);
    CHECK(tabs != NULL);
    char hibernation[128];
    snprintf(hibernation, sizeof(hibernation),
             "/tmp/tilefinch-tab-storage-%ld", (long) getpid());
    (void) unlink(hibernation);
    char destination[NAVIGATION_URL_LIMIT] = {0};
    PspTabTransition transition = {0};
    for (unsigned pass = 0; pass < 3; pass++) {
        CHECK(browser_engine_frontend_snapshot(engine, &view));
        CHECK(psp_tabs_request(tabs, view.navigation, true,
              pass == 1 ? PSP_UI_ACTION_CLOSE_TAB : PSP_UI_ACTION_NEW_TAB,
              1, "https://tabs.test/next", hibernation, &transition, destination)
              == PSP_TAB_REQUEST_LOAD);
        if (pass == 0) {
            NavigationSession *incumbent = browser_engine_navigation(engine);
            CHECK(script_runtime_evaluate_diagnostic(
                incumbent->page.runtime, "pocSummary=sessionStorage.getItem('tab')",
                "<incumbent-tab-storage>", &incumbent->page.script_result)
                && strcmp(incumbent->page.script_result.summary, "first") == 0);
        }
        CHECK(browser_engine_commit_html(engine, destination, next, sizeof(next)-1, false)
              && psp_tabs_finish(tabs, engine, &transition, true, pass == 0, hibernation));
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(nav->page.runtime && script_runtime_evaluate_diagnostic(
            nav->page.runtime, pass == 1
              ? "pocSummary=sessionStorage.getItem('tab')+':'+sessionStorage.length"
              : "pocSummary=sessionStorage.getItem('tab')+':'+sessionStorage.length"
                "+':'+localStorage.getItem('shared');sessionStorage.clear();"
                "sessionStorage.setItem('tab','second')",
            "<tab-storage>", &nav->page.script_result));
        CHECK(strcmp(nav->page.script_result.summary,
                     pass == 1 ? "first:1" : "null:0:yes") == 0);
        CHECK(browser_tabs_hibernated(tabs, 0) == (pass == 0));
    }
    (void) unlink(hibernation);
    browser_tabs_destroy(tabs);
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    return 0;
}

static int test_committed_tab_restore_boundaries(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * 1024u * 1024u;
    config.javascript.enabled = config.resources.enabled = config.fonts.enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, NULL, 0);
    CHECK(engine != NULL);
    static const char html[] = "<title>Test</title><p>Readable</p>";
    static const char first[] = "https://tabs.example/a";
    static const char second[] = "https://tabs.example/b";
    static const char redirected[] = "https://tabs.example/after-redirect";
    CHECK(browser_engine_commit_html(engine, first, html, sizeof(html) - 1u, true));
    CHECK(browser_engine_commit_html(engine, "https://tabs.example/a2",
                                     html, sizeof(html) - 1u, true));
    BrowserFrontendSnapshot view;
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    Budget *budget = browser_engine_budget(engine);
    BrowserTabs *tabs = browser_tabs_create(budget, view.navigation);
    CHECK(tabs != NULL);
    PspTabTransition transition = {0};
    char destination[NAVIGATION_URL_LIMIT];
    CHECK(psp_tabs_request(tabs, view.navigation, true, PSP_UI_ACTION_NEW_TAB,
          0, second, NULL, &transition, destination) == PSP_TAB_REQUEST_LOAD);
    CHECK(browser_engine_commit_html(engine, second, html, sizeof(html) - 1u, false));
    budget_inject_failure_after(budget, 0);
    CHECK(!psp_tabs_finish(tabs, engine, &transition, true, false, NULL));
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    CHECK(!transition.pending && !browser_engine_history_step(engine, false, NULL, NULL)
          && browser_tabs_capture_active(tabs, view.navigation)
          && !strcmp(browser_tabs_active(tabs)->history[0].url, second)
          && browser_tabs_tab(tabs, 0)->history_count == 2);

    char session[160];
    snprintf(session, sizeof(session), "/tmp/tilefinch-tab-startup-%ld.bin", (long) getpid());
    CHECK(browser_tabs_save_session(tabs, session, NULL));
    browser_tabs_destroy(tabs);
    CHECK(browser_engine_commit_html(engine, redirected, html, sizeof(html) - 1u, true));
    CHECK(browser_engine_frontend_snapshot(engine, &view));
    tabs = browser_tabs_create(budget, view.navigation);
    CHECK(tabs != NULL && psp_tabs_restore_startup(tabs, engine, session));
    CHECK(browser_tabs_count(tabs) == 2 && browser_tabs_active_index(tabs) == 1
        && browser_tabs_tab(tabs, 0)->history_count == 2
        && !strcmp(browser_tabs_tab(tabs, 0)->history[0].url, first)
        && !strcmp(browser_tabs_active(tabs)->history[0].url, redirected));
    CHECK(browser_engine_frontend_snapshot(engine, &view)
          && view.navigation->history_count == 1
          && !strcmp(navigation_current(view.navigation)->url, redirected));
    CHECK(unlink(session) == 0);
    browser_tabs_destroy(tabs);
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* A refused allocation while replacing the other tab's history with the
   committed page must not leave the engine with no history entry. */
static int test_recovered_history_keeps_an_entry(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * 1024u * 1024u;
    config.javascript.enabled = config.resources.enabled = config.fonts.enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, NULL, 0);
    CHECK(engine != NULL);
    static const char html[] = "<title>Test</title><p>Readable</p>";
    CHECK(browser_engine_commit_html(engine, "https://tabs.example/a",
                                     html, sizeof(html) - 1u, true)
          && browser_engine_commit_html(engine, "https://tabs.example/b",
                                        html, sizeof(html) - 1u, true)
          && browser_engine_commit_html(engine, "https://tabs.example/new",
                                        html, sizeof(html) - 1u, false));
    BrowserFrontendSnapshot view;
    CHECK(browser_engine_frontend_snapshot(engine, &view)
          && view.navigation->history_count >= 2);
    Budget *budget = browser_engine_budget(engine);
    budget_inject_failure_after(budget, 0);
    browser_engine_recover_tab_history(engine);
    budget_clear_failure_injection(budget);
    CHECK(browser_engine_frontend_snapshot(engine, &view)
          && view.navigation->history_count == 1
          && view.navigation->history_index == 0
          && !strcmp(navigation_current(view.navigation)->url,
                     "https://tabs.example/new"));
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selection-only"))
        return test_retained_tab_selection_and_failure();
    if (test_native_surface_capture() || test_retained_tab_selection_and_failure()
        || test_session_storage_follows_tab_identity()
        || test_committed_tab_restore_boundaries()
        || test_recovered_history_keeps_an_entry()) return 1;
    puts("tab lifecycle tests passed");
    return 0;
}
