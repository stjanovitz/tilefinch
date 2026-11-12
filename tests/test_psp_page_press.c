/* What a Cross press on the page ends in. psp_app_page_press.inc is the file
   the PSP frontend compiles; only psp_app_activate_page below stands in for
   the frontend's (test_psp_sdk_contracts.py checks what the real one hands
   over). The engine, its refusal and the status bar state are the real ones. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/controller.h"
#include "tilefinch/navigation.h"
#include "tilefinch/psp_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

typedef struct { BrowserEngine *engine; } TestBrowser;
typedef struct { const BrowserController *controller; } TestViews;
typedef struct { struct { PspUiState ui; } presentation; } TestProcess;
typedef struct {
    TestBrowser *browser;
    TestViews *views;
    TestProcess *process;
} PspApp;
typedef struct { bool page_dirty; } PspAppFrameState;
typedef struct {
    ControllerAction action;
    bool activated;
    bool preview;
    size_t activations_before;
} PspPageActivation;

static unsigned routed;
static ControllerActionType routed_type;

static bool psp_app_activate_page(
    PspApp *app, PspAppFrameState *frame, bool enter_only,
    const uint16_t *engine_frame, PspPageActivation *activation)
{
    (void) frame; (void) enter_only; (void) engine_frame;
    activation->action.form_not_sent = false;
    activation->action.page_scripts_stopped = false;
    activation->preview = false;
    activation->activations_before = app->views->controller->activations;
    activation->activated =
        browser_engine_activate(app->browser->engine, &activation->action);
    return true;
}

static void psp_app_route_controller_action(
    PspApp *app, PspAppFrameState *frame, const uint16_t *engine_frame,
    ControllerAction *action)
{
    (void) app; (void) frame; (void) engine_frame;
    routed++;
    routed_type = action->type;
}

void psp_app_dispatch_activation(
    PspApp *app, PspAppFrameState *frame, bool enter_only,
    const uint16_t *engine_frame);

#include "../src/psp_app/psp_app_page_press.inc"

typedef struct {
    TestBrowser browser;
    TestViews views;
    TestProcess process;
    PspApp app;
    PspAppFrameState frame;
} Press;

static BrowserEngine *page_engine(bool javascript, const char *html,
                                  size_t length)
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
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (engine == NULL) return NULL;
    /* As the PSP frontend runs: a page script's navigation waits for it. */
    browser_engine_set_defer_script_navigation(engine, true);
    if (!browser_engine_commit_html(
            engine, "https://form.test/", html, length, true)) {
        browser_engine_destroy(engine);
        return NULL;
    }
    return engine;
}

/* One press on ENGINE's first control with the UI in a clean state. */
static void press(Press *state, BrowserEngine *engine)
{
    memset(state, 0, sizeof(*state));
    state->browser.engine = engine;
    state->views.controller = browser_engine_controller_view(engine);
    state->app = (PspApp) {&state->browser, &state->views, &state->process};
    routed = 0;
    routed_type = CONTROLLER_ACTION_NONE;
    (void) browser_engine_focus_move(engine, true);
    psp_app_dispatch_activation(&state->app, &state->frame, false, NULL);
}

/* A form page whose one hidden value is FILL bytes long. */
static char *form_page(size_t fill, size_t *length)
{
    static const char head[] =
        "<!doctype html><title>Form</title>"
        "<form method=post action='/step'><input type=hidden name=v value='";
    static const char tail[] = "'><input type=submit name=go value=Go></form>";
    char *page = malloc(sizeof(head) + fill + sizeof(tail));
    if (page == NULL) return NULL;
    size_t used = (size_t) sprintf(page, "%s", head);
    memset(page + used, 'V', fill);
    used += fill;
    used += (size_t) sprintf(page + used, "%s", tail);
    *length = used;
    return page;
}

/* The press was refused and nothing went out: the user is told, and focus
   stays on the button (no move to an unrelated control). */
static int test_refused_form_shows_the_status(void)
{
    size_t length = 0;
    char *html = form_page(NAVIGATION_FORM_BODY_LIMIT + 6000u, &length);
    CHECK(html != NULL);
    BrowserEngine *engine = page_engine(false, html, length);
    free(html);
    CHECK(engine != NULL);
    Press state;
    press(&state, engine);
    if (strcmp(state.process.presentation.ui.status,
               "FORM COULD NOT BE SENT") != 0)
        fprintf(stderr, "refused form status='%s' routed=%u\n",
                state.process.presentation.ui.status, routed);
    CHECK(routed == 0);
    CHECK(strcmp(state.process.presentation.ui.status,
                 "FORM COULD NOT BE SENT") == 0);
    CHECK(state.process.presentation.ui.toast_frames > 0u);
    CHECK(!state.frame.page_dirty);
    browser_engine_destroy(engine);
    return 0;
}

/* The way a sign-in page's token gets too long: a submit listener fills a
   hidden field, so the refusal comes from the page's own entry list. */
static int test_form_refused_after_scripts_shows_the_status(void)
{
    char html[512];
    int length = snprintf(html, sizeof(html),
        "<!doctype html><title>Form</title>"
        "<form method=post action='/step' id=f>"
        "<input type=hidden id=t name=t value=x>"
        "<input type=submit name=go value=Go></form><script>"
        "document.addEventListener('submit',function(){"
        "document.getElementById('t').value='T'.repeat(%u)});</script>",
        NAVIGATION_FORM_BODY_LIMIT + 6000u);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    BrowserEngine *engine = page_engine(true, html, (size_t) length);
    CHECK(engine != NULL);
    Press state;
    press(&state, engine);
    if (routed != 0 || strcmp(state.process.presentation.ui.status,
                              "FORM COULD NOT BE SENT") != 0)
        fprintf(stderr, "scripted refusal status='%s' routed=%u\n",
                state.process.presentation.ui.status, routed);
    CHECK(routed == 0);
    CHECK(strcmp(state.process.presentation.ui.status,
                 "FORM COULD NOT BE SENT") == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* The same list from form.submit() is refused too: never the stale value. */
static int test_page_script_submit_refuses_an_oversized_list(void)
{
    char html[512];
    int length = snprintf(html, sizeof(html),
        "<!doctype html><title>Form</title>"
        "<form method=post action='/step' id=f>"
        "<input type=hidden id=t name=t value=x></form><script>"
        "document.getElementById('t').value='T'.repeat(%u);"
        "document.getElementById('f').submit();</script>",
        NAVIGATION_FORM_BODY_LIMIT + 6000u);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    BrowserEngine *engine = page_engine(true, html, (size_t) length);
    CHECK(engine != NULL);
    ControllerAction action = {0};
    bool taken = browser_engine_take_script_form_submission(engine, &action);
    if (taken)
        fprintf(stderr, "script submit sent body=%.40s\n",
                controller_action_body(&action));
    CHECK(!taken && action.form_not_sent);
    browser_engine_destroy(engine);
    return 0;
}

/* A form that fits goes to the router and says nothing. */
static int test_accepted_form_routes_without_a_status(void)
{
    size_t length = 0;
    char *html = form_page(6000u, &length);
    CHECK(html != NULL);
    BrowserEngine *engine = page_engine(false, html, length);
    free(html);
    CHECK(engine != NULL);
    Press state;
    press(&state, engine);
    CHECK(routed == 1u && routed_type == CONTROLLER_ACTION_FORM_SUBMIT);
    CHECK(state.process.presentation.ui.status[0] == '\0');
    browser_engine_destroy(engine);
    return 0;
}

/* Nothing to press: the older outcome, not a claim about a form. */
static int test_missing_target_is_not_reported_as_a_form(void)
{
    static const char html[] = "<!doctype html><title>Text</title><p>Text</p>";
    BrowserEngine *engine = page_engine(false, html, sizeof(html) - 1u);
    CHECK(engine != NULL);
    Press state;
    press(&state, engine);
    CHECK(routed == 0);
    CHECK(strcmp(state.process.presentation.ui.status,
                 "PAGE ITEM IS NO LONGER AVAILABLE") == 0);
    browser_engine_destroy(engine);
    return 0;
}

#define STOPPED_STATUS "PAGE SCRIPTS STOPPED - RELOAD TO RETRY"

/* Issue 4's shape: the button's work is a click listener, and the page's
   scripts were stopped. Cross reaches the router as before, and the user is
   told why nothing happened. JavaScript turned off by the user, or scripts
   still running, keep the press silent. */
static int stopped_press(bool javascript, bool stop, const char *control,
                         bool expect_status)
{
    char html[512];
    int length = snprintf(html, sizeof(html),
        "<!doctype html><title>Next</title>%s<script>"
        "document.getElementById('b').addEventListener('click',function(){"
        "document.title='clicked'});</script>", control);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    BrowserEngine *engine = page_engine(javascript, html, (size_t) length);
    CHECK(engine != NULL);
    if (stop) CHECK(browser_engine_stop_page_scripts(engine));
    Press state;
    press(&state, engine);
    const char *status = state.process.presentation.ui.status;
    bool shown = strcmp(status, STOPPED_STATUS) == 0;
    if (shown != expect_status)
        fprintf(stderr, "stopped press js=%d stop=%d '%s' status='%s'\n",
                javascript, stop, control, status);
    CHECK(shown == expect_status);
    CHECK(routed == 1u && routed_type == CONTROLLER_ACTION_CONTROL);
    browser_engine_destroy(engine);
    return 0;
}

static int test_press_on_a_stopped_page_says_why(void)
{
    static const char button[] = "<button type=button id=b>Next</button>";
    CHECK(stopped_press(true, true, button, true) == 0);
    CHECK(stopped_press(true, true,
        "<input type=button id=b value=Next>", true) == 0);
    CHECK(stopped_press(true, true,
        "<a id=b href='javascript:void(0)'>Next</a>", true) == 0);
    /* Not a script's job: the checkbox still toggles natively. */
    CHECK(stopped_press(true, true, "<input type=checkbox id=b>", false) == 0);
    CHECK(stopped_press(false, false, button, false) == 0);
    CHECK(stopped_press(true, false, button, false) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_press_on_a_stopped_page_says_why() == 0);
    CHECK(test_refused_form_shows_the_status() == 0);
    CHECK(test_form_refused_after_scripts_shows_the_status() == 0);
    CHECK(test_page_script_submit_refuses_an_oversized_list() == 0);
    CHECK(test_accepted_form_routes_without_a_status() == 0);
    CHECK(test_missing_target_is_not_reported_as_a_form() == 0);
    puts("psp page press tests passed");
    return 0;
}
