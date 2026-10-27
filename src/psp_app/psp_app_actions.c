/* Executes PspUiIntent commands without reaching into the UI renderer's
 * internals. PspApp supplies borrowed canonical owners; PspAppFrameState
 * supplies values sampled for the current loop iteration.
 */
#include "psp_app_internal.h"
/* Page tools -> Basic view, and the recovery prompt's "Show Basic view"
   (require_enable: never toggles an active Basic view off). */
__attribute__((noinline))
static void psp_app_toggle_basic_view(
    PspApp *app, PspAppFrameState *frame, bool require_enable)
{
    BrowserEngine *engine = app->browser->engine;
    if (require_enable && app->process->presentation.ui.basic_mode) {
        psp_ui_show_status(
            &app->process->presentation.ui, "BASIC VIEW ON", 180);
        return;
    }
    char basic_buffer[NAVIGATION_URL_LIMIT];
    const char *basic_url = psp_app_current_url(
        engine, basic_buffer, sizeof(basic_buffer));
    if (app->process->presentation.ui.reader_mode) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "TURN READER MODE OFF BEFORE BASIC VIEW", 180);
        return;
    }
    bool old_mode = app->process->presentation.ui.basic_mode;
    bool enable = !old_mode;
    char basic_anchor[BROWSER_TEXT_ANCHOR_LIMIT + 1u];
    int basic_anchor_y = 0;
    bool have_basic_anchor = browser_engine_capture_text_anchor(
        engine, basic_anchor, &basic_anchor_y);
    ReaderDocumentAnalysis basic_analysis = {0};
    if (enable && !browser_engine_prepare_basic_view(
            engine, &basic_analysis)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            basic_analysis.prepared
                && basic_analysis.kind != READER_PAGE_BASIC
                ? "RELOAD PAGE TO SWITCH EXTRACTED VIEWS"
                : "BASIC VIEW UNAVAILABLE",
            240);
        return;
    }
    if (!psp_app_set_basic_view(app, basic_url, enable,
                                frame == NULL ? NULL : &frame->page_dirty)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            enable ? "BASIC VIEW UNAVAILABLE"
                   : "BASIC VIEW COULD NOT CLOSE",
            240);
        return;
    }
    if (have_basic_anchor)
        (void) browser_engine_restore_text_anchor(
            engine, basic_anchor, basic_anchor_y);
    psp_ui_show_status(
        &app->process->presentation.ui,
        enable ? "BASIC VIEW ON" : "BASIC VIEW OFF", 180);
}

__attribute__((noinline))
static void psp_app_dispatch_heavy_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent);

/* Offers a file download for url (NULL: nothing to offer): the "Save
   file?" sheet when the store accepts it, otherwise the failure toast. */
void psp_app_offer_file(PspApp *app, PspAppFrameState *frame,
                        const char *url, const char *failure_status)
{
    BrowserEngine *engine = app->browser->engine;
    bool shown = url != NULL && psp_offline_store_offer_file(
        &app->browser->offline_store, engine, url);
    (void) psp_engine_views_refresh(app->views, engine);
    psp_sync_ui(&app->process->presentation.ui, engine, app->browser->profile);
    if (shown) psp_ui_show_file_download(&app->process->presentation.ui,
        psp_offline_store_file_preview(&app->browser->offline_store));
    else psp_ui_show_status(&app->process->presentation.ui, failure_status, 180);
    frame->page_dirty |= shown;
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
/* Brings the network up for an action about to fetch url, unless a trace
   replays the traffic. When it cannot, the buttons held through the
   connection UI become the previous state, so releasing them does not act
   again, and this returns false: the caller abandons the action. */
bool psp_action_network_ready(PspApp *app, const char *method,
                              const char *url,
                              const uint16_t *engine_frame)
{
    if (strcmp(app->process->config.trace, "none") != 0) return true;
    if (psp_app_ensure_network_for_navigation(app, method, url, engine_frame)) return true;
    app->interactive->previous_buttons =
        psp_ui_buttons(psp_navigation_observed_buttons());
    return false;
}
#endif

static void psp_app_finish_focus_action(
    PspApp *app, PspAppFrameState *frame, PspUiIntent *intent,
    bool changed)
{
    if (frame == NULL) return;
    frame->page_dirty = changed || frame->page_dirty;
    if (app == NULL || app->process == NULL || app->browser == NULL
        || app->views == NULL || intent == NULL || !frame->page_dirty
        || psp_navigation_cooperate_supervised()) return;
    /* Input updates retained paint/scroll state, not the composed framebuffer.
       In particular an authored outline is painted during composition, and
       focus may scroll into uncached rows. Never publish the old frame and
       clear page_dirty as though those changes had reached the screen.
       Spend one bounded input-priority slice; unfinished work stays dirty for
       the normal render scheduler, ahead of optional resource work. */
    /* Over a live canvas this loop would render and present again after
       the page's animation callbacks: two compositions per move. Present
       the move now and skip this iteration's callbacks, unless the
       previous iteration already skipped them (a held D-pad); then leave
       the move to the loop's own canvas frame. */
    bool single_composition = frame->canvas_live;
    if (single_composition && frame->runtime_deferred) return;
    if (psp_navigation_cooperate_page_input_pending()) {
        (void) psp_engine_views_refresh(app->views, app->browser->engine);
        frame->defer_runtime = true;
        return;
    }
    /* Focus dispatch may replace the render shell. Refresh borrowed views
       before arming another scope; cache preparation keeps this framebuffer
       unchanged and render-compose suppresses owner presents during writes. */
    bool supervise_paint = !single_composition
        && !psp_navigation_cooperate_active()
        && !app->process->presentation.ui.page_gamepad_capture
        && psp_engine_views_refresh(app->views, app->browser->engine)
        && app->views->frame != NULL;
    if (supervise_paint)
        psp_runtime_cooperate_begin(&app->process->presentation.ui,
            app->views->frame, &app->interactive->toolbar_input);
    BrowserRenderJobStatus rendered = browser_engine_render_frame_bounded_cancelable(
        app->browser->engine, PSP_RENDER_JOB_BUDGET_US,
        single_composition ? 4u : 0u,
        supervise_paint ? psp_navigation_cancellation() : NULL);
    uint32_t observed_buttons = 0;
    if (supervise_paint && psp_runtime_cooperate_end(&observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(observed_buttons);
    if (rendered == BROWSER_RENDER_JOB_PENDING && !single_composition
        && !psp_navigation_cooperate_page_input_pending()
        && browser_engine_render_placeholder_frame(app->browser->engine, NULL)
        && psp_engine_views_refresh(app->views, app->browser->engine)) {
        /* The cache-only pass never waits on the first cold tile. Show the
           selected position/indicator from resident pixels now; the job owns
           all unfinished tiles and keeps running ahead of optional work. */
        psp_sync_ui(&app->process->presentation.ui, app->browser->engine,
                    app->browser->profile);
        if (psp_present_internal(app->views->frame,
                                &app->process->presentation.ui, true)) {
            frame->focus_partial_presented = true;
            frame->page_dirty = false;
            frame->defer_runtime = true;
            intent->visual_changed = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            printf("tilefinch-focus-preview: at-us=%llu action=%d\n",
                   (unsigned long long) sceKernelGetSystemTimeWide(),
                   (int) intent->action);
#endif
        }
    }
    if (rendered != BROWSER_RENDER_JOB_COMPLETE
        || !psp_engine_views_refresh(app->views, app->browser->engine)) return;
    psp_sync_ui(
        &app->process->presentation.ui, app->browser->engine,
        app->browser->profile);
    bool shown = psp_present_internal(
        app->views->frame, &app->process->presentation.ui, true);
    if (!shown) return;
    frame->page_dirty = false;
    intent->visual_changed = false;
    frame->defer_runtime = single_composition;
}

/* Focus events and :focus styling can run author code and rebuild the page.
   Use the same owner-thread supervision as activation, not an uninterruptible
   d-pad receiver. The pre-dispatch checkpoint publishes only retained geometry. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void psp_app_dispatch_focus_action(
    PspApp *app, PspAppFrameState *frame, PspUiIntent *intent)
{
    BrowserEngine *engine = app->browser->engine;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Arm before the pre-author native focus presentation, not only after
       the potentially expensive event/layout dispatch has returned. */
    psp_focus_feedback_begin(engine, (int) intent->action,
        frame->replayed_press_us != 0 ? frame->replayed_press_us : frame->ui_sample_us);
#endif
    bool supervise = !psp_navigation_cooperate_active()
        && app->views->frame != NULL
        && !app->process->presentation.ui.page_gamepad_capture;
    if (supervise) {
        psp_runtime_cooperate_begin(&app->process->presentation.ui,
            app->views->frame, &app->interactive->toolbar_input);
        psp_runtime_cooperate_serve_page_scroll(engine);
    }
    bool changed = false;
    switch (intent->action) {
        case PSP_UI_ACTION_FOCUS_PREVIOUS:
        case PSP_UI_ACTION_FOCUS_NEXT:
            changed = browser_engine_focus_move(
                engine, intent->action == PSP_UI_ACTION_FOCUS_NEXT);
            break;
        case PSP_UI_ACTION_FOCUS_AT:
            changed = browser_engine_focus_at(
                engine, intent->pointer_x, intent->pointer_y);
            break;
        default: {
            ControllerFocusDirection direction = CONTROLLER_FOCUS_DOWN;
            if (intent->action == PSP_UI_ACTION_FOCUS_UP)
                direction = CONTROLLER_FOCUS_UP;
            else if (intent->action == PSP_UI_ACTION_FOCUS_LEFT)
                direction = CONTROLLER_FOCUS_LEFT;
            else if (intent->action == PSP_UI_ACTION_FOCUS_RIGHT)
                direction = CONTROLLER_FOCUS_RIGHT;
            changed = browser_engine_focus_direction(engine, direction);
            break;
        }
    }
    uint32_t observed_buttons = 0;
    if (supervise && psp_runtime_cooperate_end(&observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(observed_buttons);
    psp_app_finish_focus_action(app, frame, intent, changed);
}

typedef struct {
    TilefinchDiagnosticSource sources[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT];
    char paths[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT]
              [TILEFINCH_INSTALL_PATH_LIMIT];
    char signin_report[1024];
} PspDiagnosticPathSet;

/* Keep the five long install paths and zlib/QR setup out of the already-large
   heavy action receiver's stack frame. The path set exists only for this
   explicit menu action and is released before the QR screen is presented. */
__attribute__((noinline))
static void psp_app_build_diagnostic_qr(
    PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || frame == NULL) return;
    bool signin_only = app->process->presentation.ui.youtube_signin_diagnostics;
    static const char *const names[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT] = {
        "tilefinch-last-error.txt",
        "tilefinch-validation.txt",
        "tilefinch-crash.txt",
        "tilefinch-validation.previous.txt",
        "tilefinch-crash.previous.txt"
    };
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Validation logging is fully buffered to protect frame cadence. This
       user-triggered export is the one place where paying a flush is useful:
       it makes the current file in the bundle match the latest completed
       diagnostic line. Shipping builds have no full validation stream. */
    if (!signin_only) (void) psp_log_flush(false);
#endif
    PspDiagnosticPathSet *paths = calloc(1u, sizeof(*paths));
    if (paths == NULL) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "NOT ENOUGH MEMORY FOR DIAGNOSTICS", 240);
        frame->page_dirty = true;
        return;
    }
    bool paths_valid = true;
    for (size_t at = 0; !signin_only && at < TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT; at++) {
        paths_valid = paths_valid && tilefinch_install_data_path(
            &app->process->install_paths, names[at],
            paths->paths[at], sizeof(paths->paths[at]));
        paths->sources[at].name = names[at];
        paths->sources[at].path = paths->paths[at];
    }
    const char *wifi = psp_wifi_diagnostic_report();
    if (!signin_only && wifi[0] != '\0') {
        /* The current Wi-Fi failure takes priority over the oldest crash
           source. QR owns its bytes; this writes no Memory Stick file. */
        for (size_t at = TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT - 1u; at > 0; at--)
            paths->sources[at] = paths->sources[at - 1u];
        paths->sources[0] = (TilefinchDiagnosticSource) {
            .name = "tilefinch-wifi.txt", .memory = wifi,
            .memory_size = strlen(wifi)
        };
    }
    if (signin_only) {
        paths_valid = browser_engine_youtube_signin_report(app->browser->engine,
            paths->signin_report, sizeof(paths->signin_report));
        paths->sources[0] = (TilefinchDiagnosticSource) {
            .name = "tilefinch-youtube-signin.txt", .memory = paths->signin_report,
            .memory_size = strlen(paths->signin_report)
        };
    }
    if (!paths_valid) {
        free(paths);
        psp_ui_show_status(
            &app->process->presentation.ui,
            "DIAGNOSTIC LOG PATH UNAVAILABLE", 240);
        frame->page_dirty = true;
        return;
    }
    psp_ui_set_diagnostic_qr(&app->process->presentation.ui, NULL);
    tilefinch_diagnostic_qr_destroy(app->process->diagnostic_qr);
    app->process->diagnostic_qr = NULL;
    TilefinchDiagnosticMetadata metadata = {
        .app_version = TILEFINCH_VERSION_STRING,
        .release_sequence = TILEFINCH_RELEASE_SEQUENCE,
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        .created_unix_time = (uint64_t) time(NULL),
#else
        .created_unix_time = 0u,
#endif
        /* The public user-mode SDK has no portable model query. The kernel
           export used by some CFW utilities is not safe to make a release
           dependency merely for display metadata. */
        .psp_model = UINT32_MAX,
        .psp_firmware = (uint32_t) sceKernelDevkitVersion()
    };
    char error[PSP_UI_STATUS_CAPACITY];
    app->process->diagnostic_qr = tilefinch_diagnostic_qr_build(
        &metadata, paths->sources, signin_only ? 1u : TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT,
        error, sizeof(error));
    free(paths);
    if (app->process->diagnostic_qr == NULL) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            error[0] == '\0' ? "DIAGNOSTIC QR COULD NOT BE BUILT" : error,
            240);
    } else {
        app->process->presentation.ui.status[0] = '\0';
        app->process->presentation.ui.toast_frames = 0u;
        psp_ui_set_diagnostic_qr(
            &app->process->presentation.ui,
            tilefinch_diagnostic_qr_view(app->process->diagnostic_qr));
    }
    frame->page_dirty = true;
}

static void psp_app_step_diagnostic_qr(
    PspApp *app, PspAppFrameState *frame, int direction)
{
    if (app == NULL || frame == NULL || app->process->diagnostic_qr == NULL)
        return;
    const TilefinchDiagnosticQrView *view = tilefinch_diagnostic_qr_view(
        app->process->diagnostic_qr);
    if (view == NULL || view->page_count == 0u) return;
    unsigned page = view->page_index;
    page = direction < 0
        ? (page == 0u ? view->page_count - 1u : page - 1u)
        : (page + 1u) % view->page_count;
    if (tilefinch_diagnostic_qr_select_page(
            app->process->diagnostic_qr, page)) {
        psp_ui_set_diagnostic_qr(
            &app->process->presentation.ui,
            tilefinch_diagnostic_qr_view(app->process->diagnostic_qr));
        frame->page_dirty = true;
    }
}

static void psp_app_step_diagnostic_part(
    PspApp *app, PspAppFrameState *frame, int direction)
{
    if (app == NULL || frame == NULL || app->process->diagnostic_qr == NULL)
        return;
    const TilefinchDiagnosticQrView *view = tilefinch_diagnostic_qr_view(
        app->process->diagnostic_qr);
    if (view == NULL || view->part_count == 0u) return;
    unsigned part = view->part_index;
    part = direction < 0
        ? (part == 0u ? view->part_count - 1u : part - 1u)
        : (part + 1u) % view->part_count;
    char error[PSP_UI_STATUS_CAPACITY];
    if (!tilefinch_diagnostic_qr_select_part(
            app->process->diagnostic_qr, part, error, sizeof(error))) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            error[0] == '\0' ? "DIAGNOSTIC PART COULD NOT BE BUILT" : error,
            240);
    } else {
        app->process->presentation.ui.status[0] = '\0';
        app->process->presentation.ui.toast_frames = 0u;
    }
    psp_ui_set_diagnostic_qr(
        &app->process->presentation.ui,
        tilefinch_diagnostic_qr_view(app->process->diagnostic_qr));
    frame->page_dirty = true;
}

/*
 * History is allowed to name generated/native pages which deliberately have
 * no network server.  Rebuild those pages after moving the history cursor;
 * if rebuilding fails, put the cursor back so Back/Forward remains
 * transactional.  Returning true means the target was internal and has
 * been handled (successfully or with a local error), so the caller must not
 * let it fall through to curl.
 */
static bool psp_history_open_internal(
    PspApp *app, PspAppFrameState *frame, bool forward,
    const PspLocalRoute *route, PspLocalOpen open)
{
    if (app == NULL || frame == NULL || route == NULL
        || open == PSP_LOCAL_OPEN_WEB || open == PSP_LOCAL_OPEN_OFFLINE)
        return false;
    if (open == PSP_LOCAL_OPEN_UNAVAILABLE) {
        psp_ui_show_status(&app->process->presentation.ui, "LOCAL PAGE UNAVAILABLE", 180);
        return true;
    }
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    bool home = open == PSP_LOCAL_OPEN_NATIVE_HOME;
    bool screenshots = open == PSP_LOCAL_OPEN_SCREENSHOTS;
    bool collections = open == PSP_LOCAL_OPEN_COLLECTIONS;
    PspUiCollectionSection collection = route->section;
    PspProfilePageKind profile_page = route->profile_page;

    /* The caller already classified history_url; only the scroll is needed,
       which keeps the URL copy off this page-loading stack. */
    int restored_scroll = 0;
    if (!browser_engine_history_step(
            engine, forward, NULL, &restored_scroll)) {
        psp_ui_show_status(&app->process->presentation.ui, "HISTORY COULD NOT OPEN", 180);
        return true;
    }
    bool opened = true;
    if (home) {
        psp_show_native_home(app);
    } else if (collections) {
        psp_open_native_collections(app, collection, true);
    } else if (screenshots) {
        opened = psp_open_screenshot_list(
            engine, app->process->install_paths.data_dir, false);
        if (opened) psp_ui_leave_native_surface(&app->process->presentation.ui);
    } else {
        opened = psp_profile_open_page_history(
            engine, &app->process->presentation.ui, profile, profile_page, false);
        if (opened) psp_ui_leave_native_surface(&app->process->presentation.ui);
    }

    if (!opened) {
        (void) browser_engine_history_step_rollback(engine, forward);
        psp_ui_show_status(&app->process->presentation.ui, "LOCAL PAGE UNAVAILABLE", 180);
    } else if (!home && !collections) {
        (void) browser_engine_history_restore_scroll(engine, restored_scroll);
    }
    (void) psp_engine_views_refresh(app->views, engine);
    psp_sync_ui(&app->process->presentation.ui, engine, profile);
    /* Native HOME/COLLECTIONS deliberately sit above the resident engine
       page.  Capturing that engine page here would pair its screenshot
       document with the history cursor we just moved to a native URL. */
    if (opened && tabs != NULL && !home && !collections)
        (void) browser_tabs_capture_active(tabs, app->views->navigation);
    psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
    frame->page_dirty = opened || frame->page_dirty;
    return true;
}

/* Keep the find snapshot out of the focus/scroll receiver's stack frame. */
__attribute__((noinline))
static void psp_app_dispatch_find_step(
    PspApp *app, PspAppFrameState *frame, PspUiAction action)
{
    if (action == PSP_UI_ACTION_FIND_CLOSE) {
        browser_engine_find_clear(app->browser->engine);
        psp_ui_clear_find(&app->process->presentation.ui);
        frame->page_dirty = true;
        return;
    }
    BrowserFindSnapshot result = {0};
    int direction = action == PSP_UI_ACTION_FIND_PREVIOUS ? -1 : 1;
    if (browser_engine_find_move(app->browser->engine, direction, &result)) {
        psp_find_view_update(&app->process->presentation.find_view, &result);
        psp_ui_set_find(&app->process->presentation.ui, &app->process->presentation.find_view);
        frame->page_dirty = true;
    }
}

/* HOME Continue rows carry tab identity, not a URL. Translate that one
   presentation action into the ordinary tab command before the receiver
   switch so it reuses the complete request/restore/finish path below. */
static bool psp_app_resolve_home_tab_action(
    const PspApp *app, const PspUiIntent *source, PspUiIntent *resolved)
{
    if (app == NULL || source == NULL || resolved == NULL
        || source->action != PSP_UI_ACTION_HOME_ACTIVATE) return false;
    size_t target = 0;
    if (psp_home_target_kind(
            &app->process->presentation.home_surface,
            source->list_index, &target) != PSP_HOME_TARGET_TAB) return false;
    *resolved = *source;
    resolved->action = PSP_UI_ACTION_SWITCH_TAB;
    resolved->tab_index = target;
    return true;
}

/* Failure overlays historically sat above an unchanged incumbent, so Return
   only had to dismiss the sheet. Blank-but-successful navigations are already
   committed, however; translate Return into the ordinary transactional Back
   or Forward receiver that restores the actual originating history entry.
   A first navigation from native Home has no engine-history predecessor and
   is handled by the bounded Home fallback in the ordinary Return arm. */
static bool psp_app_resolve_recovery_return_action(
    PspApp *app, const PspUiIntent *source, PspUiIntent *resolved)
{
    if (app == NULL || source == NULL || resolved == NULL
        || source->action != PSP_UI_ACTION_RECOVERY_RETURN) return false;
    PspRecoveryOffer offer = app->interactive->recovery_offer;
    if (offer != PSP_RECOVERY_OFFER_RETURN_BACK
        && offer != PSP_RECOVERY_OFFER_RETURN_FORWARD) return false;
    *resolved = *source;
    resolved->action = offer == PSP_RECOVERY_OFFER_RETURN_FORWARD
        ? PSP_UI_ACTION_FORWARD : PSP_UI_ACTION_BACK;
    return true;
}

static void psp_app_clear_lifecycle_retry(PspApp *app)
{
    if (app == NULL) return;
    app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
}

/*
 * Keep the actions used while reading and spatially navigating out of the
 * large receiver below.  On Allegrex the full switch needs an 8 KiB stack
 * frame because its rare media/update/text-input arms carry large locals.
 * Sending every d-pad repeat through that frame needlessly spills registers
 * and pulls the cold receiver into the instruction cache.
 */
void psp_app_dispatch_action(
    PspApp *app, PspAppFrameState *frame, PspUiIntent *intent)
{
    if (intent == NULL || intent->action == PSP_UI_ACTION_NONE) return;
    if (psp_captive_portal_active(app->interactive)) {
        switch (intent->action) {
            case PSP_UI_ACTION_FOCUS_PREVIOUS:
            case PSP_UI_ACTION_FOCUS_NEXT:
            case PSP_UI_ACTION_FOCUS_UP:
            case PSP_UI_ACTION_FOCUS_DOWN:
            case PSP_UI_ACTION_FOCUS_LEFT:
            case PSP_UI_ACTION_FOCUS_RIGHT:
            case PSP_UI_ACTION_FOCUS_AT:
            case PSP_UI_ACTION_ACTIVATE:
            case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
            case PSP_UI_ACTION_BACK:
            case PSP_UI_ACTION_RELOAD:
            case PSP_UI_ACTION_PAGE_UP:
            case PSP_UI_ACTION_PAGE_DOWN:
            case PSP_UI_ACTION_SCROLL_TOP:
            case PSP_UI_ACTION_SCROLL_BOTTOM:
            case PSP_UI_ACTION_OPEN_FIND:
            case PSP_UI_ACTION_FIND_PREVIOUS:
            case PSP_UI_ACTION_FIND_NEXT:
            case PSP_UI_ACTION_FIND_EDIT:
            case PSP_UI_ACTION_FIND_CLOSE:
            case PSP_UI_ACTION_EXIT:
                break;
            default:
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "UNAVAILABLE DURING WI-FI SIGN-IN", 150);
                return;
        }
    }
    if (browser_engine_navigation_pending(app->browser->engine)) {
        /* D-pad focus moves over the loading page's preview (the engine
           presents the focus ring); the incumbent page is not on screen. */
        ControllerFocusDirection direction;
        switch (intent->action) {
            case PSP_UI_ACTION_FOCUS_PREVIOUS:
            case PSP_UI_ACTION_FOCUS_LEFT:
                direction = CONTROLLER_FOCUS_LEFT; break;
            case PSP_UI_ACTION_FOCUS_NEXT:
            case PSP_UI_ACTION_FOCUS_RIGHT:
                direction = CONTROLLER_FOCUS_RIGHT; break;
            case PSP_UI_ACTION_FOCUS_UP:
                direction = CONTROLLER_FOCUS_UP; break;
            case PSP_UI_ACTION_FOCUS_DOWN:
                direction = CONTROLLER_FOCUS_DOWN; break;
            case PSP_UI_ACTION_FOCUS_AT:
                return;
            default:
                direction = (ControllerFocusDirection) -1; break;
        }
        if ((int) direction >= 0) {
            (void) browser_engine_provisional_focus_direction(
                app->browser->engine, direction);
            return;
        }
    }
    switch (intent->action) {
        case PSP_UI_ACTION_FOCUS_PREVIOUS:
        case PSP_UI_ACTION_FOCUS_NEXT:
        case PSP_UI_ACTION_FOCUS_AT:
        case PSP_UI_ACTION_FOCUS_UP:
        case PSP_UI_ACTION_FOCUS_DOWN:
        case PSP_UI_ACTION_FOCUS_LEFT:
        case PSP_UI_ACTION_FOCUS_RIGHT:
            psp_app_dispatch_focus_action(app, frame, intent);
            return;
        case PSP_UI_ACTION_PAGE_UP:
        case PSP_UI_ACTION_PAGE_DOWN: {
            int direction =
                intent->action == PSP_UI_ACTION_PAGE_DOWN ? 1 : -1;
            if (!psp_request_provisional_scroll(
                    app->browser->engine, &app->process->presentation.ui,
                    direction, true)) {
                bool scrolled = browser_engine_scroll_page(
                    app->browser->engine, direction);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                if (scrolled)
                    psp_focus_feedback_begin(
                        app->browser->engine, (int) intent->action,
                        frame->replayed_press_us != 0
                            ? frame->replayed_press_us
                            : frame->ui_sample_us);
#endif
                frame->page_dirty = scrolled || frame->page_dirty;
            }
            return;
        }
        case PSP_UI_ACTION_SCROLL_TOP:
            frame->page_dirty = browser_engine_scroll_to_edge(
                app->browser->engine, false) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_SCROLL_BOTTOM:
            frame->page_dirty = browser_engine_scroll_to_edge(
                app->browser->engine, true) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FIND_PREVIOUS:
        case PSP_UI_ACTION_FIND_NEXT:
        case PSP_UI_ACTION_FIND_CLOSE:
            psp_app_dispatch_find_step(app, frame, intent->action);
            return;
        default:
            psp_app_dispatch_heavy_action(app, frame, intent);
            return;
    }
}

/*
 * The whole point of the split is that this frame stays out of the hot
 * dispatcher. It is static with one caller, which is exactly the shape GCC
 * inlines by default, and doing so would silently rebuild the 8 KiB frame
 * and the instruction-cache footprint that psp_app_dispatch_action's
 * 1,024-byte ratchet exists to prevent. Same reason as
 * psp_app_dispatch_find_step above.
 */
/* Back/Forward: native and offline history entries, same-document fragments, then ordinary history navigation. */
__attribute__((noinline))
static void psp_app_history_step(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent, const uint16_t *engine_frame, bool recovery_return)
{
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    if (intent->action == PSP_UI_ACTION_BACK
        && psp_captive_portal_cancel(app, frame)) return;
    bool forward = intent->action == PSP_UI_ACTION_FORWARD;
    char history_buffer[NAVIGATION_URL_LIMIT];
    const char *history_url = browser_engine_history_peek(
            engine, forward, history_buffer, sizeof(history_buffer))
        ? history_buffer : NULL;
    PspLocalRoute history_route =
        psp_local_route_classify(history_url);
    PspLocalOpen history_open = psp_local_route_open(
        &history_route, PSP_ROUTE_ORIGIN_HISTORY);
    if (psp_history_open_internal(
            app, frame, forward, &history_route, history_open)) {
        if (recovery_return) psp_app_clear_lifecycle_retry(app);
        return;
    }
    if (history_open == PSP_LOCAL_OPEN_OFFLINE) {
        BrowserHistoryEntryText moved_entry;
        int restored_scroll = 0;
        bool moved = browser_engine_history_step(
            engine, forward, &moved_entry, &restored_scroll);
        PspOfflineRouteResult result = moved
            ? psp_offline_store_handle_url(
                  &app->browser->offline_store, engine, profile,
                  moved_entry.url, moved_entry.title, false)
            : PSP_OFFLINE_ROUTE_ERROR;
        if (result == PSP_OFFLINE_ROUTE_ERROR && moved) {
            (void) browser_engine_history_step_rollback(
                engine, forward);
        } else if (result == PSP_OFFLINE_ROUTE_PAGE) {
            (void) browser_engine_history_restore_scroll(
                engine, restored_scroll);
        }
        app->process->presentation.ui.reader_mode = false;
        app->process->presentation.ui.basic_mode = false;
        (void) psp_engine_views_refresh(app->views, engine);
        psp_sync_ui(&app->process->presentation.ui, engine, profile);
        if (result == PSP_OFFLINE_ROUTE_PAGE && tabs != NULL)
            (void) browser_tabs_capture_active(
                tabs, app->views->navigation);
        psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
        frame->page_dirty = result == PSP_OFFLINE_ROUTE_PAGE
            || frame->page_dirty;
        if (recovery_return && result == PSP_OFFLINE_ROUTE_PAGE)
            psp_app_clear_lifecycle_retry(app);
        psp_ui_show_status(
            &app->process->presentation.ui,
            psp_offline_store_status(&app->browser->offline_store),
            180);
        return;
    }
    /* Fragment history is a document-local restore. Handle it before
       the network/loading transition so Back/Forward never wakes
       Wi-Fi, hides Reader, or flashes a loading page for work that
       cannot issue request bytes. */
    if (history_url != NULL
        && navigation_url_is_same_document(
               app->views->navigation, history_url)) {
        bool restored = browser_engine_history_move(engine, forward);
        if (restored) {
            if (recovery_return)
                psp_app_clear_lifecycle_retry(app);
            (void) psp_engine_views_refresh(app->views, engine);
            psp_sync_ui(
                &app->process->presentation.ui, engine, profile);
            if (tabs != NULL) {
                (void) browser_tabs_capture_active(
                    tabs, app->views->navigation);
            }
            psp_tabs_sync_ui(
                &app->process->presentation.ui, tabs,
                &app->process->presentation.tab_view);
            frame->page_dirty = true;
        } else {
            psp_ui_show_status(
                &app->process->presentation.ui,
                forward ? "NO FORWARD PAGE" : "NO BACK PAGE", 120);
        }
        return;
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (history_url != NULL && !psp_action_network_ready(
            app, "GET", history_url, engine_frame))
        return;
#endif
    psp_ui_set_loading(&app->process->presentation.ui, true, -1);
    psp_leave_reader_for_navigation(
        engine, &app->process->presentation.ui, profile, history_url);
    psp_ui_show_status(
        &app->process->presentation.ui, forward ? "LOADING FORWARD"
                     : "LOADING BACK",
        120);
    psp_navigation_cooperate_begin(
        &app->process->presentation.ui, engine_frame, engine);
    psp_text_input_before_navigation(&app->process->text_input);
    bool javascript_ready = history_url != NULL
        && browser_engine_set_javascript_enabled(
            engine,
            browser_profile_javascript_allowed_for_url(
                profile, history_url));
    bool started = javascript_ready
        && browser_engine_begin_navigation_history(
            engine, forward, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
    if (started) {
        if (recovery_return) psp_app_clear_lifecycle_retry(app);
        app->interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    } else {
        psp_navigation_cooperate_end(
            "interactive-history-start-failed");
        psp_ui_set_loading(&app->process->presentation.ui, false, 0);
        psp_ui_show_status(
            &app->process->presentation.ui, forward ? "NO FORWARD PAGE" : "NO BACK PAGE",
            120);
    }
}

/* Reload, and the recovery sheet's retry actions: local surfaces through the route classifier, otherwise a fresh page load. */
__attribute__((noinline))
static void psp_app_reload(
    PspApp *app, PspAppFrameState *frame, const uint16_t *engine_frame)
{
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    char *lifecycle_retry_url = app->interactive->lifecycle_retry_url;
    char entry_buffer[NAVIGATION_URL_LIMIT];
    const char *entry_url = psp_app_current_url(
        engine, entry_buffer, sizeof(entry_buffer));
    char reload_url[NAVIGATION_URL_LIMIT];
    snprintf(reload_url, sizeof(reload_url), "%s",
             (app->interactive->recovery_offer != PSP_RECOVERY_OFFER_NONE)
                 ? lifecycle_retry_url
                 : (entry_url == NULL ? app->process->config.url : entry_url));
    bool resumed_navigation =
        app->interactive->recovery_offer != PSP_RECOVERY_OFFER_NONE;
    PspLocalRoute reload_route = psp_local_route_classify(reload_url);
    PspLocalOpen reload_open = psp_local_route_open(
        &reload_route, PSP_ROUTE_ORIGIN_RELOAD);
    if (reload_open == PSP_LOCAL_OPEN_NATIVE_HOME) {
        psp_show_native_home(app);
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        return;
    }
    if (reload_open == PSP_LOCAL_OPEN_COLLECTIONS) {
        /* A pre-upgrade /bookmarks or /history entry shows the
           native surface, as Back/Forward and tab restore show
           it; the frontend no longer routes to the engine's
           HTML generators for them. */
        psp_open_native_collections(
            app, reload_route.section, true);
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        return;
    }
    if (reload_open == PSP_LOCAL_OPEN_SCREENSHOTS) {
        psp_text_input_before_navigation(&app->process->text_input);
        bool opened = psp_open_screenshot_list(
            engine, app->process->install_paths.data_dir, false);
        if (opened)
            psp_ui_leave_native_surface(&app->process->presentation.ui);
        (void) psp_engine_views_refresh(app->views, engine);
        psp_sync_ui(&app->process->presentation.ui, engine, profile);
        if (opened && tabs != NULL)
            (void) browser_tabs_capture_active(
                tabs, app->views->navigation);
        psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
        frame->page_dirty = opened || frame->page_dirty;
        psp_ui_show_status(
            &app->process->presentation.ui, opened ? "PAGE REFRESHED"
                            : "PAGE UNAVAILABLE",
            180);
        if (opened) {
            app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        }
        return;
    }
    if (reload_open == PSP_LOCAL_OPEN_PROFILE_PAGE) {
        psp_text_input_before_navigation(&app->process->text_input);
        bool opened = psp_profile_open_page(
            engine, &app->process->presentation.ui, profile,
            reload_route.profile_page);
        frame->page_dirty = opened || frame->page_dirty;
        (void) psp_engine_views_refresh(app->views, engine);
        if (opened && tabs != NULL)
            (void) browser_tabs_capture_active(
                tabs, app->views->navigation);
        psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
        psp_ui_show_status(
            &app->process->presentation.ui, opened ? "PAGE REFRESHED"
                        : "PAGE UNAVAILABLE",
            180);
        if (opened) {
            app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        }
        return;
    }
    if (reload_open == PSP_LOCAL_OPEN_OFFLINE) {
        PspOfflineRouteResult result =
            psp_offline_store_handle_url(
                &app->browser->offline_store, engine, profile,
                reload_url, NULL,
                false);
        (void) psp_engine_views_refresh(app->views, engine);
        psp_sync_ui(&app->process->presentation.ui, engine, profile);
        if (result == PSP_OFFLINE_ROUTE_PAGE && tabs != NULL)
            (void) browser_tabs_capture_active(
                tabs, app->views->navigation);
        psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
        frame->page_dirty = result == PSP_OFFLINE_ROUTE_PAGE
            || frame->page_dirty;
        psp_ui_show_status(
            &app->process->presentation.ui,
            psp_offline_store_status(&app->browser->offline_store),
            180);
        if (result == PSP_OFFLINE_ROUTE_PAGE) {
            app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        }
        return;
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (!psp_action_network_ready(
            app, "GET", reload_url, engine_frame))
        return;
#endif
    bool started = psp_begin_page_load(
        engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
        reload_url, false,
        PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
    if (started) {
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        app->interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    }
    if (started && resumed_navigation)
        psp_ui_show_status(
            &app->process->presentation.ui, "RETRYING INTERRUPTED PAGE", 180);
}

/* The recovery sheet's Reload in Basic view: one script-free reload of the failed page. */
__attribute__((noinline))
static void psp_app_recovery_reload_basic(
    PspApp *app, const uint16_t *engine_frame)
{
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    char *lifecycle_retry_url = app->interactive->lifecycle_retry_url;
    if (app->interactive->recovery_offer == PSP_RECOVERY_OFFER_NONE) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "FAILED PAGE IS NO LONGER AVAILABLE", 180);
        return;
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (strcmp(app->process->config.trace, "none") == 0
        && !psp_ensure_network_for_navigation(
               app->network, app->network_lifecycle,
               (int) app->process->config.network_profile,
               "GET", lifecycle_retry_url, true,
               engine_frame, &app->process->presentation.ui)) {
        return;
    }
#endif
    /* Page JavaScript is off for this one load only: the commit (or
       failure) restores the site's policy, and every ordinary
       navigation sets it again. */
    if (!browser_engine_set_javascript_enabled(engine, false)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "BASIC VIEW UNAVAILABLE", 240);
        return;
    }
    psp_leave_reader_for_navigation(
        engine, &app->process->presentation.ui, profile,
        lifecycle_retry_url);
    psp_ui_set_navigation_target(
        &app->process->presentation.ui, lifecycle_retry_url);
    psp_ui_set_loading(&app->process->presentation.ui, true, -1);
    psp_ui_show_status(
        &app->process->presentation.ui, "LOADING BASIC VIEW", 240);
    psp_navigation_cooperate_begin(
        &app->process->presentation.ui, engine_frame, engine);
    psp_text_input_before_navigation(&app->process->text_input);
    bool started = psp_retry_navigation_url_after_reclaim(
        engine, lifecycle_retry_url, PSP_NAVIGATION_DOCUMENT_BYTES,
        PSP_NAVIGATION_TIMEOUT_MS, false);
    if (started) {
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        app->interactive->basic_reload_pending = true;
        snprintf(app->interactive->basic_reload_url,
                 sizeof(app->interactive->basic_reload_url), "%s",
                 lifecycle_retry_url);
        app->interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    } else {
        psp_navigation_cooperate_end("basic-reload-start-failed");
        (void) browser_engine_set_javascript_enabled(
            engine, browser_profile_javascript_allowed_for_url(
                        profile, lifecycle_retry_url));
        psp_ui_set_loading(&app->process->presentation.ui, false, 0);
        psp_ui_show_status(
            &app->process->presentation.ui,
            browser_engine_last_error(engine), 300);
    }
}

__attribute__((noinline))
static void psp_app_dispatch_heavy_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent)
{
    if (intent == NULL || intent->action == PSP_UI_ACTION_NONE) return;
    app->interactive->script_text_focus_deadline_us = 0;
    PspUiIntent resolved_home_tab;
    if (psp_app_resolve_home_tab_action(
            app, intent, &resolved_home_tab)) intent = &resolved_home_tab;
    PspUiIntent resolved_recovery_return;
    bool recovery_return = psp_app_resolve_recovery_return_action(
        app, intent, &resolved_recovery_return);
    if (recovery_return) {
        intent = &resolved_recovery_return;
    }
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    const uint16_t *engine_frame = app->views->frame;
    char *lifecycle_retry_url = app->interactive->lifecycle_retry_url;
    switch (intent->action) {
        case PSP_UI_ACTION_ACTIVATE:
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT: {
            psp_app_dispatch_activation(
                app, frame,
                intent->action == PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT,
                engine_frame);
            break;
        }
        case PSP_UI_ACTION_BACK:
        case PSP_UI_ACTION_FORWARD:
            psp_app_history_step(app, frame, intent, engine_frame, recovery_return);
            break;
        case PSP_UI_ACTION_VOICE_FOCUSED_TEXT:
            psp_youtube_preresolve_reset(
                &app->browser->youtube_preresolve,
                "voice input memory reclaim");
            if (psp_replace_focused_text(
                    engine, engine_frame, &app->process->presentation.ui,
                    &app->process->text_input, true, NULL)) {
                frame->page_dirty = true;
                (void) psp_engine_views_refresh(app->views, engine);
            }
            app->interactive->previous_buttons = 0;
            break;
        case PSP_UI_ACTION_DOWNLOAD_LINK: {
            char url[NAVIGATION_URL_LIMIT];
            psp_app_offer_file(app, frame,
                controller_focused_download_url(browser_engine_controller(engine), url, sizeof(url))
                    ? url : NULL,
                "FOCUS AN HTTPS DOWNLOAD LINK FIRST");
            break;
        }
        case PSP_UI_ACTION_CONFIRM_FILE_DOWNLOAD:
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            {
                const PspUiFileDownloadPreview *offer =
                    psp_offline_store_file_preview(&app->browser->offline_store);
                if (offer == NULL || !psp_action_network_ready(app, "GET", offer->origin, engine_frame)) {
                    psp_offline_store_discard_file(&app->browser->offline_store);
                    break;
                }
            }
#endif
            (void) psp_offline_store_accept_file(&app->browser->offline_store);
            psp_ui_show_status(&app->process->presentation.ui,
                psp_offline_store_status(&app->browser->offline_store), 240);
            break;
        case PSP_UI_ACTION_CANCEL_FILE_DOWNLOAD:
            psp_offline_store_discard_file(&app->browser->offline_store);
            break;
        case PSP_UI_ACTION_CONFIRM_ZIP_INSTALL: {
            bool removing = zip_install_preview(app->browser->offline_store.zip_install) != NULL
                && zip_install_preview(app->browser->offline_store.zip_install)->removing;
            psp_ui_show_status(&app->process->presentation.ui,
                removing ? "REMOVING HOMEBREW - O STOP" : "INSTALLING HOMEBREW - O CANCEL", 600);
            psp_work_cooperate_begin(&app->process->presentation.ui, engine_frame,
                true, true, false, removing ? "STOPPING REMOVAL..." : "STOPPING INSTALLATION...", "zip-install", NULL, NULL);
            (void) psp_offline_store_install_prepared_zip(&app->browser->offline_store);
            psp_navigation_cooperate_end_adopting_buttons("zip-install", &app->interactive->previous_buttons);
            psp_ui_show_status(&app->process->presentation.ui,
                psp_offline_store_status(&app->browser->offline_store), 300);
            if (removing) {
                (void) psp_offline_store_handle_url(&app->browser->offline_store, engine, profile,
                    "https://tilefinch.local/offline/homebrew", NULL, false);
                (void) psp_engine_views_refresh(app->views, engine);
                psp_sync_ui(&app->process->presentation.ui, engine, profile);
                frame->page_dirty = true;
            }
            break;
        }
        case PSP_UI_ACTION_CANCEL_ZIP_INSTALL:
            psp_offline_store_discard_zip(&app->browser->offline_store);
            break;
        case PSP_UI_ACTION_SAVE_FOR_LATER: {
            psp_ui_show_status(
                &app->process->presentation.ui, "SAVING ARTICLE - CIRCLE STOPS", 120);
            psp_work_cooperate_begin(
                &app->process->presentation.ui, engine_frame, true, true, false,
                "STOPPING ARTICLE SAVE...", "offline-article", NULL, NULL);
            bool saved = psp_offline_store_save_current(
                &app->browser->offline_store, engine);
            bool cancelled = psp_navigation_cancel_requested();
            psp_navigation_cooperate_end_adopting_buttons(
                "offline-article", &app->interactive->previous_buttons);
            psp_ui_show_status(
                &app->process->presentation.ui, cancelled ? "ARTICLE SAVE STOPPED"
                    : psp_offline_store_status(&app->browser->offline_store),
                saved ? 180 : (cancelled ? 120 : 240));
            break;
        }
        case PSP_UI_ACTION_INSTALL_OFFLINE_APP: {
            psp_ui_show_status(&app->process->presentation.ui,
                               "PREPARING APP PREVIEW - CIRCLE STOPS", 120);
            psp_work_cooperate_begin(
                &app->process->presentation.ui, engine_frame, true, true,
                false, "STOPPING APP PREVIEW...", "offline-app-preview",
                NULL, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t preview_started_us = sceKernelGetSystemTimeWide();
#endif
            bool prepared = psp_offline_store_prepare_current_app(
                &app->browser->offline_store, engine);
            bool cancelled = psp_navigation_cancel_requested();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            printf("tilefinch-offline-app-op: op=preview ok=%u "
                   "cancelled=%u elapsed=%lluus status=\"%s\"\n",
                   prepared ? 1u : 0u, cancelled ? 1u : 0u,
                   (unsigned long long) (sceKernelGetSystemTimeWide()
                                         - preview_started_us),
                   psp_offline_store_status(&app->browser->offline_store));
#endif
            psp_navigation_cooperate_end_adopting_buttons(
                "offline-app-preview", &app->interactive->previous_buttons);
            if (prepared && !cancelled) {
                psp_ui_show_offline_app_preview(
                    &app->process->presentation.ui,
                    psp_offline_store_app_preview(
                        &app->browser->offline_store));
            } else {
                psp_offline_store_discard_app_preparation(
                    &app->browser->offline_store);
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    cancelled ? "APP PREVIEW STOPPED"
                              : psp_offline_store_status(
                                    &app->browser->offline_store),
                    cancelled ? 120 : 240);
            }
            break;
        }
        case PSP_UI_ACTION_CONFIRM_OFFLINE_APP: {
            psp_ui_show_status(&app->process->presentation.ui,
                               "INSTALLING OFFLINE APP - CIRCLE STOPS", 120);
            psp_work_cooperate_begin(
                &app->process->presentation.ui, engine_frame, true, true,
                false, "STOPPING APP INSTALL...", "offline-app", NULL, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            uint64_t install_started_us = sceKernelGetSystemTimeWide();
#endif
            bool saved = psp_offline_store_install_current_app(
                &app->browser->offline_store, engine);
            bool cancelled = psp_navigation_cancel_requested();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            printf("tilefinch-offline-app-op: op=install ok=%u "
                   "cancelled=%u elapsed=%lluus status=\"%s\"\n",
                   saved ? 1u : 0u, cancelled ? 1u : 0u,
                   (unsigned long long) (sceKernelGetSystemTimeWide()
                                         - install_started_us),
                   psp_offline_store_status(&app->browser->offline_store));
#endif
            psp_navigation_cooperate_end_adopting_buttons(
                "offline-app", &app->interactive->previous_buttons);
            psp_ui_show_status(
                &app->process->presentation.ui,
                cancelled ? "APP INSTALL STOPPED"
                          : psp_offline_store_status(
                                &app->browser->offline_store),
                saved ? 180 : (cancelled ? 120 : 240));
            break;
        }
        case PSP_UI_ACTION_CANCEL_OFFLINE_APP: {
            /* The recompile offer is an overlay over Library > Saved and
               shares the UI's native-view slot with the collections view;
               re-establish the list it returns to. */
            bool offered =
                app->browser->offline_store.recompile_offer_id != 0;
            (void) psp_offline_store_take_offer(
                &app->browser->offline_store, false);
            if (offered)
                psp_collections_sync_ui(
                    &app->process->presentation.ui,
                    &app->process->presentation.collections_surface,
                    profile, &app->browser->offline_store,
                    PSP_UI_COLLECTION_SAVED);
            break;
        }
        case PSP_UI_ACTION_RECOMPILE_OFFLINE_APP:
        case PSP_UI_ACTION_OPEN_OFFLINE_APP_ANYWAY: {
            /* The answer to a Library recompile offer: recompile with
               progress (Circle stops), then open the app either way the
               user chose, through the ordinary Library activation. */
            PspOfflineStore *store = &app->browser->offline_store;
            PspUiState *ui = &app->process->presentation.ui;
            PspCollectionsSurface *surface =
                &app->process->presentation.collections_surface;
            bool anyway =
                intent->action == PSP_UI_ACTION_OPEN_OFFLINE_APP_ANYWAY;
            uint32_t id = psp_offline_store_take_offer(store, anyway);
            /* The offer borrowed the native-view slot the Saved list uses:
               restore the list before the recompile's progress is shown
               over it (it otherwise read "NOTHING HERE YET"). */
            psp_collections_sync_ui(ui, surface, profile, store,
                                    PSP_UI_COLLECTION_SAVED);
            if (!anyway) {
                psp_ui_show_status(
                    ui, "RECOMPILING OFFLINE APP - CIRCLE STOPS", 120);
                psp_work_cooperate_begin(
                    ui, engine_frame, true, true, false,
                    "STOPPING RECOMPILE...", "offline-app-recompile",
                    NULL, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                uint64_t recompile_started_us = sceKernelGetSystemTimeWide();
#endif
                bool recompiled = psp_offline_store_recompile_app(
                    store, id, &id);
                bool cancelled = psp_navigation_cancel_requested();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                printf("tilefinch-offline-app-op: op=recompile ok=%u "
                       "cancelled=%u id=%u elapsed=%lluus status=\"%s\"\n",
                       recompiled ? 1u : 0u, cancelled ? 1u : 0u,
                       (unsigned) id,
                       (unsigned long long) (sceKernelGetSystemTimeWide()
                                             - recompile_started_us),
                       psp_offline_store_status(store));
#endif
                psp_navigation_cooperate_end_adopting_buttons(
                    "offline-app-recompile",
                    &app->interactive->previous_buttons);
                if (!recompiled) {
                    psp_collections_sync_ui(ui, surface, profile, store,
                                            PSP_UI_COLLECTION_SAVED);
                    psp_ui_show_status(
                        ui, cancelled ? "RECOMPILE STOPPED"
                                      : psp_offline_store_status(store),
                        cancelled ? 120 : 240);
                    break;
                }
            }
            psp_collections_sync_ui(ui, surface, profile, store,
                                    PSP_UI_COLLECTION_SAVED);
            for (size_t row = 0; id != 0 && row < surface->view.count;
                 row++) {
                if (surface->id[row] != id) continue;
                ui->collections_selection = (uint8_t) row;
                PspUiIntent open = {
                    .action = PSP_UI_ACTION_COLLECTION_ACTIVATE,
                    .list_index = (uint8_t) row
                };
                psp_app_dispatch_heavy_action(app, frame, &open);
                break;
            }
            break;
        }
        case PSP_UI_ACTION_SHOW_SCREENSHOTS: {
            if (app->process->presentation.ui.screen
                    == PSP_UI_SCREEN_COLLECTIONS) {
                psp_collections_sync_ui(
                    &app->process->presentation.ui,
                    &app->process->presentation.collections_surface,
                    profile, &app->browser->offline_store,
                    PSP_UI_COLLECTION_SCREENSHOTS);
                break;
            }
            psp_text_input_before_navigation(&app->process->text_input);
            psp_leave_reader_for_navigation(
                engine, &app->process->presentation.ui, profile,
                "https://tilefinch.local/screenshots");
            bool opened = psp_open_screenshot_list(
                engine, app->process->install_paths.data_dir, true);
            if (opened)
                psp_ui_leave_native_surface(&app->process->presentation.ui);
            (void) psp_engine_views_refresh(app->views, engine);
            psp_sync_ui(&app->process->presentation.ui, engine, profile);
            if (opened && tabs != NULL)
                (void) browser_tabs_capture_active(
                    tabs, app->views->navigation);
            psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
            frame->page_dirty = opened || frame->page_dirty;
            psp_ui_show_status(
                &app->process->presentation.ui,
                opened ? "SCREENSHOTS" : "SCREENSHOTS UNAVAILABLE",
                180);
            break;
        }
        case PSP_UI_ACTION_RECOVERY_DISABLE_JAVASCRIPT:
            if (!(app->interactive->recovery_offer != PSP_RECOVERY_OFFER_NONE)
                || !browser_profile_set_site_javascript_enabled(
                    profile, lifecycle_retry_url, false)) {
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "SITE JAVASCRIPT COULD NOT BE DISABLED", 240);
                break;
            }
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_app_reload(app, frame, engine_frame);
            break;
        case PSP_UI_ACTION_RECOVERY_AUDIO_ONLY:
            browser_profile_set_youtube_audio_only(profile, true);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_app_reload(app, frame, engine_frame);
            break;
        case PSP_UI_ACTION_RECOVERY_LOWER_QUALITY:
            browser_profile_set_youtube_quality(
                profile, BROWSER_YOUTUBE_QUALITY_240P);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_app_reload(app, frame, engine_frame);
            break;
        case PSP_UI_ACTION_RECOVERY_RETURN:
            {
            bool return_home = app->interactive->recovery_offer
                == PSP_RECOVERY_OFFER_RETURN_HOME;
            app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
            if (return_home) {
                psp_show_native_home(app);
                frame->page_dirty = true;
            }
            psp_ui_show_status(
                &app->process->presentation.ui,
                "RETURNED TO LAST USABLE PAGE", 180);
            break;
            }
        case PSP_UI_ACTION_RECOVERY_READER: {
            if (app->interactive->recovery_offer == PSP_RECOVERY_OFFER_NONE) {
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "FAILED PAGE IS NO LONGER AVAILABLE", 180);
                break;
            }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (strcmp(app->process->config.trace, "none") == 0
                && !psp_ensure_network_for_navigation(
                       app->network, app->network_lifecycle,
                       (int) app->process->config.network_profile,
                       "GET", lifecycle_retry_url, true,
                       engine_frame, &app->process->presentation.ui)) {
                break;
            }
#endif
            psp_reader_mark_engaged(&app->process->presentation.ui, false);
            bool prepared = psp_reader_navigation_prepare(
                engine, &app->process->presentation.ui, profile,
                &app->interactive->reader_navigation,
                lifecycle_retry_url);
            psp_ui_set_navigation_target(
                &app->process->presentation.ui, lifecycle_retry_url);
            psp_ui_set_loading(&app->process->presentation.ui, true, -1);
            psp_ui_show_status(
                &app->process->presentation.ui,
                prepared ? "LOADING READER PAGE" : "READER MODE UNAVAILABLE",
                240);
            if (!prepared) break;
            psp_navigation_cooperate_begin(
                &app->process->presentation.ui, engine_frame, engine);
            psp_text_input_before_navigation(&app->process->text_input);
            bool started = psp_retry_navigation_url_after_reclaim(
                engine, lifecycle_retry_url, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS, false);
            if (started) {
                app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            } else {
                psp_navigation_cooperate_end("reader-retry-start-failed");
                psp_reader_navigation_finish(
                    engine, &app->process->presentation.ui, profile,
                    &app->interactive->reader_navigation, NULL, false);
                psp_ui_set_loading(&app->process->presentation.ui, false, 0);
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    browser_engine_last_error(engine), 300);
            }
            break;
        }
        case PSP_UI_ACTION_YOUTUBE_SIGNIN_CONTINUE: {
            const char *target = browser_engine_youtube_signin_continue_target(engine);
            if (target != NULL
                && psp_begin_page_load(engine, &app->process->presentation.ui,
                    profile, engine_frame, &app->process->text_input,
                    target, true,
                    PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS))
                app->interactive->navigation_job_started_us = (uint64_t) sceKernelGetSystemTimeWide();
            break;
        }
        case PSP_UI_ACTION_RELOAD:
            psp_app_reload(app, frame, engine_frame);
            break;
        case PSP_UI_ACTION_TOGGLE_READER:
            psp_app_toggle_reader(app, frame);
            break;
        case PSP_UI_ACTION_TOGGLE_BASIC:
            psp_app_toggle_basic_view(app, frame, false);
            break;
        case PSP_UI_ACTION_RECOVERY_BASIC:
            app->interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
            psp_app_toggle_basic_view(app, frame, true);
            break;
        case PSP_UI_ACTION_RECOVERY_RELOAD_BASIC:
            psp_app_recovery_reload_basic(app, engine_frame);
            break;
        case PSP_UI_ACTION_TOGGLE_READER_SITE: {
            char entry_buffer[NAVIGATION_URL_LIMIT];
            const char *entry_url = psp_app_current_url(
                engine, entry_buffer, sizeof(entry_buffer));
            const char *url = entry_url == NULL ? app->process->presentation.ui.url : entry_url;
            bool enable = !browser_profile_reader_site_always(
                profile, url);
            bool changed = browser_profile_set_reader_site_always(
                profile, url, enable);
            app->process->presentation.ui.reader_site_always = changed
                ? enable : browser_profile_reader_site_always(profile, url);
            if (changed)
                psp_profile_store_mark_dirty(
                    &app->browser->profile_store, frame->ui_sample_us);
            psp_ui_show_status(
                &app->process->presentation.ui,
                !changed ? "READER SITE SETTING UNAVAILABLE"
                : (enable ? "READER AUTO ON FOR THIS SITE"
                          : "READER AUTO OFF FOR THIS SITE"),
                240);
            break;
        }
        case PSP_UI_ACTION_OPEN_FIND:
        case PSP_UI_ACTION_FIND_EDIT: {
            BrowserFindSnapshot previous_find = {0};
            bool editing = intent->action == PSP_UI_ACTION_FIND_EDIT
                && browser_engine_find_snapshot(
                       engine, &previous_find);
            char query[BROWSER_FIND_QUERY_LIMIT + 1u] = {0};
            PspTextInputRequest request = {
                .description = "FIND IN PAGE",
                .initial = editing ? previous_find.query
                    : browser_tabs_active_find_query(tabs),
                .keyboard_url_mode = false,
                .suggest_navigation = false
            };
            bool accepted = psp_text_input_request(
                &app->process->text_input, engine_frame, &app->process->presentation.ui, &request,
                query, sizeof(query));
            BrowserFindSnapshot result = {0};
            if (accepted && query[0] != '\0'
                && browser_engine_find_begin(
                       engine, query, &result)) {
                psp_find_view_update(&app->process->presentation.find_view, &result);
                psp_ui_set_find(&app->process->presentation.ui, &app->process->presentation.find_view);
                (void) browser_tabs_set_active_find_query(
                    tabs, result.query);
                frame->page_dirty = true;
            } else if (editing) {
                psp_find_view_update(&app->process->presentation.find_view, &previous_find);
                psp_ui_set_find(&app->process->presentation.ui, &app->process->presentation.find_view);
            } else {
                browser_engine_find_clear(engine);
                psp_ui_clear_find(&app->process->presentation.ui);
                if (accepted)
                    psp_ui_show_status(
                        &app->process->presentation.ui, "ENTER TEXT TO FIND", 180);
            }
            app->interactive->previous_buttons = 0;
            break;
        }
        case PSP_UI_ACTION_OPEN_ADDRESS:
        case PSP_UI_ACTION_OPEN_VOICE_ADDRESS: {
            bool use_voice =
                intent->action == PSP_UI_ACTION_OPEN_VOICE_ADDRESS;
            if (use_voice) {
                psp_youtube_preresolve_reset(
                    &app->browser->youtube_preresolve,
                    "voice input memory reclaim");
            }
            char entry_buffer[NAVIGATION_URL_LIMIT];
            const char *entry_url = psp_app_current_url(
                engine, entry_buffer, sizeof(entry_buffer));
            char destination[NAVIGATION_URL_LIMIT] = {0};
            const char *current_url =
                entry_url == NULL ? app->process->config.url : entry_url;
            bool accepted = psp_request_omnibox(
                &app->process->text_input, engine_frame, &app->process->presentation.ui, profile,
                current_url, use_voice, false,
                destination, sizeof(destination));
            /* A typed internal-home address is the built-in start page, and
               the start page is native chrome now: loading the URL would
               resolve a host nothing serves and land on an error page.
               This is still a submitted omnibox navigation, so release the
               optional voice model exactly as psp_begin_page_load() does for
               every non-HOME destination before taking the short route. */
            bool native_home = accepted
                && psp_local_route_classify(destination).kind
                       == PSP_LOCAL_ROUTE_NATIVE_HOME;
            if (native_home) {
                psp_text_input_before_navigation(&app->process->text_input);
                (void) psp_route_native_home(app, destination);
            } else if (accepted) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                    if (!psp_action_network_ready(
                            app, "GET", destination, engine_frame))
                        break;
#endif
                    bool started = psp_begin_page_load(
                        engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                        destination, true,
                        PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
                    if (started)
                        app->interactive->navigation_job_started_us =
                            (uint64_t) sceKernelGetSystemTimeWide();
            }
            app->interactive->previous_buttons = 0;
            break;
        }
        case PSP_UI_ACTION_HOME: {
            /*
             * The built-in start page is now native chrome: it draws
             * immediately and never enters tab history. A profile
             * that opted into a custom homepage still gets its own
             * page, which is a document like any other.
             */
            if (!browser_profile_custom_homepage_enabled(profile)) {
                psp_home_sync_ui(
                    &app->process->presentation.ui, &app->process->presentation.home_surface, profile, tabs, true);
                psp_ui_show_home(&app->process->presentation.ui);
                break;
            }
            psp_text_input_before_navigation(&app->process->text_input);
            bool opened = psp_profile_open_page(
                engine, &app->process->presentation.ui, profile, PSP_PROFILE_PAGE_HOMEPAGE);
            frame->page_dirty = opened || frame->page_dirty;
            (void) psp_engine_views_refresh(app->views, engine);
            if (opened && tabs != NULL)
                (void) browser_tabs_capture_active(tabs, app->views->navigation);
            psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
            psp_ui_show_status(
                &app->process->presentation.ui, opened ? "My homepage" : "Homepage unavailable",
                180);
            break;
        }
        case PSP_UI_ACTION_HOME_ACTIVATE: {
            const char *destination = psp_home_target_url(
                &app->process->presentation.home_surface, intent->list_index, profile);
            if (destination == NULL) {
                psp_ui_show_status(&app->process->presentation.ui, "NOTHING TO OPEN", 120);
                break;
            }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (!psp_action_network_ready(
                    app, "GET", destination, engine_frame))
                break;
#endif
            psp_ui_leave_native_surface(&app->process->presentation.ui);
            bool started = psp_begin_page_load(
                engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                destination, true, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
            if (started) {
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            } else {
                psp_ui_show_home(&app->process->presentation.ui);
            }
            break;
        }
        case PSP_UI_ACTION_SWITCH_TAB:
        case PSP_UI_ACTION_NEW_TAB:
        case PSP_UI_ACTION_CLOSE_TAB:
            psp_app_tab_request(app, frame, intent, engine_frame);
            break;
        case PSP_UI_ACTION_TOGGLE_BOOKMARK: {
            BrowserHistoryEntryText entry;
            const char *entry_url =
                browser_engine_current_entry_text(engine, &entry, NULL)
                ? entry.url : NULL;
            bool local_page = entry_url == NULL
                || psp_ui_internal_url(entry_url);
            bool removed = !local_page
                && browser_profile_has_bookmark(
                       profile, entry_url);
            bool changed = !local_page
                && (removed
                        ? browser_profile_remove_bookmark(
                              profile, entry_url)
                        : browser_profile_add_bookmark(
                              profile, entry_url, entry.title));
            if (changed)
                psp_profile_store_mark_dirty(
                    &app->browser->profile_store, frame->ui_sample_us);
            if (changed && psp_profile_store_flush(&app->browser->profile_store)) {
                psp_ui_show_status(
                    &app->process->presentation.ui, removed ? "BOOKMARK REMOVED"
                                 : "BOOKMARK SAVED",
                    180);
            } else {
                psp_ui_show_status(
                    &app->process->presentation.ui, local_page ? "LOCAL PAGE NOT BOOKMARKED"
                                    : "BOOKMARK NOT UPDATED",
                    180);
            }
            break;
        }
        case PSP_UI_ACTION_SHOW_HOMEPAGE: {
            psp_text_input_before_navigation(&app->process->text_input);
            bool opened = psp_profile_open_page(
                engine, &app->process->presentation.ui, profile, PSP_PROFILE_PAGE_HOMEPAGE);
            frame->page_dirty = opened || frame->page_dirty;
            (void) psp_engine_views_refresh(app->views, engine);
            if (opened && tabs != NULL)
                (void) browser_tabs_capture_active(
                    tabs, app->views->navigation);
            psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
            psp_ui_show_status(
                &app->process->presentation.ui, opened ? "MY HOMEPAGE" : "HOMEPAGE UNAVAILABLE",
                180);
            break;
        }
        case PSP_UI_ACTION_SHOW_OFFLINE:
        case PSP_UI_ACTION_SHOW_DOWNLOADS:
        case PSP_UI_ACTION_SHOW_BOOKMARKS:
        case PSP_UI_ACTION_SHOW_HISTORY: {
            /*
             * All three are one native surface on a section. The
             * engine's HTML generators still exist for the host lab
             * and tests; the frontend simply stops routing to them,
             * so the chrome no longer waits on a document to show a
             * list it already has in memory.
             */
            PspUiCollectionSection section =
                psp_collections_action_section(
                    intent->action,
                    (PspUiCollectionSection)
                        app->process->presentation.ui.collections_section);
            if (section == PSP_UI_COLLECTION_SAVED
                || section == PSP_UI_COLLECTION_DOWNLOADS)
                (void) offline_library_load(&app->browser->offline_store.library);
            if (app->process->presentation.ui.screen != PSP_UI_SCREEN_COLLECTIONS)
                psp_ui_show_collections(&app->process->presentation.ui, section);
            psp_collections_sync_ui(
                &app->process->presentation.ui, &app->process->presentation.collections_surface, profile,
                &app->browser->offline_store, section);
            break;
        }
        case PSP_UI_ACTION_COLLECTION_ACTIVATE:
            psp_app_collection_activate(app, frame, intent, engine_frame);
            break;
        case PSP_UI_ACTION_COLLECTION_DELETE: {
            PspUiCollectionSection section =
                (PspUiCollectionSection) app->process->presentation.ui.collections_section;
            bool removed = false;
            bool uninstalling = false;
            if (section == PSP_UI_COLLECTION_SAVED
                || section == PSP_UI_COLLECTION_DOWNLOADS) {
                const OfflineLibraryItem *selected = offline_library_find(
                    &app->browser->offline_store.library,
                    app->process->presentation.collections_surface.id[
                        intent->list_index]);
                uninstalling = selected != NULL
                    && selected->type == OFFLINE_ITEM_WEB_APP;
                removed = offline_library_remove(
                    &app->browser->offline_store.library,
                    app->process->presentation.collections_surface.id[intent->list_index]);
                if (removed)
                    (void) offline_library_save(
                        &app->browser->offline_store.library);
            } else if (section == PSP_UI_COLLECTION_HISTORY) {
                const char *url = psp_collections_row_url(
                    &app->process->presentation.collections_surface, intent->list_index);
                removed = url != NULL
                    && browser_profile_forget_history(profile, url);
                if (removed) {
                    psp_profile_store_mark_dirty(
                        &app->browser->profile_store, frame->ui_sample_us);
                    (void) psp_profile_store_flush(&app->browser->profile_store);
                }
            }
            psp_collections_sync_ui(
                &app->process->presentation.ui, &app->process->presentation.collections_surface, profile,
                &app->browser->offline_store, section);
            psp_ui_show_status(
                &app->process->presentation.ui,
                removed ? (uninstalling ? "OFFLINE APP UNINSTALLED"
                                         : "DELETED")
                        : (uninstalling ? "APP COULD NOT BE UNINSTALLED"
                                        : "COULD NOT DELETE"),
                150);
            break;
        }
        case PSP_UI_ACTION_SCREENSHOT:
            psp_app_take_screenshot(app, engine_frame);
            break;
        case PSP_UI_ACTION_CHECK_WIFI_SIGN_IN:
            (void) psp_captive_portal_start(app, frame);
            break;
        case PSP_UI_ACTION_BUILD_DIAGNOSTIC_QR:
            psp_app_build_diagnostic_qr(app, frame);
            break;
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PREVIOUS:
            psp_app_step_diagnostic_qr(app, frame, -1);
            break;
        case PSP_UI_ACTION_DIAGNOSTIC_QR_NEXT:
            psp_app_step_diagnostic_qr(app, frame, 1);
            break;
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_PREVIOUS:
            psp_app_step_diagnostic_part(app, frame, -1);
            break;
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_NEXT:
            psp_app_step_diagnostic_part(app, frame, 1);
            break;
        case PSP_UI_ACTION_YOUTUBE_LOGIN_SAVE:
        case PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS:
        case PSP_UI_ACTION_YOUTUBE_LOGIN_NOT_NOW:
        case PSP_UI_ACTION_YOUTUBE_LOGIN_NEVER:
            psp_app_youtube_login_action(app, frame, intent);
            frame->page_dirty = true;
            break;
        case PSP_UI_ACTION_SHOW_SITE_STORAGE:
        case PSP_UI_ACTION_SITE_STORAGE_DELETE:
        case PSP_UI_ACTION_STORAGE_OFFER_SESSION:
        case PSP_UI_ACTION_STORAGE_OFFER_ALWAYS:
        case PSP_UI_ACTION_STORAGE_OFFER_DECLINE:
            psp_app_site_storage_action(app, frame, intent);
            frame->page_dirty = true;
            break;
        case PSP_UI_ACTION_HEAVY_RUN_SESSION:
        case PSP_UI_ACTION_HEAVY_RUN_ALWAYS:
        case PSP_UI_ACTION_HEAVY_CANCEL:
        case PSP_UI_ACTION_HEAVY_STOP_SCRIPTS:
            psp_app_heavy_action(app, frame, intent);
            frame->page_dirty = true;
            break;
        case PSP_UI_ACTION_CLOSE_DIAGNOSTIC_QR:
            psp_ui_set_diagnostic_qr(&app->process->presentation.ui, NULL);
            tilefinch_diagnostic_qr_destroy(app->process->diagnostic_qr);
            app->process->diagnostic_qr = NULL;
            frame->page_dirty = true;
            break;
        case PSP_UI_ACTION_POWER_TEST:
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        {
            PspClockWorkerSnapshot current_power = {0};
            if (app->process->clock_live)
                psp_clock_worker_snapshot(
                    &app->process->clock_worker, &current_power);
            uint64_t now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            if (!app->interactive->power_auto.active) {
                app->interactive->power_auto_boot_pending = true;
                app->process->presentation.ui.validation_power_test_phase = 1;
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "POWER TEST: WAITING FOR BROWSER TO BECOME IDLE",
                    900);
                printf(
                    "tilefinch-power-auto: event=queued "
                    "trigger=menu\n");
            } else {
                PspPowerTestResult result =
                    psp_power_test_finish(
                    &app->interactive->power_test, now_us,
                    app->process->clock_live ? &current_power : NULL,
                    "menu-abort");
                psp_power_auto_accumulate(
                    &app->interactive->power_auto, &result);
                app->interactive->power_auto.active = false;
                app->process->presentation.ui.validation_power_test_phase = 0;
                psp_power_auto_log_summary(
                    &app->interactive->power_auto, now_us, "menu-abort");
                psp_ui_show_status(
                    &app->process->presentation.ui, "POWER TEST STOPPED", 240);
            }
            break;
        }
#else
            break;
#endif
        case PSP_UI_ACTION_MEDIA_TEST:
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (app->interactive->media_stability_active) {
                app->interactive->media_stability_active = false;
                app->process->presentation.ui.validation_media_test_phase = 0;
                if (app->browser->media.playback != NULL)
                    media_playback_set_playing(
                        app->browser->media.playback, false);
                app->browser->media.ui.playing = false;
                psp_ui_show_status(
                    &app->process->presentation.ui, "VIDEO TEST STOPPED", 240);
                printf(
                    "tilefinch-media-stability: event=menu-abort "
                    "elapsed=%lluus\n",
                    (unsigned long long) (
                        app->interactive->media_stability_started_us == 0 ? 0
                        : (uint64_t) sceKernelGetSystemTimeWide()
                            - app->interactive->media_stability_started_us));
            } else {
                app->interactive->media_stability_active = true;
                app->interactive->media_stability_auto_exit = false;
                app->interactive->media_stability_started_us = 0;
                app->interactive->media_stability_next_sample_us = 0;
                app->interactive->media_stability_forward_seek = false;
                app->interactive->media_stability_backward_seek = false;
                app->interactive->media_stability_loops = 0;
                (*&app->interactive->media_stability_skew) =
                    (PspMediaStabilitySkew) {0};
                app->interactive->media_stability_min_free = UINT_MAX;
                app->interactive->media_stability_min_largest = UINT_MAX;
                app->interactive->media_stability_start_capacity = INT_MIN;
                app->interactive->media_stability_start_percent = INT_MIN;
                app->interactive->validation_media_play_injected = false;
                app->interactive->validation_media_play_confirmed = false;
                app->process->presentation.ui.validation_media_test_phase = 1;
                bool started = psp_begin_page_load(
                    engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                    PSP_MEDIA_STABILITY_URL, true,
                    PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
                if (started) {
                    app->interactive->navigation_job_started_us =
                        (uint64_t) sceKernelGetSystemTimeWide();
                    psp_ui_show_status(
                        &app->process->presentation.ui,
                        "2 MIN VIDEO TEST - CIRCLE STOPS",
                        900);
                    printf(
                        "tilefinch-media-stability: event=queued "
                        "quality=%up trigger=menu\n",
                        app->process->presentation.ui.youtube_240p ? 240u : 360u);
                } else {
                    app->interactive->media_stability_active = false;
                    app->process->presentation.ui.validation_media_test_phase = 0;
                    psp_ui_show_status(
                        &app->process->presentation.ui, "VIDEO TEST COULD NOT START", 240);
                }
            }
#endif
            break;
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL:
            (void) psp_app_edit_developer_update_url(app, frame);
            break;
        case PSP_UI_ACTION_SET_VIDEO_DECODER:
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            (void) psp_app_set_video_decoder(app);
#endif
            break;
        case PSP_UI_ACTION_EXIT:
            psp_exit_plan_request(
                &app->interactive->exit, PSP_EXIT_USER);
            break;
        case PSP_UI_ACTION_NONE:
        default:
            break;
    }
}

void psp_app_pump_provider_handoff_reclaim(
    PspApp *app, uint64_t frame_us, bool player_presented)
{
    if (app == NULL || app->interactive == NULL || app->browser == NULL
        || frame_us == 0) return;
    /* Direct Play cancels page image work immediately. A JPEG already inside
       entropy decode finishes below browser priority; reap its arena here on
       the first later frame instead of retaining it through playback. */
    (void) browser_engine_maintain_background_workers(
        app->browser->engine);
    if (app->interactive->provider_handoff_present_pending) {
        if (!player_presented) return;
        app->interactive->provider_handoff_present_pending = false;
    }
    BrowserOptionalMemoryReclaimJob *job =
        &app->interactive->provider_handoff_reclaim;
    if (!browser_engine_optional_memory_reclaim_pending(job)
        || app->interactive->provider_handoff_reclaim_pump_us == frame_us)
        return;
    app->interactive->provider_handoff_reclaim_pump_us = frame_us;
    bool complete = browser_engine_pump_optional_memory_reclaim(
        app->browser->engine, job);
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    (void) complete;
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-provider-handoff: phase=reclaim step=%u done=%d "
           "free=%zu target=%zu reclaimed=%zu/%zu/%zu/%zu\n",
           (unsigned) job->phase, complete ? 1 : 0,
           budget_remaining(app->browser->budget),
           job->target_remaining_bytes, job->reclaimed.font_staging_bytes,
           job->reclaimed.javascript_bytes,
           job->reclaimed.session_cache_bytes,
           job->reclaimed.render_cache_bytes);
#endif
}
