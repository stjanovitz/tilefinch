#include "tilefinch/psp_tabs.h"
#include "tilefinch/captive_portal.h"
#include "tilefinch/platform.h"
#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    CHECK(psp_tabs_request(tabs, engine, false, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_LOAD);
    CHECK(transition.pending && !strcmp(destination, original));
    PspTabTransition retained = transition;
    CHECK(psp_tabs_request(tabs, engine, false, PSP_UI_ACTION_NEW_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_REFUSED);
    CHECK(!memcmp(&transition, &retained, sizeof(transition)));
    CHECK(psp_tabs_finish(tabs, engine, &transition, false, false, NULL));
    CHECK(!transition.pending && browser_tabs_count(tabs) == 1
          && !strcmp(browser_tabs_active(tabs)->history[0].url, original));
    CHECK(!psp_tabs_finish(tabs, engine, &transition, false, false, NULL));
    CHECK(psp_tabs_request(tabs, engine, false, PSP_UI_ACTION_SWITCH_TAB,
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
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB,
        0, TILEFINCH_HOMEPAGE_URL, NULL, &transition, destination)
        == PSP_TAB_REQUEST_COMPLETE && !transition.pending);
    /* The refreshed page can be shorter and insert a link before the one
       the user selected. Keep semantic focus and do not leave hit testing
       below the page the renderer can actually display. */
    CHECK(browser_tabs_active(tabs)->history[0].focus_identity != 0);
    CHECK(psp_tabs_request(tabs, engine, false, PSP_UI_ACTION_SWITCH_TAB,
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
        "</script><input id=untouched value=default>"
        "<select id=choice><option>First</option><option>Second</option></select>"
        "<details><summary>More</summary><p>Untouched</p></details>";
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
        CHECK(psp_tabs_request(tabs, engine, true,
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
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_NEW_TAB,
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
    CHECK(tabs != NULL && psp_tabs_unread_notice(NULL, tabs) == NULL);
    /* An unreadable session gets the one boot notice. */
    CHECK(chmod(session, 0) == 0);
    bool unread_restored = psp_tabs_restore_startup(tabs, engine, session);
    CHECK(chmod(session, 0600) == 0);
    CHECK(!unread_restored
          && !strcmp(psp_tabs_unread_notice(NULL, tabs),
                     PSP_TABS_SESSION_UNREAD_NOTICE));
    CHECK(psp_tabs_restore_startup(tabs, engine, session)
          && psp_tabs_unread_notice(NULL, tabs) == NULL);
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

static bool focus_control_id(BrowserEngine *engine, const char *id)
{
    const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
    size_t length = strlen(id);
    for (size_t i = 0; i < layout->control_count; i++) {
        lxb_dom_node_t *node = layout->controls[i].node;
        size_t actual_length = 0;
        const char *actual = document_attribute(node, "id", &actual_length);
        if (actual != NULL && length == actual_length && !memcmp(actual, id, length))
            return browser_engine_focus_node(engine, node);
    }
    return false;
}

static bool focus_note_form(BrowserEngine *engine, const char *action)
{
    const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
    size_t length = strlen(action);
    for (size_t i = 0; i < layout->control_count; i++) {
        lxb_dom_node_t *node = layout->controls[i].node;
        size_t name_length = 0, action_length = 0;
        const char *name = document_attribute(node, "name", &name_length);
        if (name == NULL || name_length != 4u || memcmp(name, "note", 4u)) continue;
        const char *actual = document_attribute(
            controller_control_form_owner(node), "action", &action_length);
        if (actual != NULL && action_length == length && !memcmp(actual, action, length))
            return browser_engine_focus_node(engine, node);
    }
    return false;
}

static bool draft_replay_begin(char directory[], const char *method,
                               const char *url, const char *sent, const char *html)
{
    if (!mkdtemp(directory)) return false;
    char path[128];
    size_t html_length = strlen(html), sent_length = strlen(sent);
    uint64_t sent_hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sent_length; i++)
        sent_hash = (sent_hash ^ (unsigned char) sent[i]) * UINT64_C(1099511628211);
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    FILE *file = fopen(path, "wb");
    if (file == NULL || fwrite(html, 1, html_length, file) != html_length
        || fclose(file) != 0) return false;
    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    file = fopen(path, "wb");
    if (file == NULL || fprintf(file,
        "psp-http-trace=1\nmethod=%s\nurl=%s\nsuccess=1\n"
        "request-body-length=%zu\nrequest-body-hash=%016llx\n"
        "async-delay-pumps=0\nexternal-cancel=0\ntransport-timeout=0\n"
        "error=\nstatus=200\nlength=%zu\neffective-url=%s\n"
        "content-type=text/html\netag=\nlast-modified=\ncf-mitigated=\n"
        "accept-ch=\ncritical-ch=\nserver=fixture\ncf-ray=\nset-cookie-count=0\n",
        method, url, sent_length, (unsigned long long) sent_hash, html_length, url) <= 0
        || fclose(file) != 0) return false;
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    file = fopen(path, "wb");
    char error[128] = {0};
    return file != NULL && fputs("psp-http-trace-clock=1\norigin-ms=1700000000000\n"
        "capture-complete=yes\nrecord-count=1\n", file) >= 0 && fclose(file) == 0
        && fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error));
}

static bool draft_replay_end(const char *directory)
{
    FetchTraceReplayStats stats = {0};
    bool once = fetch_trace_replay_stats(&stats) && stats.request_count == 1u
        && stats.matched_request_count == 1u && stats.served_request_count == 1u;
    fetch_trace_end();
    char path[128];
    static const char *const files[] = {"0000.body", "0000.meta", "trace.meta"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        (void) unlink(path);
    }
    (void) rmdir(directory);
    return once;
}

static bool draft_navigation_succeeds(BrowserEngine *engine)
{
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t i = 0; i < 1000u && status == BROWSER_NAVIGATION_JOB_PENDING; i++)
        status = browser_engine_pump_navigation(engine, NULL);
    return status == BROWSER_NAVIGATION_JOB_SUCCEEDED;
}

typedef struct { Budget *budget; bool reached; } DraftPressure;

static bool draft_pressure(void *opaque, const char *phase, size_t work)
{
    (void) work;
    DraftPressure *pressure = opaque;
    if (!pressure->reached && !strcmp(phase, "navigation-eof")) {
        pressure->reached = true;
        pressure->budget->limit = pressure->budget->current;
        budget_inject_failure_after(pressure->budget, 0);
    }
    return true;
}

typedef struct { unsigned asks; bool leave; } DraftDecision;
static bool confirm_loss(void *context)
{
    DraftDecision *choice = context;
    choice->asks++;
    return choice->leave;
}

static int test_tab_draft_stays_with_its_form(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * 1024u * 1024u;
    config.javascript.enabled = config.resources.enabled = config.fonts.enabled = false;
    config.navigation_replacement_mode = NAVIGATION_REPLACEMENT_LOW_MEMORY;
    BrowserEngine *engine = browser_engine_create(&config, NULL, 0);
    CHECK(engine != NULL);
    static const char first[] = "https://tabs.example/draft";
    static const char second[] = "https://tabs.example/second";
    static const char form[] = "<form method=post action='/send'>"
        "<input name=note><input id=secret type=password>"
        "<button id=send>Send</button></form>"
        "<form method=post action='/other-form'><input name=note value=untouched></form>"
        "<a href='/next'>Next chapter</a>";
    static const char updated[] = "<form method=post action='/other-form'>"
        "<input name=note value=untouched></form>"
        "<form method=post action='/send'><input name=note>"
        "<input id=secret type=password><button id=send>Send</button></form>"
        "<a href='/next'>Next chapter</a>";
    static const char other[] = "<title>Other tab</title><p>Other tab</p>";
    CHECK(browser_engine_commit_html(engine, "https://tabs.example/start",
              other, sizeof(other) - 1u, true)
          && browser_engine_commit_html(engine, first, form, sizeof(form) - 1u, true));
    Budget *budget = browser_engine_budget(engine);
    BrowserTabs *tabs = browser_tabs_create(budget, browser_engine_navigation(engine));
    CHECK(tabs != NULL && focus_control_id(engine, "secret")
          && browser_engine_replace_text(engine, "private", 7u)
          && focus_note_form(engine, "/send"));
    PspTabTransition transition = {0};
    char destination[NAVIGATION_URL_LIMIT], value[160], oversized[141];
    memset(oversized, 'x', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';
    CHECK(browser_engine_replace_text(engine, oversized, sizeof(oversized) - 1u)
          && psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_NEW_TAB, 0, second,
                NULL, &transition, destination) == PSP_TAB_REQUEST_CAPTURE_FAILED
          && !transition.pending && browser_tabs_count(tabs) == 1u
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, oversized));
    CHECK(!browser_engine_begin_navigation_history(engine, false, 4096, 1000)
          && strstr(browser_engine_last_error(engine), "page kept open") != NULL
          && !strcmp(navigation_current(browser_engine_navigation(engine))->url, first));
    ControllerAction action = {0};
    const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
    CHECK(layout->link_count != 0u
          && browser_engine_focus_node(engine, layout->links[0].node)
          && browser_engine_activate(engine, &action)
          && !browser_engine_begin_navigation_action(engine, &action, 4096, 1000)
          && strstr(browser_engine_last_error(engine), "page kept open") != NULL
          && focus_note_form(engine, "/send")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, oversized)
          && browser_engine_replace_text(engine, "long draft", 10u));
    budget_inject_failure_after(budget, 0);
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_NEW_TAB, 0,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_CAPTURE_FAILED);
    budget_clear_failure_injection(budget);
    CHECK(!transition.pending && browser_tabs_count(tabs) == 1u
          && browser_tabs_active_index(tabs) == 0u
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "long draft"));

    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_NEW_TAB, 0,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_LOAD
          && !strcmp(destination, second)
          && browser_engine_commit_html(engine, destination, other, sizeof(other) - 1u, false)
          && psp_tabs_finish(tabs, engine, &transition, true, false, NULL));
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB, 0,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_LOAD);
    budget_inject_failure_after(budget, 0);
    CHECK(!browser_engine_commit_html(engine, destination, updated, sizeof(updated) - 1u, false));
    budget_clear_failure_injection(budget);
    CHECK(psp_tabs_finish(tabs, engine, &transition, false, false, NULL)
          && browser_tabs_active_index(tabs) == 1u
          && !strcmp(browser_engine_navigation(engine)->page.document_url, second));

    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB, 0,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_LOAD
          && browser_engine_commit_html(engine, destination, updated, sizeof(updated) - 1u, false)
          && psp_tabs_finish(tabs, engine, &transition, true, false, NULL)
          && focus_note_form(engine, "/other-form")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "untouched")
          && focus_control_id(engine, "secret")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "")
          && focus_note_form(engine, "/send")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "long draft"));
    layout = &browser_engine_navigation(engine)->page.layout;
    CHECK(layout->link_count > 0u);
    CHECK(browser_engine_focus_node(engine, layout->links[0].node));
    CHECK(browser_engine_activate(engine, &action) && action.type == CONTROLLER_ACTION_NAVIGATE);
    char refused_replay[] = "/tmp/tilefinch-draft-pressure-XXXXXX";
    CHECK(draft_replay_begin(refused_replay, "GET", "https://tabs.example/next", "", other));
    NavigationSession *nav = browser_engine_navigation(engine);
    size_t retirements = nav->performance.low_memory_replacements_started;
    size_t original_limit = budget->limit;
    DraftPressure pressure = {.budget = budget};
    TilefinchPlatformServices services = {.context = &pressure, .cooperate = draft_pressure};
    tilefinch_platform_set_services(&services);
    CHECK(browser_engine_begin_navigation_action(engine, &action, 4096, 1000));
    bool loaded = draft_navigation_succeeds(engine);
    budget->limit = original_limit;
    budget_clear_failure_injection(budget);
    tilefinch_platform_set_services(NULL);
    CHECK(pressure.reached && !loaded && draft_replay_end(refused_replay)
          && nav->performance.low_memory_replacements_started == retirements
          && !strcmp(nav->page.document_url, first)
          && focus_note_form(engine, "/send")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "long draft"));
    layout = &nav->page.layout;
    CHECK(browser_engine_focus_node(engine, layout->links[0].node)
          && browser_engine_activate(engine, &action) && action.type == CONTROLLER_ACTION_NAVIGATE);
    char link_replay[] = "/tmp/tilefinch-draft-link-XXXXXX";
    CHECK(draft_replay_begin(link_replay, "GET", "https://tabs.example/next", "", other)
          && browser_engine_begin_navigation_action(engine, &action, 4096, 1000)
          && draft_navigation_succeeds(engine) && draft_replay_end(link_replay));
    char back_replay[] = "/tmp/tilefinch-draft-back-XXXXXX";
    CHECK(draft_replay_begin(back_replay, "GET", first, "", updated)
          && browser_engine_begin_navigation_history(engine, false, 4096, 1000)
          && draft_navigation_succeeds(engine) && draft_replay_end(back_replay)
          && focus_note_form(engine, "/send")
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "long draft"));
    CHECK(focus_control_id(engine, "send") && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && !strcmp(action.method, "POST") && !strcmp(action.url, "https://tabs.example/send")
          && !strcmp(controller_action_body(&action), "note=long+draft"));
    char post_replay[] = "/tmp/tilefinch-draft-post-XXXXXX";
    CHECK(draft_replay_begin(post_replay, "POST", "https://tabs.example/send",
          "note=long+draft", "<title>Sent</title><p>Sent once</p>"));
    budget_inject_failure_after(budget, 0);
    CHECK(!browser_engine_begin_navigation_action(engine, &action, 4096, 1000));
    budget_clear_failure_injection(budget);
    FetchTraceReplayStats before_send = {0};
    CHECK(fetch_trace_replay_stats(&before_send) && before_send.request_count == 0u
          && !strcmp(nav->page.document_url, first)
          /* A bounded pre-transport retry uses the existing action rather
             than dispatching another click or another submit event. */
          && browser_engine_begin_navigation_action(engine, &action, 4096, 1000)
          && draft_navigation_succeeds(engine)
          && !strcmp(browser_engine_navigation(engine)->page.document.title, "Sent")
          && draft_replay_end(post_replay));

    static const char many[] = "<input id=a><input id=b><input id=c>"
        "<input id=d><input id=e><input id=f><input id=g><input id=h><input id=i>";
    CHECK(browser_engine_commit_html(engine, "https://tabs.example/many",
          many, sizeof(many) - 1u, true));
    for (char ch = 'a'; ch <= 'i'; ch++) {
        char id[] = {ch, '\0'};
        CHECK(focus_control_id(engine, id) && browser_engine_replace_text(engine, "edit", 4u));
    }
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB, 1,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_CAPTURE_FAILED
          && browser_tabs_active_index(tabs) == 0u && !transition.pending
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "edit"));
    DraftDecision choice = {0};
    TilefinchPlatformServices dialog = {.context = &choice, .confirm_draft_loss = confirm_loss};
    tilefinch_platform_set_services(&dialog);
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB, 1,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_CAPTURE_FAILED
          && choice.asks == 1 && !transition.pending);
    choice.leave = true;
    CHECK(psp_tabs_request(tabs, engine, true, PSP_UI_ACTION_SWITCH_TAB, 1,
          second, NULL, &transition, destination) == PSP_TAB_REQUEST_LOAD
          && choice.asks == 2 && transition.pending);
    /* Consent applies to this attempt, not to the incumbent: a failed load
       keeps the full draft live and asks again on a new departure. */
    CHECK(psp_tabs_finish(tabs, engine, &transition, false, false, NULL)
          && browser_tabs_active_index(tabs) == 0
          && browser_engine_text_value(engine, value, sizeof(value), NULL)
          && !strcmp(value, "edit"));
    choice.leave = false;
    CHECK(!browser_engine_begin_navigation_history(engine, false, 4096, 1000)
          && choice.asks == 3 && !strcmp(navigation_current(nav)->url, "https://tabs.example/many"));
    choice.leave = true;
    char leave_replay[] = "/tmp/tilefinch-draft-leave-XXXXXX";
    CHECK(draft_replay_begin(leave_replay, "GET", "https://tabs.example/send", "", other)
          && browser_engine_begin_navigation_history(engine, false, 4096, 1000)
          && choice.asks == 4 && draft_navigation_succeeds(engine) && draft_replay_end(leave_replay));
    CHECK(browser_engine_commit_html(engine, first, form, sizeof(form) - 1u, true)
          && focus_note_form(engine, "/send")
          && browser_engine_replace_text(engine, oversized, sizeof(oversized)-1u));
    CHECK(!browser_engine_capture_history_controls(engine) && choice.asks == 4);
    choice.leave = false;
    CHECK(!browser_engine_history_step(engine, false, NULL, NULL)
          && choice.asks == 5 && !strcmp(navigation_current(nav)->url, first));
    layout = &browser_engine_navigation(engine)->page.layout;
    CHECK(layout->link_count && browser_engine_focus_node(engine, layout->links[0].node)
          && browser_engine_activate(engine, &action) && action.type == CONTROLLER_ACTION_NAVIGATE);
    char agreed_link[] = "/tmp/tilefinch-draft-confirm-link-XXXXXX";
    CHECK(draft_replay_begin(agreed_link, "GET", "https://tabs.example/next", "", other));
    choice.leave = false;
    CHECK(!browser_engine_begin_navigation_action(engine, &action, 4096, 1000)
          && choice.asks == 6 && !browser_engine_navigation_pending(engine));
    FetchTraceReplayStats before_leave = {0};
    CHECK(fetch_trace_replay_stats(&before_leave) && before_leave.request_count == 0u);
    choice.leave = true;
    CHECK(browser_engine_begin_navigation_action(engine, &action, 4096, 1000)
          && choice.asks == 7 && draft_navigation_succeeds(engine)
          && draft_replay_end(agreed_link) && !strcmp(navigation_current(nav)->url, action.url));
    tilefinch_platform_set_services(NULL);
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
    if (argc == 2 && !strcmp(argv[1], "--draft-only"))
        return test_tab_draft_stays_with_its_form();
    if (test_native_surface_capture() || test_retained_tab_selection_and_failure()
        || test_session_storage_follows_tab_identity()
        || test_committed_tab_restore_boundaries()
        || test_tab_draft_stays_with_its_form()
        || test_recovered_history_keeps_an_entry()) return 1;
    puts("tab lifecycle tests passed");
    return 0;
}
