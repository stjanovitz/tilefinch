#include "psp_app_internal.h"

#include "tilefinch/user_agent.h"

#define PSP_CAPTIVE_PORTAL_PAGE_LIMIT (512u * 1024u)
#define PSP_CAPTIVE_PORTAL_PAGE_TIMEOUT_MS 20000L
#define PSP_CAPTIVE_PORTAL_PROBE_TIMEOUT_MS 8000L
#define PSP_CAPTIVE_PORTAL_SESSION_LIMIT_US UINT64_C(300000000)

typedef enum {
    PSP_CAPTIVE_PORTAL_PROBING = 0,
    PSP_CAPTIVE_PORTAL_OPENING,
    PSP_CAPTIVE_PORTAL_ACTIVE,
    PSP_CAPTIVE_PORTAL_VERIFYING,
    PSP_CAPTIVE_PORTAL_RETURN_PENDING,
    PSP_CAPTIVE_PORTAL_RETURNING
} PspCaptivePortalPhase;

struct PspCaptivePortal {
    PspCaptivePortalPhase phase;
    uint64_t request_id;
    size_t body_length;
    size_t previous_tab;
    size_t portal_tab;
    unsigned probe_count;
    uint64_t started_us;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    bool resume_update_check;
#endif
    unsigned char body[TILEFINCH_CAPTIVE_PORTAL_PROBE_BODY_LIMIT];
    char original_url[NAVIGATION_URL_LIMIT];
    char portal_url[NAVIGATION_URL_LIMIT];
};

static bool portal_header_value(
    const FetchResult *result, const char *name,
    char *output, size_t output_size)
{
    if (output != NULL && output_size != 0) output[0] = '\0';
    if (result == NULL || name == NULL || output == NULL || output_size == 0)
        return false;
    size_t name_length = strlen(name);
    size_t offset = 0;
    while (offset < result->response_headers_length) {
        const char *line = result->response_headers + offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - offset);
        size_t length = newline == NULL
            ? result->response_headers_length - offset
            : (size_t) (newline - line);
        if (length != 0 && line[length - 1u] == '\r') length--;
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && (size_t) (colon - line) == name_length
            && strncasecmp(line, name, name_length) == 0) {
            const char *value = colon + 1;
            const char *end = line + length;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t wanted = (size_t) (end - value);
            if (wanted >= output_size) return false;
            memcpy(output, value, wanted);
            output[wanted] = '\0';
            return true;
        }
        if (newline == NULL) break;
        offset = (size_t) (newline - result->response_headers) + 1u;
    }
    return false;
}

static void portal_release(PspApp *app)
{
    if (app == NULL || app->interactive == NULL
        || app->interactive->captive_portal == NULL) return;
    PspCaptivePortal *portal = app->interactive->captive_portal;
    if (portal->request_id != 0) {
        (void) fetch_background_transport_cancel(
            portal->request_id, "captive portal operation ended");
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (portal->resume_update_check && app->update_check_pending != NULL
        && (app->update_check_running == NULL
            || !*app->update_check_running)) {
        *app->update_check_pending = true;
    }
#endif
    budget_free(app->browser->budget, portal);
    app->interactive->captive_portal = NULL;
}

bool psp_captive_portal_active(const PspInteractiveState *interactive)
{
    return interactive != NULL && interactive->captive_portal != NULL;
}

static bool portal_probe_begin(PspApp *app, PspCaptivePortal *portal,
                               bool verifying)
{
    FetchRequest request = {
        .method = "GET",
        .accept = "text/html,application/xhtml+xml;q=0.9,*/*;q=0.1",
        .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_OMIT,
        .identity_encoding = true,
        .user_agent = TILEFINCH_BROWSER_USER_AGENT
    };
    portal->request_id = fetch_background_transport_enqueue_hop_stream(
        TILEFINCH_CAPTIVE_PORTAL_PROBE_URL, &request,
        TILEFINCH_CAPTIVE_PORTAL_PROBE_BODY_LIMIT,
        PSP_CAPTIVE_PORTAL_PROBE_TIMEOUT_MS);
    if (portal->request_id == 0) return false;
    portal->body_length = 0;
    portal->phase = verifying ? PSP_CAPTIVE_PORTAL_VERIFYING
                              : PSP_CAPTIVE_PORTAL_PROBING;
    portal->probe_count++;
    psp_ui_show_status(
        &app->process->presentation.ui,
        verifying ? "CHECKING INTERNET ACCESS..."
                  : "CHECKING WI-FI SIGN-IN...", 300);
    return true;
}

bool psp_captive_portal_start(PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || frame == NULL || app->interactive == NULL
        || app->browser == NULL || app->browser->budget == NULL) return false;
    if (app->interactive->captive_portal != NULL) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN IS ALREADY OPEN", 180);
        return false;
    }
    if (browser_engine_navigation_pending(app->browser->engine)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WAIT FOR THE PAGE TO FINISH, THEN RETRY", 180);
        return false;
    }
    uint32_t active_download = 0;
    if (app->browser->media.ui.visible
        || psp_media_open_work_pending(&app->browser->media)
        || psp_update_session_active(&app->browser->update_session)
        || app->interactive->screenshot.writer.status == SCREENSHOT_PNG_PENDING
        || offline_download_manager_active(
               &app->browser->offline_store.download, &active_download)
        || psp_voice_component_session_active(
               app->browser->voice_component_session)
        || psp_glyph_component_session_active(
               app->browser->glyph_component_session)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "FINISH THE CURRENT OPERATION, THEN RETRY", 180);
        return false;
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (app->network_lifecycle == NULL
        || !psp_network_lifecycle_ready(app->network_lifecycle)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "CONNECT TO WI-FI FIRST", 180);
        return false;
    }
#endif
    PspCaptivePortal *portal = budget_calloc_category(
        app->browser->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*portal));
    if (portal == NULL) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "NOT ENOUGH MEMORY FOR WI-FI SIGN-IN", 240);
        return false;
    }
    const NavigationEntry *entry = app->views == NULL
        ? NULL : navigation_current(app->views->navigation);
    snprintf(portal->original_url, sizeof(portal->original_url), "%s",
             entry == NULL || entry->url[0] == '\0'
                 ? app->process->presentation.ui.url : entry->url);
    app->interactive->captive_portal = portal;
    portal->started_us = (uint64_t) sceKernelGetSystemTimeWide();
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (app->update_check_pending != NULL && *app->update_check_pending) {
        portal->resume_update_check = true;
        *app->update_check_pending = false;
    }
#endif
    if (!portal_probe_begin(app, portal, false)) {
        portal_release(app);
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN CHECK IS BUSY - RETRY", 180);
        return false;
    }
    frame->page_dirty = true;
    return true;
}

static bool portal_open(PspApp *app, PspAppFrameState *frame,
                        PspCaptivePortal *portal, const char *url)
{
    BrowserSession *session = browser_engine_session(app->browser->engine);
    BrowserTabs *tabs = app->browser->tabs;
    if (session == NULL || tabs == NULL
        || browser_tabs_count(tabs) >= BROWSER_TAB_LIMIT
        || !browser_tabs_capture_active(tabs, app->views->navigation)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            browser_tabs_count(tabs) >= BROWSER_TAB_LIMIT
                ? "CLOSE A TAB FOR WI-FI SIGN-IN"
                : "WI-FI SIGN-IN COULD NOT OPEN", 240);
        return false;
    }
    portal->previous_tab = browser_tabs_active_index(tabs);
    if (!browser_tabs_add(tabs, url, "Wi-Fi sign-in", &portal->portal_tab))
        return false;
    if (!browser_tabs_select(tabs, portal->portal_tab)) {
        (void) browser_tabs_remove(tabs, portal->portal_tab);
        (void) browser_tabs_select(tabs, portal->previous_tab);
        return false;
    }
    (void) browser_engine_cancel_network_work(
        app->browser->engine, "entering isolated captive portal");
    if (!browser_session_captive_portal_begin(session, url)) {
        (void) browser_tabs_remove(tabs, portal->portal_tab);
        (void) browser_tabs_select(tabs, portal->previous_tab);
        return false;
    }
    if (url != portal->portal_url)
        snprintf(portal->portal_url, sizeof(portal->portal_url), "%s", url);
    app->interactive->tab_transition = (PspTabTransition) {
        .pending = true,
        .added_target = true,
        .previous_index = portal->previous_tab,
        .target_index = portal->portal_tab,
        .close_after_success = (size_t) -1
    };
    app->process->presentation.ui.captive_portal_active = true;
    psp_tabs_sync_ui(
        &app->process->presentation.ui, tabs,
        &app->process->presentation.tab_view);
    if (!psp_begin_page_load(
            app->browser->engine, &app->process->presentation.ui,
            app->browser->profile, app->views->frame,
            &app->process->text_input, url, true,
            PSP_CAPTIVE_PORTAL_PAGE_LIMIT,
            PSP_CAPTIVE_PORTAL_PAGE_TIMEOUT_MS)) {
        browser_session_captive_portal_end(session);
        (void) browser_tabs_remove(tabs, portal->portal_tab);
        (void) browser_tabs_select(tabs, portal->previous_tab);
        memset(&app->interactive->tab_transition, 0,
               sizeof(app->interactive->tab_transition));
        app->process->presentation.ui.captive_portal_active = false;
        return false;
    }
    portal->phase = PSP_CAPTIVE_PORTAL_OPENING;
    app->interactive->navigation_job_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    frame->page_dirty = true;
    return true;
}

static bool portal_return(PspApp *app, PspAppFrameState *frame,
                          PspCaptivePortal *portal)
{
    BrowserTabs *tabs = app->browser->tabs;
    BrowserSession *session = browser_engine_session(app->browser->engine);
    if (tabs == NULL || session == NULL) return false;
    (void) browser_engine_cancel_network_work(
        app->browser->engine, "leaving captive portal");
    /* Destroy the portal's document/runtime before restoring the ordinary
       cookie and storage tables. RETURN_PENDING deliberately spans one frame;
       without this scrub the old portal script could observe the restored
       session before the replacement navigation starts. */
    static const char closed[] =
        "<!doctype html><title>Wi-Fi sign-in closed</title>";
    bool retired = browser_engine_commit_html(
        app->browser->engine, TILEFINCH_HOMEPAGE_URL,
        closed, sizeof(closed) - 1u, false);
    if (!retired) {
        BrowserOptionalMemoryReclaim reclaim = {0};
        if (browser_engine_reclaim_optional_memory(
                app->browser->engine, &reclaim)) {
            retired = browser_engine_commit_html(
                app->browser->engine, TILEFINCH_HOMEPAGE_URL,
                closed, sizeof(closed) - 1u, false);
        }
    }
    if (!retired) {
        portal->phase = PSP_CAPTIVE_PORTAL_ACTIVE;
        psp_ui_show_status(
            &app->process->presentation.ui,
            "NOT ENOUGH MEMORY TO CLOSE WI-FI SIGN-IN", 240);
        return false;
    }
    browser_session_captive_portal_end(session);
    app->process->presentation.ui.captive_portal_active = false;
    if (!browser_tabs_select(tabs, portal->previous_tab)) return false;
    app->interactive->tab_transition = (PspTabTransition) {
        .pending = true,
        .previous_index = portal->portal_tab,
        .target_index = portal->previous_tab,
        .close_after_success = portal->portal_tab
    };
    portal->phase = PSP_CAPTIVE_PORTAL_RETURN_PENDING;
    psp_tabs_sync_ui(
        &app->process->presentation.ui, tabs,
        &app->process->presentation.tab_view);
    if (portal->original_url[0] == '\0'
        || psp_ui_native_home_url(portal->original_url)) {
        (void) browser_tabs_remove(tabs, portal->portal_tab);
        memset(&app->interactive->tab_transition, 0,
               sizeof(app->interactive->tab_transition));
        psp_show_native_home(app);
        frame->page_dirty = true;
        portal_release(app);
        return true;
    }
    frame->page_dirty = true;
    return true;
}

static bool portal_begin_return_navigation(
    PspApp *app, PspAppFrameState *frame, PspCaptivePortal *portal)
{
    BrowserTabs *tabs = app->browser->tabs;
    if (tabs == NULL) return false;
    if (!psp_begin_page_load(
            app->browser->engine, &app->process->presentation.ui,
            app->browser->profile, app->views->frame,
            &app->process->text_input, portal->original_url, false,
            app->browser->engine == NULL ? 0u
                : browser_engine_config(app->browser->engine)
                    ->maximum_document_bytes,
            app->browser->engine == NULL ? 0L
                : browser_engine_config(app->browser->engine)
                    ->navigation_timeout_ms)) {
        (void) browser_tabs_remove(tabs, portal->portal_tab);
        (void) browser_tabs_select(tabs, portal->previous_tab);
        memset(&app->interactive->tab_transition, 0,
               sizeof(app->interactive->tab_transition));
        portal_release(app);
        return false;
    }
    portal->phase = PSP_CAPTIVE_PORTAL_RETURNING;
    app->interactive->navigation_job_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    frame->page_dirty = true;
    return true;
}

static void portal_probe_complete(
    PspApp *app, PspAppFrameState *frame, PspCaptivePortal *portal,
    FetchResult *result)
{
    char location[TILEFINCH_URL_SERIALIZED_LIMIT] = {0};
    (void) portal_header_value(
        result, "location", location, sizeof(location));
    TilefinchCaptiveProbeResponse response = {
        .status_code = result->status_code,
        .content_type = result->content_type,
        .location = location,
        .body = portal->body,
        .body_length = portal->body_length,
        .transport_succeeded = result->error[0] == '\0'
            && result->transport_code == 0,
        .timed_out = result->timed_out
    };
    TilefinchCaptiveProbeResult classified =
        tilefinch_captive_probe_classify(&response);
    bool verifying = portal->phase == PSP_CAPTIVE_PORTAL_VERIFYING;
    if (classified == TILEFINCH_CAPTIVE_PROBE_INTERNET) {
        if (verifying) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "WI-FI SIGN-IN COMPLETE", 180);
            (void) portal_return(app, frame, portal);
        } else {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "INTERNET ACCESS IS AVAILABLE", 180);
            portal_release(app);
        }
        return;
    }
    if (classified == TILEFINCH_CAPTIVE_PROBE_PORTAL && !verifying
        && tilefinch_captive_probe_portal_url(
            TILEFINCH_CAPTIVE_PORTAL_PROBE_URL, location,
            portal->portal_url)
        && portal_open(app, frame, portal, portal->portal_url)) return;
    if (verifying) {
        portal->phase = PSP_CAPTIVE_PORTAL_ACTIVE;
        psp_ui_show_status(
            &app->process->presentation.ui,
            classified == TILEFINCH_CAPTIVE_PROBE_PORTAL
                ? "WI-FI STILL REQUIRES SIGN-IN"
                : "COULD NOT VERIFY INTERNET - SIGN-IN STAYS OPEN", 240);
        return;
    }
    psp_ui_show_status(
        &app->process->presentation.ui,
        classified == TILEFINCH_CAPTIVE_PROBE_RETRYABLE
            ? "WI-FI CHECK TIMED OUT - RETRY"
            : "NO WI-FI SIGN-IN PAGE DETECTED", 240);
    portal_release(app);
}

void psp_captive_portal_pump(PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || frame == NULL || app->interactive == NULL) return;
    PspCaptivePortal *portal = app->interactive->captive_portal;
    if (portal == NULL) return;
    if (portal->phase == PSP_CAPTIVE_PORTAL_RETURN_PENDING) {
        (void) portal_begin_return_navigation(app, frame, portal);
        return;
    }
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (now_us - portal->started_us >= PSP_CAPTIVE_PORTAL_SESSION_LIMIT_US) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN TIME LIMIT REACHED", 240);
        (void) psp_captive_portal_cancel(app, frame);
        return;
    }
    if (portal->request_id == 0
        || (portal->phase != PSP_CAPTIVE_PORTAL_PROBING
            && portal->phase != PSP_CAPTIVE_PORTAL_VERIFYING)) return;
    FetchBackgroundProgress progress = {0};
    if (!fetch_background_transport_progress(portal->request_id, &progress))
        return;
    if (progress.available_body_bytes != 0) {
        size_t remaining = sizeof(portal->body) - portal->body_length;
        size_t length = 0;
        if (remaining == 0) {
            (void) fetch_background_transport_cancel(
                portal->request_id, "captive portal probe body exceeded limit");
            psp_ui_show_status(
                &app->process->presentation.ui,
                "WI-FI SIGN-IN RESPONSE WAS TOO LARGE", 240);
            portal_release(app);
            return;
        }
        if (!fetch_background_transport_take_chunk(
                portal->request_id, portal->body + portal->body_length,
                remaining, &length)) return;
        portal->body_length += length;
        return;
    }
    if (!progress.complete) return;
    FetchResult result = {0};
    uint64_t request_id = portal->request_id;
    portal->request_id = 0;
    if (!fetch_background_transport_take_fetch_result_consumed(
            request_id, app->browser->budget, &result,
            portal->body_length)) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN CHECK FAILED - RETRY", 180);
        portal_release(app);
        return;
    }
    portal_probe_complete(app, frame, portal, &result);
    fetch_result_destroy(&result);
}

/* Keep rare front-end services behind one resident-loop call boundary. */
bool psp_app_background_handle_frame(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent,
    uint64_t now_us)
{
    psp_captive_portal_pump(app, frame);
    return psp_voice_component_handle_frame(app, intent, now_us);
}

void psp_captive_portal_navigation_settled(
    PspApp *app, PspAppFrameState *frame, bool succeeded, bool cancelled)
{
    if (app == NULL || frame == NULL || app->interactive == NULL) return;
    PspCaptivePortal *portal = app->interactive->captive_portal;
    if (portal == NULL) return;
    if (portal->phase == PSP_CAPTIVE_PORTAL_OPENING) {
        if (!succeeded) {
            browser_session_captive_portal_end(app->browser->session);
            app->process->presentation.ui.captive_portal_active = false;
            psp_ui_show_status(
                &app->process->presentation.ui,
                cancelled ? "WI-FI SIGN-IN CANCELLED"
                          : "WI-FI SIGN-IN PAGE COULD NOT LOAD",
                240);
            portal_release(app);
            return;
        }
        const NavigationEntry *entry = navigation_current(
            app->views->navigation);
        if (entry == NULL || entry->url[0] == '\0'
            || (!browser_session_captive_portal_url_allowed(
                    app->browser->session, entry->url)
                && !browser_session_captive_portal_authorize_navigation(
                    app->browser->session, portal->portal_url,
                    entry->url))) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "WI-FI SIGN-IN REDIRECT LIMIT REACHED", 240);
            (void) portal_return(app, frame, portal);
            return;
        }
        snprintf(portal->portal_url, sizeof(portal->portal_url), "%s",
                 entry->url);
        portal->phase = PSP_CAPTIVE_PORTAL_ACTIVE;
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN  O CANCEL", 300);
        return;
    }
    if (portal->phase == PSP_CAPTIVE_PORTAL_RETURNING) {
        if (!succeeded) {
            if (portal->portal_tab < browser_tabs_count(app->browser->tabs))
                (void) browser_tabs_remove(
                    app->browser->tabs, portal->portal_tab);
            (void) browser_tabs_select(
                app->browser->tabs, portal->previous_tab);
        }
        portal_release(app);
        return;
    }
    if (portal->phase == PSP_CAPTIVE_PORTAL_ACTIVE && succeeded) {
        const NavigationEntry *entry = navigation_current(
            app->views->navigation);
        if (entry == NULL || entry->url[0] == '\0'
            || (!browser_session_captive_portal_url_allowed(
                    app->browser->session, entry->url)
                && !browser_session_captive_portal_authorize_navigation(
                    app->browser->session, portal->portal_url,
                    entry->url))) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "WI-FI SIGN-IN REDIRECT LIMIT REACHED", 240);
            (void) portal_return(app, frame, portal);
            return;
        }
        snprintf(portal->portal_url, sizeof(portal->portal_url), "%s",
                 entry->url);
        if (portal_probe_begin(app, portal, true)) return;
        psp_ui_show_status(
            &app->process->presentation.ui,
            "SIGN-IN SENT - CHECK AGAIN FROM HELP", 240);
    }
}

bool psp_captive_portal_cancel(PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || frame == NULL || app->interactive == NULL
        || app->interactive->captive_portal == NULL) return false;
    PspCaptivePortal *portal = app->interactive->captive_portal;
    if (portal->phase == PSP_CAPTIVE_PORTAL_PROBING) {
        psp_ui_show_status(
            &app->process->presentation.ui,
            "WI-FI SIGN-IN CHECK CANCELLED", 120);
        portal_release(app);
        return true;
    }
    if (portal->phase == PSP_CAPTIVE_PORTAL_ACTIVE
        || portal->phase == PSP_CAPTIVE_PORTAL_VERIFYING) {
        (void) portal_return(app, frame, portal);
        return true;
    }
    return false;
}

void psp_captive_portal_destroy(PspApp *app)
{
    if (app == NULL || app->interactive == NULL) return;
    PspCaptivePortal *portal = app->interactive->captive_portal;
    if (app->browser != NULL && app->browser->session != NULL)
        browser_session_captive_portal_end(app->browser->session);
    if (portal != NULL && app->browser != NULL
        && app->browser->tabs != NULL) {
        if (portal->portal_tab < browser_tabs_count(app->browser->tabs))
            (void) browser_tabs_remove(
                app->browser->tabs, portal->portal_tab);
        if (portal->previous_tab < browser_tabs_count(app->browser->tabs))
            (void) browser_tabs_select(
                app->browser->tabs, portal->previous_tab);
        memset(&app->interactive->tab_transition, 0,
               sizeof(app->interactive->tab_transition));
    }
    app->process->presentation.ui.captive_portal_active = false;
    portal_release(app);
}
