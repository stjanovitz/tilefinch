#include "tilefinch/psp_tabs.h"
#include "tilefinch/captive_portal.h"
#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <string.h>

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
    browser_tabs_destroy(tabs);
    CHECK(browser_engine_shutdown(engine) && budget->current == 0);
    browser_engine_destroy(engine);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selection-only"))
        return test_retained_tab_selection_and_failure();
    if (test_native_surface_capture() || test_retained_tab_selection_and_failure()) return 1;
    puts("tab lifecycle tests passed");
    return 0;
}
