/* Executes PspUiIntent commands without reaching into the UI renderer's
 * internals. PspApp supplies borrowed canonical owners; PspAppFrameState
 * supplies values sampled for the current loop iteration.
 */
#include "psp_app_internal.h"
__attribute__((noinline))
static void psp_app_dispatch_heavy_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent);

typedef struct {
    TilefinchDiagnosticSource sources[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT];
    char paths[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT]
              [TILEFINCH_INSTALL_PATH_LIMIT];
} PspDiagnosticPathSet;

/* Keep the five long install paths and zlib/QR setup out of the already-large
   heavy action receiver's stack frame. The path set exists only for this
   explicit menu action and is released before the QR screen is presented. */
__attribute__((noinline))
static void psp_app_build_diagnostic_qr(
    PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || frame == NULL) return;
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
    (void) psp_log_flush(false);
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
    for (size_t at = 0; at < TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT; at++) {
        paths_valid = paths_valid && tilefinch_install_data_path(
            &app->process->install_paths, names[at],
            paths->paths[at], sizeof(paths->paths[at]));
        paths->sources[at].name = names[at];
        paths->sources[at].path = paths->paths[at];
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
        &metadata, paths->sources, TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT,
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

static bool psp_history_move(
    BrowserEngine *engine, bool forward, const NavigationEntry **entry)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    return forward ? navigation_forward(navigation, entry)
                   : navigation_back(navigation, entry);
}

static void psp_history_rollback(BrowserEngine *engine, bool forward)
{
    const NavigationEntry *ignored = NULL;
    (void) (forward
        ? navigation_back(browser_engine_navigation(engine), &ignored)
        : navigation_forward(browser_engine_navigation(engine), &ignored));
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
    const char *history_url)
{
    if (app == NULL || frame == NULL || history_url == NULL) return false;
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    bool root_compat = strcmp(history_url, "https://tilefinch.local") == 0
        || strcmp(history_url, "https://tilefinch.local/") == 0;
    bool home = root_compat || psp_ui_native_home_url(history_url);
    bool screenshots = strcmp(
        history_url, "https://tilefinch.local/screenshots") == 0;
    PspUiCollectionSection collection = PSP_UI_COLLECTION_OFFLINE;
    bool collections = psp_ui_legacy_collection_url(
        history_url, &collection);
    PspProfilePageKind profile_page = psp_profile_page_kind(history_url);

    if (!home && !screenshots && !collections
        && profile_page == PSP_PROFILE_PAGE_NONE) {
        if (!psp_ui_internal_url(history_url)) return false;
        psp_ui_show_status(&app->process->presentation.ui, "LOCAL PAGE UNAVAILABLE", 180);
        return true;
    }

    const NavigationEntry *moved_entry = NULL;
    if (!psp_history_move(engine, forward, &moved_entry)) {
        psp_ui_show_status(&app->process->presentation.ui, "HISTORY COULD NOT OPEN", 180);
        return true;
    }
    int restored_scroll = moved_entry == NULL ? 0 : moved_entry->scroll_y;
    bool opened = true;
    if (home) {
        psp_show_native_home(app);
    } else if (collections) {
        if (collection == PSP_UI_COLLECTION_OFFLINE)
            (void) offline_library_load(&app->browser->offline_store.library);
        psp_ui_show_collections(&app->process->presentation.ui, collection);
        psp_collections_sync_ui(
            &app->process->presentation.ui, &app->process->presentation.collections_surface, profile,
            &app->browser->offline_store.library, collection);
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
        psp_history_rollback(engine, forward);
        psp_ui_show_status(&app->process->presentation.ui, "LOCAL PAGE UNAVAILABLE", 180);
    } else if (!home && !collections) {
        (void) navigation_set_scroll(
            browser_engine_navigation(engine), restored_scroll);
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

/*
 * Keep the actions used while reading and spatially navigating out of the
 * large receiver below.  On Allegrex the full switch needs an 8 KiB stack
 * frame because its rare media/update/text-input arms carry large locals.
 * Sending every d-pad repeat through that frame needlessly spills registers
 * and pulls the cold receiver into the instruction cache.
 */
void psp_app_dispatch_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent)
{
    if (intent == NULL || intent->action == PSP_UI_ACTION_NONE) return;
    switch (intent->action) {
        case PSP_UI_ACTION_FOCUS_PREVIOUS:
            frame->page_dirty = browser_engine_focus_move(app->browser->engine, false)
                || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_NEXT:
            frame->page_dirty = browser_engine_focus_move(app->browser->engine, true)
                || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_AT:
            /* The d-pad press that hides the cursor also chooses the focus
               target, rather than jumping back to the previous one. */
            frame->page_dirty = browser_engine_focus_at(
                app->browser->engine, intent->pointer_x, intent->pointer_y)
                || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_UP:
            frame->page_dirty = browser_engine_focus_direction(
                app->browser->engine, CONTROLLER_FOCUS_UP) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_DOWN:
            frame->page_dirty = browser_engine_focus_direction(
                app->browser->engine, CONTROLLER_FOCUS_DOWN) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_LEFT:
            frame->page_dirty = browser_engine_focus_direction(
                app->browser->engine, CONTROLLER_FOCUS_LEFT) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_FOCUS_RIGHT:
            frame->page_dirty = browser_engine_focus_direction(
                app->browser->engine, CONTROLLER_FOCUS_RIGHT) || frame->page_dirty;
            return;
        case PSP_UI_ACTION_PAGE_UP:
            if (!psp_request_provisional_scroll(app->browser->engine, &app->process->presentation.ui, -1)) {
                frame->page_dirty = browser_engine_scroll_page(
                    app->browser->engine, -1) || frame->page_dirty;
            }
            return;
        case PSP_UI_ACTION_PAGE_DOWN:
            if (!psp_request_provisional_scroll(app->browser->engine, &app->process->presentation.ui, 1)) {
                frame->page_dirty = browser_engine_scroll_page(
                    app->browser->engine, 1) || frame->page_dirty;
            }
            return;
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
__attribute__((noinline))
static void psp_app_dispatch_heavy_action(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent)
{
    if (intent == NULL || intent->action == PSP_UI_ACTION_NONE) return;
    BrowserEngine *engine = app->browser->engine;
    Budget *budget = app->browser->budget;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    const uint16_t *engine_frame = app->views->frame;
    const char *tab_hibernation_path = app->process->storage.tab_hibernation;
    char *lifecycle_retry_url = app->interactive->lifecycle_retry_url;
    switch (intent->action) {
        case PSP_UI_ACTION_ACTIVATE:
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT: {
            bool enter_only =
                intent->action == PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT;
            if (app->views->controller == NULL) {
                psp_ui_show_status(&app->process->presentation.ui, "PAGE CONTROLS UNAVAILABLE",
                                   180);
                break;
            }
            if (app->views->controller->focus_kind == CONTROLLER_FOCUS_NONE) {
                frame->page_dirty =
                    browser_engine_focus_move(engine, true)
                    || frame->page_dirty;
                break;
            }
            ControllerTextInputInfo text_info = {0};
            bool focused_text =
                browser_engine_text_input_info(engine, &text_info)
                && text_info.editable;
            if (enter_only && (!focused_text || text_info.multiline)) {
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "ENTER IS NOT AVAILABLE HERE", 120);
                break;
            }
            bool submit_after_edit = false;
            if (focused_text && !enter_only) {
                if (frame->pointer_activation
                    && !browser_engine_pointer_commit_click(engine)) {
                    psp_ui_show_status(
                        &app->process->presentation.ui, "FIELD IS NO LONGER AVAILABLE", 120);
                    break;
                }
                (void) psp_engine_views_refresh(app->views, engine);
                text_info = (ControllerTextInputInfo) {0};
                if (!browser_engine_text_input_info(
                        engine, &text_info)
                    || !text_info.editable) {
                    break;
                }
                if (psp_replace_focused_text(
                        engine, engine_frame, &app->process->presentation.ui, &app->process->text_input,
                        false, &submit_after_edit)) {
                    frame->page_dirty = true;
                    (void) psp_engine_views_refresh(app->views, engine);
                }
                app->interactive->previous_buttons = 0;
                if (!submit_after_edit) break;
            }
            ControllerAction action;
            if (browser_engine_activate(engine, &action)) {
                bool navigates =
                    action.type == CONTROLLER_ACTION_NAVIGATE
                    || action.type
                         == CONTROLLER_ACTION_FORM_SUBMIT;
                if (action.type == CONTROLLER_ACTION_MEDIA) {
                    if (action.media_kind == MEDIA_DISCOVERY_WEBM) {
                        psp_ui_show_status(
                            &app->process->presentation.ui,
                            "VIDEO FORMAT COMING SOON", 180);
                        break;
                    }
                    if (action.media_audio_only
                        && action.media_kind == MEDIA_DISCOVERY_HLS) {
                        psp_ui_show_status(
                            &app->process->presentation.ui,
                            "AUDIO HLS FORMAT COMING SOON", 180);
                        break;
                    }
                    const NavigationEntry *entry =
                        navigation_current(app->views->navigation);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                    if (strcmp(app->process->config.trace, "none") == 0
                        && !psp_ensure_network_for_navigation(
                               app->network, app->network_lifecycle,
                               (int) app->process->config.network_profile,
                               "GET", action.url, true,
                               engine_frame,
                               &app->process->presentation.ui)) {
                        app->interactive->previous_buttons = psp_ui_buttons(
                            psp_navigation_observed_buttons());
                        break;
                    }
#endif
                    bool opened = entry != NULL
                        && (action.media_audio_only
                            ? psp_media_open_page_audio(
                                  &app->browser->media, action.url, entry->url,
                                  app->views->navigation->generation,
                                  action.media_node_handle, action.media_mode,
                                  action.media_credentials, true, false)
                            : action.media_kind == MEDIA_DISCOVERY_HLS
                            ? psp_media_open_page_hls(
                                  &app->browser->media, action.url, entry->url,
                                  app->views->navigation->generation,
                                  action.media_node_handle, action.media_mode,
                                  action.media_credentials, true, false)
                            : psp_media_open_page_source(
                                  &app->browser->media, action.url, entry->url,
                                  app->views->navigation->generation,
                                  action.media_node_handle, action.media_mode,
                                  action.media_credentials, true, false));
                    (void) browser_engine_update_media_state(
                        engine, action.media_node_handle,
                        opened ? SCRIPT_MEDIA_STATE_LOADING
                               : SCRIPT_MEDIA_STATE_ERROR,
                        0.0, 0.0);
                    if (!opened) {
                        psp_ui_show_status(
                            &app->process->presentation.ui,
                            action.media_audio_only
                                ? "AUDIO SOURCE COULD NOT OPEN"
                                : "VIDEO SOURCE COULD NOT OPEN", 180);
                    }
                    frame->page_dirty = true;
                    break;
                }
                if (navigates && action.prefer_native_media
                    && youtube_watch_url_supported(action.url)) {
                    const NavigationEntry *entry =
                        navigation_current(app->views->navigation);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                    if (strcmp(app->process->config.trace, "none") == 0
                        && !psp_ensure_network_for_navigation(
                               app->network, app->network_lifecycle,
                               (int) app->process->config.network_profile,
                               "GET", action.url, true, engine_frame,
                               &app->process->presentation.ui)) {
                        app->interactive->previous_buttons = psp_ui_buttons(
                            psp_navigation_observed_buttons());
                        break;
                    }
#endif
                    /* The result document remains the rollback surface. Its
                       optional thumbnails and script requests are no longer
                       useful while the player owns the screen, and retiring
                       them makes the resolver's first worker descriptor
                       available immediately. */
                    bool opened = entry != NULL
                        && psp_media_open_provider_route(
                            &app->browser->media, action.url,
                            app->views->navigation->generation);
                    if (opened) {
                        (void) browser_engine_cancel_network_work(
                            engine, "provider video selected");
                        psp_ui_set_loading(
                            &app->process->presentation.ui, false, 0);
                        frame->page_dirty = true;
                        break;
                    }
                    /* A synchronous admission refusal falls through to the
                       ordinary watch-page navigation. The visible Details
                       action always takes that path directly. */
                    action.prefer_native_media = false;
                }
                if (navigates
                    && psp_internal_action_url(
                           action.url, "address")) {
                    const NavigationEntry *entry =
                        navigation_current(app->views->navigation);
                    char destination[NAVIGATION_URL_LIMIT] = {0};
                    bool accepted = psp_request_omnibox(
                        &app->process->text_input, engine_frame, &app->process->presentation.ui, profile,
                        entry == NULL ? NULL : entry->url,
                        false, true,
                        destination, sizeof(destination));
                    app->interactive->previous_buttons = 0;
                    if (!accepted) break;
                    snprintf(
                        action.url, sizeof(action.url), "%s",
                        destination);
                    snprintf(
                        action.method, sizeof(action.method),
                        "%s", "GET");
                    action.body_length = 0;
                    action.body[0] = '\0';
                    action.content_type[0] = '\0';
                }
                /* `tilefinch://home` is the same destination spelled as an
                   internal action, which only a page's own link can emit;
                   it is not a URL any other navigation site can carry, so
                   it stays local rather than widening the shared
                   classifier. */
                if (navigates
                    && (psp_internal_action_url(action.url, "home")
                        || psp_ui_native_home_url(action.url))) {
                    psp_text_input_before_navigation(&app->process->text_input);
                    psp_show_native_home(app);
                    break;
                }
                if (navigates
                    && psp_internal_action_url(
                           action.url, "retry")) {
                    const NavigationEntry *entry =
                        navigation_current(app->views->navigation);
                    if (entry == NULL || entry->url == NULL
                        || entry->url[0] == '\0') {
                        psp_ui_show_status(
                            &app->process->presentation.ui, "Nothing to retry", 180);
                        break;
                    }
                    snprintf(
                        action.url, sizeof(action.url), "%s",
                        entry->url);
                    snprintf(
                        action.method, sizeof(action.method),
                        "%s", "GET");
                    action.body_length = 0;
                    action.body[0] = '\0';
                    action.content_type[0] = '\0';
                }
                if (navigates && psp_offline_url(action.url)) {
                    const NavigationEntry *source_entry =
                        navigation_current(app->views->navigation);
                    bool youtube_save = strncmp(
                            action.url,
                            "https://tilefinch.local/offline/youtube?id=",
                            strlen("https://tilefinch.local/offline/youtube?id="))
                        == 0;
                    bool library_root = strcmp(
                            action.url,
                            "https://tilefinch.local/offline") == 0;
                    bool offline_source = source_entry != NULL
                        && psp_offline_url(source_entry->url);
                    bool trusted_source = library_root
                        || offline_source
                        || (youtube_save && source_entry != NULL
                            && youtube_watch_url_supported(
                                   source_entry->url));
                    PspOfflineRouteResult offline_result =
                        trusted_source
                        ? psp_offline_store_handle_url(
                              &app->browser->offline_store, engine, action.url,
                              source_entry == NULL
                                  ? NULL : source_entry->title,
                              true)
                        : PSP_OFFLINE_ROUTE_ERROR;
                    if (!trusted_source)
                        psp_ui_show_status(
                            &app->process->presentation.ui, "OFFLINE SAVE LINK REFUSED", 180);
                    else
                        psp_ui_show_status(
                            &app->process->presentation.ui,
                            psp_offline_store_status(&app->browser->offline_store),
                            240);
                    if (offline_result != PSP_OFFLINE_ROUTE_NONE) {
                        app->process->presentation.ui.reader_mode = false;
                        (void) psp_engine_views_refresh(app->views, engine);
                        psp_sync_ui(&app->process->presentation.ui, engine, profile);
                        if (tabs != NULL)
                            (void) browser_tabs_capture_active(
                                tabs, app->views->navigation);
                        psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                        frame->page_dirty = offline_result
                            != PSP_OFFLINE_ROUTE_ERROR || frame->page_dirty;
                        break;
                    }
                }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                if (navigates
                    && strcmp(app->process->config.trace, "none") == 0
                    && !psp_ensure_network_for_navigation(
                           app->network, app->network_lifecycle,
                           (int) app->process->config.network_profile,
                           action.method, action.url, true,
                           engine_frame, &app->process->presentation.ui)) {
                    app->interactive->previous_buttons = psp_ui_buttons(
                        psp_navigation_observed_buttons());
                    break;
                }
#endif
                if (navigates) {
                    if (!browser_engine_set_javascript_enabled(
                            engine,
                            browser_profile_javascript_allowed_for_url(
                                profile, action.url))) {
                        psp_ui_show_status(
                            &app->process->presentation.ui,
                            "JAVASCRIPT POLICY UNAVAILABLE", 240);
                        break;
                    }
                    bool reader_navigation =
                        psp_reader_navigation_prepare(
                            engine, &app->process->presentation.ui, profile,
                            &app->interactive->reader_navigation,
                            action.url);
                    if (!reader_navigation) {
                        psp_leave_reader_for_navigation(
                            engine, &app->process->presentation.ui, profile,
                            action.url);
                    }
                    psp_ui_set_loading(&app->process->presentation.ui, true, -1);
                    psp_ui_show_status(
                        &app->process->presentation.ui,
                        reader_navigation
                            ? "LOADING READER PAGE"
                            : "LOADING - CIRCLE CANCELS",
                        reader_navigation ? 300 : 120);
                    psp_navigation_cooperate_begin(
                        &app->process->presentation.ui, engine_frame, engine);
                    psp_text_input_before_navigation(&app->process->text_input);
                }
                bool action_ok = action.type
                        == CONTROLLER_ACTION_NONE
                    || (navigates
                        ? psp_retry_navigation_action_after_reclaim(
                              engine, &action, 4 * MIB, 30000)
                        : browser_engine_execute_action(
                              engine, &action, 4 * MIB, 30000));
                if (action_ok) {
                    if (navigates) {
                        app->interactive->navigation_job_started_us =
                            (uint64_t) sceKernelGetSystemTimeWide();
                    } else {
                        (void) psp_engine_views_refresh(app->views, engine);
                        frame->page_dirty = true;
                    }
                } else {
                    if (navigates) {
                        (void) psp_engine_views_refresh(app->views, engine);
                        const NavigationEntry *incumbent =
                            navigation_current(app->views->navigation);
                        psp_reader_navigation_finish(
                            engine, &app->process->presentation.ui, profile,
                            &app->interactive->reader_navigation,
                            incumbent == NULL ? NULL : incumbent->url,
                            false);
                        (void) psp_engine_views_refresh(app->views, engine);
                        (void) psp_write_failure_report(
                            "navigation-action-start",
                            browser_engine_last_error(engine),
                            action.url, 0, 0);
                        psp_report_job_failure(
                            "navigation-action-start",
                            "interactive-action-start-failed",
                            -1, 0,
                            browser_engine_last_error(engine));
                        psp_navigation_cooperate_end(
                            "interactive-action-start-failed");
                        psp_ui_set_loading(
                            &app->process->presentation.ui, false, 0);
                    }
                    psp_ui_show_status(
                        &app->process->presentation.ui, browser_engine_last_error(engine), 300);
                }
            }
            break;
        }
        case PSP_UI_ACTION_BACK:
        case PSP_UI_ACTION_FORWARD: {
            bool forward = intent->action == PSP_UI_ACTION_FORWARD;
            const char *history_url = NULL;
            if (app->views->navigation->history_count != 0) {
                size_t target = app->views->navigation->history_index;
                if (forward && target + 1u
                                   < app->views->navigation->history_count) {
                    target++;
                    history_url = app->views->navigation->history[target].url;
                } else if (!forward && target > 0) {
                    target--;
                    history_url = app->views->navigation->history[target].url;
                }
            }
            if (psp_history_open_internal(
                    app, frame, forward, history_url)) {
                break;
            }
            if (history_url != NULL
                && psp_offline_url(history_url)) {
                NavigationSession *mutable_navigation =
                    browser_engine_navigation(engine);
                const NavigationEntry *moved_entry = NULL;
                bool moved = forward
                    ? navigation_forward(
                          mutable_navigation, &moved_entry)
                    : navigation_back(
                          mutable_navigation, &moved_entry);
                int restored_scroll = moved_entry == NULL
                    ? 0 : moved_entry->scroll_y;
                PspOfflineRouteResult result = moved
                    ? psp_offline_store_handle_url(
                          &app->browser->offline_store, engine, history_url,
                          moved_entry == NULL
                              ? NULL : moved_entry->title,
                          false)
                    : PSP_OFFLINE_ROUTE_ERROR;
                if (result == PSP_OFFLINE_ROUTE_ERROR && moved) {
                    const NavigationEntry *ignored = NULL;
                    (void) (forward
                        ? navigation_back(
                              mutable_navigation, &ignored)
                        : navigation_forward(
                              mutable_navigation, &ignored));
                } else if (result == PSP_OFFLINE_ROUTE_PAGE) {
                    (void) navigation_set_scroll(
                        mutable_navigation, restored_scroll);
                }
                app->process->presentation.ui.reader_mode = false;
                (void) psp_engine_views_refresh(app->views, engine);
                psp_sync_ui(&app->process->presentation.ui, engine, profile);
                if (result == PSP_OFFLINE_ROUTE_PAGE && tabs != NULL)
                    (void) browser_tabs_capture_active(
                        tabs, app->views->navigation);
                psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                frame->page_dirty = result == PSP_OFFLINE_ROUTE_PAGE
                    || frame->page_dirty;
                psp_ui_show_status(
                    &app->process->presentation.ui, psp_offline_store_status(&app->browser->offline_store),
                    180);
                break;
            }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (history_url != NULL
                && strcmp(app->process->config.trace, "none") == 0
                && !psp_ensure_network_for_navigation(
                       app->network, app->network_lifecycle,
                       (int) app->process->config.network_profile,
                       "GET", history_url, true, engine_frame, &app->process->presentation.ui)) {
                app->interactive->previous_buttons = psp_ui_buttons(
                    psp_navigation_observed_buttons());
                break;
            }
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
            bool started = browser_engine_begin_navigation_history(
                engine, forward, 4 * MIB, 30000);
            if (started) {
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
            break;
        }
        case PSP_UI_ACTION_VOICE_FOCUSED_TEXT:
            if (psp_replace_focused_text(
                    engine, engine_frame, &app->process->presentation.ui,
                    &app->process->text_input, true, NULL)) {
                frame->page_dirty = true;
                (void) psp_engine_views_refresh(app->views, engine);
            }
            app->interactive->previous_buttons = 0;
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
            uint32_t observed_buttons =
                psp_ui_buttons(psp_navigation_observed_buttons());
            psp_navigation_cooperate_end("offline-article");
            app->interactive->previous_buttons = observed_buttons;
            psp_ui_show_status(
                &app->process->presentation.ui, cancelled ? "ARTICLE SAVE STOPPED"
                    : psp_offline_store_status(&app->browser->offline_store),
                saved ? 180 : (cancelled ? 120 : 240));
            break;
        }
        case PSP_UI_ACTION_SHOW_SCREENSHOTS: {
            if (app->process->presentation.ui.screen
                    == PSP_UI_SCREEN_COLLECTIONS) {
                psp_collections_sync_ui(
                    &app->process->presentation.ui,
                    &app->process->presentation.collections_surface,
                    profile, &app->browser->offline_store.library,
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
        case PSP_UI_ACTION_RELOAD: {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            char reload_url[NAVIGATION_URL_LIMIT];
            snprintf(reload_url, sizeof(reload_url), "%s",
                     app->interactive->lifecycle_retry_available
                         ? lifecycle_retry_url
                         : (entry == NULL ? app->process->config.url : entry->url));
            bool resumed_navigation = app->interactive->lifecycle_retry_available;
            PspProfilePageKind profile_page =
                psp_profile_page_kind(reload_url);
            if (psp_route_native_home(app, reload_url)) {
                app->interactive->lifecycle_retry_available = false;
                break;
            }
            if (strcmp(
                    reload_url,
                    "https://tilefinch.local/screenshots") == 0) {
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
                if (opened) app->interactive->lifecycle_retry_available = false;
                break;
            }
            if (profile_page != PSP_PROFILE_PAGE_NONE) {
                psp_text_input_before_navigation(&app->process->text_input);
                bool opened = psp_profile_open_page(
                    engine, &app->process->presentation.ui, profile, profile_page);
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
                if (opened) app->interactive->lifecycle_retry_available = false;
                break;
            }
            if (psp_offline_url(reload_url)) {
                PspOfflineRouteResult result =
                    psp_offline_store_handle_url(
                        &app->browser->offline_store, engine, reload_url, NULL,
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
                    &app->process->presentation.ui, psp_offline_store_status(&app->browser->offline_store),
                    180);
                if (result == PSP_OFFLINE_ROUTE_PAGE)
                    app->interactive->lifecycle_retry_available = false;
                break;
            }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (strcmp(app->process->config.trace, "none") == 0
                && !psp_ensure_network_for_navigation(
                       app->network, app->network_lifecycle,
                       (int) app->process->config.network_profile,
                       "GET", reload_url, true, engine_frame, &app->process->presentation.ui)) {
                app->interactive->previous_buttons = psp_ui_buttons(
                    psp_navigation_observed_buttons());
                break;
            }
#endif
            bool started = psp_begin_page_load(
                engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                reload_url, false,
                4 * MIB, 30000);
            if (started) {
                app->interactive->lifecycle_retry_available = false;
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            }
            if (started && resumed_navigation)
                psp_ui_show_status(
                    &app->process->presentation.ui, "RETRYING INTERRUPTED PAGE", 180);
            break;
        }
        case PSP_UI_ACTION_TOGGLE_READER: {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            const char *reader_url =
                entry == NULL ? NULL : entry->url;
            bool old_mode = app->process->presentation.ui.reader_mode;
            unsigned old_percent = app->process->presentation.ui.page_font_percent;
            bool enable = !old_mode;
            unsigned target_percent =
                browser_profile_page_font_percent(profile);
            char reader_anchor[BROWSER_TEXT_ANCHOR_LIMIT + 1u];
            int reader_anchor_y = 0;
            bool have_reader_anchor =
                browser_engine_capture_text_anchor(
                    engine, reader_anchor, &reader_anchor_y);
            ReaderDocumentAnalysis reader_analysis = {0};
            if (enable && !browser_engine_prepare_reader(
                    engine, &reader_analysis)) {
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "READER MODE UNAVAILABLE", 180);
                break;
            }
            if (enable && app->process->presentation.ui.remember_reader_site_scale) {
                (void) browser_profile_reader_site_font_percent(
                    profile, reader_url, &target_percent);
            }
            if (psp_set_presentation_css(
                    engine, &app->process->presentation.ui, profile, enable, reader_url,
                    target_percent, true)) {
                app->process->presentation.ui.reader_mode = enable;
                browser_engine_set_reader_candidate_mode(engine, enable);
                app->process->presentation.ui.page_font_percent = target_percent;
                (void) psp_engine_views_refresh(app->views, engine);
                if (have_reader_anchor)
                    (void) browser_engine_restore_text_anchor(
                        engine, reader_anchor, reader_anchor_y);
                frame->page_dirty = true;
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    enable ? "READER MODE ON" : "READER MODE OFF",
                    180);
            } else {
                (void) psp_set_presentation_css(
                    engine, &app->process->presentation.ui, profile, old_mode, reader_url,
                    old_percent, true);
                app->process->presentation.ui.reader_mode = old_mode;
                app->process->presentation.ui.page_font_percent = old_percent;
                psp_ui_show_status(
                    &app->process->presentation.ui, enable ? "READER MODE UNAVAILABLE"
                                : "READER MODE COULD NOT CLOSE",
                    240);
            }
            break;
        }
        case PSP_UI_ACTION_TOGGLE_READER_SITE: {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            const char *url = entry == NULL ? app->process->presentation.ui.url : entry->url;
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
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            char destination[NAVIGATION_URL_LIMIT] = {0};
            const char *current_url =
                entry == NULL ? app->process->config.url : entry->url;
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
                && psp_ui_native_home_url(destination);
            if (native_home) {
                psp_text_input_before_navigation(&app->process->text_input);
                (void) psp_route_native_home(app, destination);
            } else if (accepted) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                    if (strcmp(app->process->config.trace, "none") == 0
                        && !psp_ensure_network_for_navigation(
                               app->network, app->network_lifecycle,
                               (int) app->process->config.network_profile,
                               "GET", destination, true,
                               engine_frame, &app->process->presentation.ui)) {
                        app->interactive->previous_buttons = psp_ui_buttons(
                            psp_navigation_observed_buttons());
                        break;
                    }
#endif
                    bool started = psp_begin_page_load(
                        engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                        destination, true,
                        4 * MIB, 30000);
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
            if (strcmp(app->process->config.trace, "none") == 0
                && !psp_ensure_network_for_navigation(
                       app->network, app->network_lifecycle,
                       (int) app->process->config.network_profile,
                       "GET", destination, true, engine_frame, &app->process->presentation.ui)) {
                app->interactive->previous_buttons = psp_ui_buttons(
                    psp_navigation_observed_buttons());
                break;
            }
#endif
            psp_ui_leave_native_surface(&app->process->presentation.ui);
            bool started = psp_begin_page_load(
                engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                destination, true, 4 * MIB, 30000);
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
        case PSP_UI_ACTION_CLOSE_TAB: {
            const char *homepage_url =
                browser_profile_custom_homepage_enabled(profile)
                ? BROWSER_PROFILE_HOMEPAGE_URL
                : TILEFINCH_HOMEPAGE_URL;
            char destination[NAVIGATION_URL_LIMIT] = {0};
            PspTabRequestResult result = psp_tabs_request(
                tabs, app->views->navigation,
                !psp_ui_screen_is_native_surface(
                    (PspUiScreen) app->process->presentation.ui.base_screen),
                intent->action,
                intent->tab_index, homepage_url,
                tab_hibernation_path, &app->interactive->tab_transition, destination);
            if (result != PSP_TAB_REQUEST_REFUSED
                && result != PSP_TAB_REQUEST_HIBERNATION_FAILED) {
                if (intent->action == PSP_UI_ACTION_CLOSE_TAB)
                    psp_tabs_remove_thumbnail(
                        &app->process->presentation.tab_view, intent->tab_index);
                else if (intent->action == PSP_UI_ACTION_NEW_TAB)
                    psp_tabs_invalidate_thumbnail(
                        &app->process->presentation.tab_view,
                        browser_tabs_active_index(tabs));
            }
            if (result == PSP_TAB_REQUEST_HIBERNATION_FAILED) {
                psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                psp_ui_show_status(
                    &app->process->presentation.ui, "HIBERNATED TAB COULD NOT OPEN", 240);
            } else if (result == PSP_TAB_REQUEST_REFUSED) {
                psp_ui_show_status(
                    &app->process->presentation.ui, tabs == NULL
                        ? "TABS UNAVAILABLE"
                        : (browser_tabs_count(tabs) >=
                                   BROWSER_TAB_LIMIT
                               && intent->action
                                      == PSP_UI_ACTION_NEW_TAB
                               ? "FIVE TAB LIMIT"
                               : "KEEP ONE TAB OPEN"),
                    180);
            } else if (result == PSP_TAB_REQUEST_COMPLETE) {
                psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    intent->action == PSP_UI_ACTION_CLOSE_TAB
                        ? "TAB CLOSED" : "TAB ALREADY OPEN",
                    120);
            } else {
                PspProfilePageKind profile_page =
                    psp_profile_page_kind(destination);
                PspUiCollectionSection legacy_section;
                if (psp_ui_native_home_url(destination)) {
                    bool opened = psp_tabs_finish_native_home(
                        tabs, &app->interactive->tab_transition,
                        browser_profile_tab_hibernation_enabled(profile),
                        tab_hibernation_path);
                    /* The tab transition has to settle first, so this arm
                       shows HOME only on success and syncs the strip either
                       way; psp_show_native_home covers the success half. */
                    if (opened) psp_show_native_home(app);
                    else psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                    psp_ui_show_status(
                        &app->process->presentation.ui, opened ? "HOME" : "TAB COULD NOT OPEN",
                        120);
                } else if (psp_ui_legacy_collection_url(
                        destination, &legacy_section)) {
                    /* A restored tab still pointing at a pre-upgrade
                       collections URL opens the native surface, not
                       the retired HTML generator. Settle the tab
                       transition as a non-load and show COLLECTIONS
                       on the section with its rows synced, the same
                       show/sync pair the menu's SHOW_* actions use. */
                    (void) psp_tabs_finish(
                        tabs, engine, &app->interactive->tab_transition, false,
                        browser_profile_tab_hibernation_enabled(
                            profile),
                        tab_hibernation_path);
                    if (legacy_section == PSP_UI_COLLECTION_OFFLINE)
                        (void) offline_library_load(
                            &app->browser->offline_store.library);
                    if (app->process->presentation.ui.screen != PSP_UI_SCREEN_COLLECTIONS)
                        psp_ui_show_collections(&app->process->presentation.ui, legacy_section);
                    psp_collections_sync_ui(
                        &app->process->presentation.ui, &app->process->presentation.collections_surface, profile,
                        &app->browser->offline_store.library, legacy_section);
                    (void) psp_engine_views_refresh(app->views, engine);
                    psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                } else if (profile_page != PSP_PROFILE_PAGE_NONE) {
                    psp_text_input_before_navigation(&app->process->text_input);
                    bool opened = psp_profile_open_page(
                        engine, &app->process->presentation.ui, profile, profile_page);
                    if (opened)
                        psp_ui_leave_native_surface(&app->process->presentation.ui);
                    (void) psp_engine_views_refresh(app->views, engine);
                    bool restored = psp_tabs_finish(
                        tabs, engine, &app->interactive->tab_transition, opened,
                        browser_profile_tab_hibernation_enabled(
                            profile),
                        tab_hibernation_path);
                    (void) psp_engine_views_refresh(app->views, engine);
                    frame->page_dirty = opened || frame->page_dirty;
                    psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                    psp_ui_show_status(
                        &app->process->presentation.ui, opened && restored
                            ? "TAB READY" : "TAB COULD NOT OPEN",
                        180);
                } else if (psp_offline_url(destination)) {
                    psp_text_input_before_navigation(&app->process->text_input);
                    psp_leave_reader_for_navigation(
                        engine, &app->process->presentation.ui, profile, destination);
                    PspOfflineRouteResult offline_result =
                        psp_offline_store_handle_url(
                            &app->browser->offline_store, engine, destination,
                            NULL, false);
                    bool opened =
                        offline_result == PSP_OFFLINE_ROUTE_PAGE;
                    if (opened)
                        psp_ui_leave_native_surface(&app->process->presentation.ui);
                    (void) psp_engine_views_refresh(app->views, engine);
                    bool restored = psp_tabs_finish(
                        tabs, engine, &app->interactive->tab_transition, opened,
                        browser_profile_tab_hibernation_enabled(
                            profile),
                        tab_hibernation_path);
                    (void) psp_engine_views_refresh(app->views, engine);
                    psp_sync_ui(&app->process->presentation.ui, engine, profile);
                    frame->page_dirty = opened || frame->page_dirty;
                    psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                    psp_ui_show_status(
                        &app->process->presentation.ui, opened && restored
                            ? "TAB READY" : "TAB COULD NOT OPEN",
                        180);
                } else {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                    if (strcmp(app->process->config.trace, "none") == 0
                        && !psp_ensure_network_for_navigation(
                               app->network, app->network_lifecycle,
                               (int) app->process->config.network_profile,
                               "GET", destination, true,
                               engine_frame, &app->process->presentation.ui)) {
                        (void) psp_tabs_finish(
                            tabs, engine, &app->interactive->tab_transition, false,
                            browser_profile_tab_hibernation_enabled(
                                profile),
                            tab_hibernation_path);
                        psp_tabs_sync_ui(
                            &app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                        app->interactive->previous_buttons = psp_ui_buttons(
                            psp_navigation_observed_buttons());
                        break;
                    }
#endif
                    bool started = psp_begin_page_load(
                        engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                        destination, false, 4 * MIB, 30000);
                    if (started) {
                        psp_ui_leave_native_surface(&app->process->presentation.ui);
                        app->interactive->navigation_job_started_us =
                            (uint64_t) sceKernelGetSystemTimeWide();
                    } else {
                        (void) psp_tabs_finish(
                            tabs, engine, &app->interactive->tab_transition, false,
                            browser_profile_tab_hibernation_enabled(
                                profile),
                            tab_hibernation_path);
                        psp_tabs_sync_ui(
                            &app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                    }
                }
            }
            break;
        }
        case PSP_UI_ACTION_TOGGLE_BOOKMARK: {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            bool local_page = entry == NULL || entry->url == NULL
                || strncmp(
                       entry->url, "https://tilefinch.local/",
                       strlen("https://tilefinch.local/")) == 0;
            bool removed = !local_page
                && browser_profile_has_bookmark(
                       profile, entry->url);
            bool changed = !local_page
                && (removed
                        ? browser_profile_remove_bookmark(
                              profile, entry->url)
                        : browser_profile_add_bookmark(
                              profile, entry->url, entry->title));
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
                &app->browser->offline_store.library, section);
            break;
        }
        case PSP_UI_ACTION_COLLECTION_ACTIVATE: {
            if (app->process->presentation.ui.collections_section
                    == PSP_UI_COLLECTION_SCREENSHOTS) {
                psp_text_input_before_navigation(&app->process->text_input);
                psp_leave_reader_for_navigation(
                    engine, &app->process->presentation.ui, profile,
                    "https://tilefinch.local/screenshots");
                bool opened = psp_open_screenshot_list(
                    engine, app->process->install_paths.data_dir, true);
                if (opened)
                    psp_ui_leave_native_surface(
                        &app->process->presentation.ui);
                (void) psp_engine_views_refresh(app->views, engine);
                psp_sync_ui(
                    &app->process->presentation.ui, engine, profile);
                if (opened && tabs != NULL)
                    (void) browser_tabs_capture_active(
                        tabs, app->views->navigation);
                psp_tabs_sync_ui(
                    &app->process->presentation.ui, tabs,
                    &app->process->presentation.tab_view);
                frame->page_dirty = opened || frame->page_dirty;
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    opened ? "SCREENSHOTS" : "SCREENSHOTS UNAVAILABLE",
                    180);
                break;
            }
            const char *destination = psp_collections_row_url(
                &app->process->presentation.collections_surface, intent->list_index);
            if (destination == NULL) {
                psp_ui_show_status(&app->process->presentation.ui, "NOTHING TO OPEN", 120);
                break;
            }
            if (app->process->presentation.ui.collections_section
                    == PSP_UI_COLLECTION_SAVED
                || app->process->presentation.ui.collections_section
                    == PSP_UI_COLLECTION_DOWNLOADS) {
                char offline_url[NAVIGATION_URL_LIMIT];
                const OfflineLibraryItem *item =
                    offline_library_find(
                        &app->browser->offline_store.library,
                        app->process->presentation.collections_surface.id[intent->list_index]);
                snprintf(
                    offline_url, sizeof(offline_url),
                    "https://tilefinch.local/offline/%s?id=%u",
                    item != NULL && item->type == OFFLINE_ITEM_YOUTUBE
                        ? "video" : "article",
                    (unsigned)
                        app->process->presentation.collections_surface.id[intent->list_index]);
                psp_text_input_before_navigation(&app->process->text_input);
                psp_ui_leave_native_surface(&app->process->presentation.ui);
                psp_leave_reader_for_navigation(
                    engine, &app->process->presentation.ui, profile, offline_url);
                PspOfflineRouteResult offline_result =
                    psp_offline_store_handle_url(
                        &app->browser->offline_store, engine, offline_url, NULL,
                        true);
                (void) psp_engine_views_refresh(app->views, engine);
                psp_sync_ui(&app->process->presentation.ui, engine, profile);
                bool offline_opened =
                    offline_result == PSP_OFFLINE_ROUTE_PAGE;
                if (offline_opened && tabs != NULL)
                    (void) browser_tabs_capture_active(
                        tabs, app->views->navigation);
                psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
                frame->page_dirty = offline_opened || frame->page_dirty;
                if (!offline_opened)
                    psp_ui_show_collections(
                        &app->process->presentation.ui, (PspUiCollectionSection)
                                 app->process->presentation.ui.collections_section);
                psp_ui_show_status(
                    &app->process->presentation.ui, psp_offline_store_status(&app->browser->offline_store),
                    180);
                break;
            }
            char destination_copy[NAVIGATION_URL_LIMIT];
            snprintf(destination_copy, sizeof(destination_copy),
                     "%s", destination);
            /* A bookmark or history row saved before the start page became
               native chrome still holds its URL. Route it to the surface
               instead of fetching a host that no longer answers. The copy
               is made first because showing HOME rebuilds the surfaces the
               row pointer came from. */
            if (psp_route_native_home(app, destination_copy)) break;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (strcmp(app->process->config.trace, "none") == 0
                && !psp_ensure_network_for_navigation(
                       app->network, app->network_lifecycle,
                       (int) app->process->config.network_profile,
                       "GET", destination_copy, true,
                       engine_frame, &app->process->presentation.ui)) {
                app->interactive->previous_buttons = psp_ui_buttons(
                    psp_navigation_observed_buttons());
                break;
            }
#endif
            psp_ui_leave_native_surface(&app->process->presentation.ui);
            if (psp_begin_page_load(
                    engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                    destination_copy, true, 4 * MIB, 30000)) {
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            } else {
                psp_ui_show_collections(
                    &app->process->presentation.ui, (PspUiCollectionSection)
                             app->process->presentation.ui.collections_section);
            }
            break;
        }
        case PSP_UI_ACTION_COLLECTION_DELETE: {
            PspUiCollectionSection section =
                (PspUiCollectionSection) app->process->presentation.ui.collections_section;
            bool removed = false;
            if (section == PSP_UI_COLLECTION_SAVED
                || section == PSP_UI_COLLECTION_DOWNLOADS) {
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
                &app->browser->offline_store.library, section);
            psp_ui_show_status(
                &app->process->presentation.ui, removed ? "DELETED" : "COULD NOT DELETE", 150);
            break;
        }
        case PSP_UI_ACTION_SCREENSHOT: {
            if (app->interactive->screenshot.writer.status
                    == SCREENSHOT_PNG_PENDING) {
                psp_ui_show_status(
                    &app->process->presentation.ui, "SCREENSHOT ALREADY SAVING", 180);
                break;
            }
            char destination[SCREENSHOT_PNG_PATH_CAPACITY] = {0};
            PspScreenshotDestination destination_status =
                psp_screenshot_destination(
                    app->process->install_paths.data_dir, destination);
            if (destination_status
                    == PSP_SCREENSHOT_DESTINATION_FULL) {
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    "MEMORY STICK FULL - SCREENSHOT NOT SAVED",
                    240);
                break;
            }
            if (destination_status
                    != PSP_SCREENSHOT_DESTINATION_OK) {
                psp_ui_show_status(
                    &app->process->presentation.ui, "SCREENSHOT FOLDER UNAVAILABLE", 240);
                break;
            }
            if (!psp_present_internal(engine_frame, &app->process->presentation.ui, true)) {
                psp_ui_show_status(
                    &app->process->presentation.ui, "SCREENSHOT VIEW UNAVAILABLE", 240);
                break;
            }
            const uint16_t *visible =
                psp_display_front_buffer(&psp_display);
            size_t screenshot_pixel_count =
                (size_t) PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT;
            app->interactive->screenshot.pixels = budget_malloc_category(
                budget, BUDGET_CATEGORY_RENDER,
                screenshot_pixel_count * sizeof(*app->interactive->screenshot.pixels));
            if (visible == NULL || app->interactive->screenshot.pixels == NULL) {
                budget_free(budget, app->interactive->screenshot.pixels);
                app->interactive->screenshot.pixels = NULL;
                psp_ui_show_status(
                    &app->process->presentation.ui, "NOT ENOUGH MEMORY FOR SCREENSHOT", 240);
                break;
            }
            for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
                memcpy(
                    app->interactive->screenshot.pixels
                        + (size_t) y * PSP_SCREEN_WIDTH,
                    visible + (size_t) y * PSP_VRAM_STRIDE,
                    PSP_SCREEN_WIDTH * sizeof(*app->interactive->screenshot.pixels));
            }
            if (!screenshot_png_begin(
                    &app->interactive->screenshot.writer, destination,
                    app->interactive->screenshot.pixels, PSP_SCREEN_WIDTH,
                    PSP_SCREEN_HEIGHT, PSP_SCREEN_WIDTH)) {
                printf(
                    "tilefinch-screenshot: event=begin-failed "
                    "error=\"%s\"\n",
                    screenshot_png_error(&app->interactive->screenshot.writer));
                screenshot_png_cancel(&app->interactive->screenshot.writer);
                budget_free(budget, app->interactive->screenshot.pixels);
                app->interactive->screenshot.pixels = NULL;
                psp_ui_show_status(
                    &app->process->presentation.ui, "SCREENSHOT COULD NOT START", 240);
                break;
            }
            app->interactive->screenshot.reported_tenth = 0;
            psp_ui_show_status(
                &app->process->presentation.ui, "SAVING SCREENSHOT 0%", 240);
            printf(
                "tilefinch-screenshot: event=start path=\"%s\" "
                "bytes=%zu\n",
                destination,
                screenshot_pixel_count
                    * sizeof(*app->interactive->screenshot.pixels));
            break;
        }
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
                    4 * MIB, 30000);
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
