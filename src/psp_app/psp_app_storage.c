#include "psp_app_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tilefinch/update.h"
#include "tilefinch/url.h"

/*
 * Site storage on the PSP frontend. The core keeps every site in RAM and
 * raises one offer when a site outgrows it; this file shows that offer,
 * records the answer, applies the per-site and global settings, and fills
 * the site list of Settings > Device & storage > Site data & storage.
 */

static bool profile_has_room(const BrowserProfile *profile,
                             const char *origin)
{
    return browser_profile_site_storage_policy(profile, origin)
               != BROWSER_SITE_STORAGE_ASK
        || browser_profile_site_storage_count(profile)
               < BROWSER_PROFILE_STORAGE_SITE_LIMIT;
}

void psp_app_site_storage_boot(PspProcessResources *process,
                               PspBrowserResources *browser)
{
    BrowserSession *session = browser->session;
    const BrowserProfile *profile = browser->profile;
    /* Traced and replayed runs keep everything in RAM. */
    if (!process->persistent_site_data_available) return;
    const char *directory = process->storage.site_storage;
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) {
        printf("tilefinch-site-storage: directory unavailable errno=%d\n",
               errno);
        return;
    }
    browser_site_storage_remove_session_files(directory);
    if (!browser_session_site_storage_configure(session, directory)) {
        printf("tilefinch-site-storage: configure failed\n");
        return;
    }
    browser_session_site_storage_set_offers(
        session, browser_profile_site_storage_offers(profile));
    for (size_t i = 0; i < browser_profile_site_storage_count(profile);
         i++) {
        const char *origin = NULL;
        BrowserSiteStoragePolicy policy = BROWSER_SITE_STORAGE_ASK;
        if (browser_profile_site_storage_entry(profile, i, &origin, &policy)
            && !browser_session_site_storage_set_policy(
                   session, origin, policy))
            printf("tilefinch-site-storage: policy not applied\n");
    }
}

void psp_app_site_storage_sync_ui(PspUiState *ui,
                                  const BrowserSession *session,
                                  const BrowserProfile *profile,
                                  const char *url)
{
    unsigned state = browser_profile_site_storage_policy(profile, url);
    if (browser_session_site_storage_tier(session, url)
        == BROWSER_SITE_STORAGE_STICK_SESSION) state = 3u;
    ui->site_storage_state = state & 3u;
    ui->site_storage_offers = browser_profile_site_storage_offers(profile);
}

bool psp_app_site_storage_poll(PspApp *app)
{
    PspPresentationResources *presentation = &app->process->presentation;
    if (presentation->ui.screen != PSP_UI_SCREEN_PAGE) return false;
    BrowserSiteStorageRequest request;
    if (!browser_session_site_storage_take_request(
            app->browser->session, &request)) return false;
    snprintf(presentation->site_storage_offer_origin,
             sizeof(presentation->site_storage_offer_origin), "%s",
             request.origin);
    uint64_t free_bytes = 0;
    bool free_known = tilefinch_update_query_free_space(
        app->process->storage.site_storage, &free_bytes);
    psp_ui_show_storage_offer(
        &presentation->ui, request.origin, request.current_bytes,
        request.needed_bytes, request.stick_limit_bytes, free_known,
        free_bytes);
    return true;
}

/* https origins drop their scheme; anything else keeps it visible. */
static void site_storage_label(const char *origin, char *out,
                               size_t capacity)
{
    const char *shown = strncmp(origin, "https://", 8) == 0
        ? origin + 8 : origin;
    snprintf(out, capacity, "%s", shown);
}

static void site_storage_fill(PspApp *app)
{
    PspPresentationResources *presentation = &app->process->presentation;
    PspUiSiteStorageView *view = &presentation->site_storage_view;
    const BrowserSession *session = app->browser->session;
    memset(view, 0, sizeof(*view));
    size_t total = browser_session_site_storage_count(session);
    for (size_t i = 0;
         i < total && view->count < PSP_UI_SITE_STORAGE_ROW_LIMIT; i++) {
        BrowserSiteStorageInfo info;
        if (!browser_session_site_storage_info(session, i, &info)) break;
        PspUiSiteStorageRow *row = &view->rows[view->count];
        snprintf(presentation->site_storage_origins[view->count],
                 sizeof(presentation->site_storage_origins[0]), "%s",
                 info.origin);
        site_storage_label(info.origin, row->origin, sizeof(row->origin));
        char size[16];
        const char *where;
        if (info.tier == BROWSER_SITE_STORAGE_STICK_ALWAYS) {
            psp_ui_format_bytes(size, sizeof(size), info.file_bytes);
            where = "Always";
        } else if (info.tier == BROWSER_SITE_STORAGE_STICK_SESSION) {
            psp_ui_format_bytes(size, sizeof(size), info.file_bytes);
            where = "Session";
        } else {
            psp_ui_format_bytes(size, sizeof(size), info.bytes);
            where = info.policy == BROWSER_SITE_STORAGE_MEMORY_ONLY
                ? "RAM only" : "RAM";
        }
        snprintf(row->detail, sizeof(row->detail), "%s  %s", size, where);
        row->state = info.tier == BROWSER_SITE_STORAGE_STICK_SESSION
            ? 3u : (uint8_t) info.policy;
        view->count++;
    }
    uint64_t free_bytes = 0;
    if (app->process->persistent_site_data_available
        && tilefinch_update_query_free_space(
               app->process->storage.site_storage, &free_bytes)) {
        char size[16];
        psp_ui_format_bytes(size, sizeof(size), free_bytes);
        snprintf(view->stick_free, sizeof(view->stick_free),
                 "%s free on Memory Stick", size);
    }
    /* From Site information, start on this site's row. */
    char origin[BROWSER_ORIGIN_LIMIT];
    if (presentation->ui.site_storage_from_site_info
        && tilefinch_url_origin(presentation->ui.url, origin,
                                sizeof(origin))) {
        for (size_t row = 0; row < view->count; row++) {
            if (strcmp(presentation->site_storage_origins[row], origin)
                == 0) {
                presentation->ui.data_options_selection = (uint8_t) row;
                break;
            }
        }
    }
    psp_ui_set_site_storage(&presentation->ui, view);
}

static void site_storage_answer(PspApp *app, PspAppFrameState *frame,
                                PspUiAction action)
{
    PspPresentationResources *presentation = &app->process->presentation;
    BrowserSession *session = app->browser->session;
    BrowserProfile *profile = app->browser->profile;
    const char *origin = presentation->site_storage_offer_origin;
    if (action == PSP_UI_ACTION_STORAGE_OFFER_DECLINE) {
        browser_session_site_storage_decline(session, origin);
        psp_ui_show_status(&presentation->ui,
                           "SITE STORAGE STAYS IN MEMORY", 150);
        return;
    }
    bool always = action == PSP_UI_ACTION_STORAGE_OFFER_ALWAYS;
    /* An always-kept site the profile cannot remember would never load
       again; keep it for this session instead. */
    bool remembered = always && profile_has_room(profile, origin);
    bool granted = browser_session_site_storage_grant(
        session, origin, remembered);
    if (granted && remembered
        && browser_profile_set_site_storage_policy(
               profile, origin, BROWSER_SITE_STORAGE_STICK)) {
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
    }
    psp_ui_show_status(
        &presentation->ui,
        !granted ? "MEMORY STICK STORAGE FAILED"
            : remembered ? "SITE STORAGE ON MEMORY STICK"
            : always ? "SITE LIST FULL - THIS SESSION ONLY"
                     : "MEMORY STICK FOR THIS SESSION",
        240);
}

static void site_storage_delete(PspApp *app, PspAppFrameState *frame,
                                size_t row)
{
    PspPresentationResources *presentation = &app->process->presentation;
    BrowserProfile *profile = app->browser->profile;
    if (row >= presentation->site_storage_view.count) return;
    const char *origin = presentation->site_storage_origins[row];
    bool deleted = browser_session_site_storage_forget(
        app->browser->session, origin);
    if (browser_profile_site_storage_policy(profile, origin)
            != BROWSER_SITE_STORAGE_ASK
        && browser_profile_set_site_storage_policy(
               profile, origin, BROWSER_SITE_STORAGE_ASK)) {
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
    }
    site_storage_fill(app);
    psp_ui_show_status(&presentation->ui,
                       deleted ? "SITE STORAGE DELETED"
                               : "SITE STORAGE NOT DELETED",
                       180);
}

void psp_app_site_storage_action(PspApp *app, PspAppFrameState *frame,
                                 const PspUiIntent *intent)
{
    switch (intent->action) {
        case PSP_UI_ACTION_SHOW_SITE_STORAGE:
            site_storage_fill(app);
            break;
        case PSP_UI_ACTION_SITE_STORAGE_DELETE:
            site_storage_delete(app, frame, intent->list_index);
            break;
        case PSP_UI_ACTION_STORAGE_OFFER_SESSION:
        case PSP_UI_ACTION_STORAGE_OFFER_ALWAYS:
        case PSP_UI_ACTION_STORAGE_OFFER_DECLINE:
            site_storage_answer(app, frame, intent->action);
            break;
        default:
            break;
    }
}

bool psp_app_site_storage_setting(PspApp *app, PspAppFrameState *frame,
                                  const PspUiIntent *intent)
{
    PspUiState *ui = &app->process->presentation.ui;
    BrowserSession *session = app->browser->session;
    BrowserProfile *profile = app->browser->profile;
    if (intent->setting.id == PSP_UI_SETTING_SITE_STORAGE_OFFERS) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_site_storage_offers(profile, enabled);
        browser_session_site_storage_set_offers(session, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(ui, enabled ? "MEMORY STICK OFFERS ON"
                                       : "MEMORY STICK OFFERS OFF", 150);
        return true;
    }
    const char *url;
    if (intent->setting.id == PSP_UI_SETTING_SITE_STORAGE_SITE) {
        url = ui->url;
    } else if (intent->setting.id == PSP_UI_SETTING_SITE_STORAGE_LISTED) {
        if (intent->list_index
            >= app->process->presentation.site_storage_view.count)
            return true;
        url = app->process->presentation.site_storage_origins[
            intent->list_index];
    } else {
        return false;
    }
    BrowserSiteStoragePolicy policy =
        (BrowserSiteStoragePolicy) intent->setting.value.unsigned_value;
    BrowserSiteStorageTier previous_tier =
        browser_session_site_storage_tier(session, url);
    const char *status;
    if (policy != BROWSER_SITE_STORAGE_ASK
        && !profile_has_room(profile, url)) {
        status = "SITE LIST FULL";
    } else if (!browser_session_site_storage_set_policy(
                   session, url, policy)
               || !browser_profile_set_site_storage_policy(
                      profile, url, policy)) {
        status = policy == BROWSER_SITE_STORAGE_STICK
            ? "MEMORY STICK UNAVAILABLE" : "SITE STORAGE NOT CHANGED";
    } else {
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        status = policy == BROWSER_SITE_STORAGE_STICK
            ? "SITE STORAGE ON MEMORY STICK"
            : previous_tier == BROWSER_SITE_STORAGE_STICK_ALWAYS
                ? "STICK DATA KEPT UNTIL EXIT"
            : policy == BROWSER_SITE_STORAGE_MEMORY_ONLY
                ? "SITE STORAGE IN RAM ONLY"
                : "ASK BEFORE USING MEMORY STICK";
    }
    psp_app_site_storage_sync_ui(ui, session, profile, ui->url);
    if (intent->setting.id == PSP_UI_SETTING_SITE_STORAGE_LISTED)
        site_storage_fill(app);
    psp_ui_show_status(ui, status, 180);
    return true;
}
