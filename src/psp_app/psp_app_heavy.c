#include "psp_app_internal.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/content_blocker.h"
#include "tilefinch/platform.h"

/*
 * Heavy pages on the PSP frontend (docs/USER_GUIDE.md, "Heavy pages"). The
 * engine weighs each page's author script and classes it
 * (include/tilefinch/script_admission.h); this file turns the class into
 * what the user sees:
 *
 * - (b) an empty app shell whose big scripts would take 15 s or more: the
 *   engine holds them and the heavy-page offer asks first, with the size
 *   and the time it may take. X runs them for this session, Triangle
 *   always for this site (profile HS record), O leaves the page as it is.
 * - (c) scripts that cannot fit even in the best case: they are refused
 *   and the recovery sheet, titled "Too heavy for the PSP", offers Basic
 *   view (unless the Basic view fallback is Off), Reader and Return.
 * - (a) a heavy page that already shows content: its scripts run in the
 *   background; a status says so once, and X stops them. If the page then
 *   runs low on memory the status offers it once more.
 *
 * Settings > Browsing & input > Heavy pages chooses Ask (the above), Run
 * (never ask) or Basic view (refuse the big scripts of (b) and (c) pages).
 */

/* An (a) page whose reachable heap falls below this has the stop offered
   again. */
#define PSP_HEAVY_LOW_MEMORY_BYTES (2u * 1024u * 1024u)
#define PSP_HEAVY_STATUS_FRAMES 360u
#define PSP_HEAVY_SHELL_GRACE_US UINT64_C(5000000)
/* The poll runs every loop iteration for the page's whole life; weighing
   the page on one in this many is prompt enough for its notices. */
#define PSP_HEAVY_MEASURE_INTERVAL 8u

static ScriptHeavyPolicy heavy_policy_for_mode(BrowserHeavyPagesMode mode)
{
    return mode == BROWSER_HEAVY_PAGES_RUN ? SCRIPT_HEAVY_POLICY_RUN
        : mode == BROWSER_HEAVY_PAGES_BASIC ? SCRIPT_HEAVY_POLICY_REFUSE
        : SCRIPT_HEAVY_POLICY_ASK;
}

static bool heavy_site_key(const char *url, char site[CONTENT_BLOCKER_HOST_LIMIT])
{
    return url != NULL && content_blocker_site_from_url(url, site);
}

static bool heavy_session_allowed(const PspInteractiveState *interactive,
                                  const char *url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!heavy_site_key(url, site)) return false;
    for (size_t i = 0; i < interactive->heavy_session_site_count; i++) {
        if (strcmp(interactive->heavy_session_sites[i], site) == 0)
            return true;
    }
    return false;
}

static void heavy_session_allow(PspApp *app, const char *url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    PspInteractiveState *interactive = app->interactive;
    if (!heavy_site_key(url, site) || heavy_session_allowed(interactive, url))
        return;
    size_t at = interactive->heavy_session_site_count;
    if (at >= PSP_HEAVY_SESSION_SITE_LIMIT) {
        /* Oldest out. */
        memmove(interactive->heavy_session_sites[0],
                interactive->heavy_session_sites[1],
                (PSP_HEAVY_SESSION_SITE_LIMIT - 1u)
                    * sizeof(interactive->heavy_session_sites[0]));
        at = PSP_HEAVY_SESSION_SITE_LIMIT - 1u;
    } else {
        interactive->heavy_session_site_count++;
    }
    snprintf(interactive->heavy_session_sites[at],
             sizeof(interactive->heavy_session_sites[at]), "%s", site);
}

static ScriptHeavyPolicy heavy_policy_for_url(
    const PspInteractiveState *interactive, const char *url)
{
    const BrowserProfile *profile = interactive->heavy_profile;
    if (browser_profile_heavy_site_allowed(profile, url)
        || heavy_session_allowed(interactive, url))
        return SCRIPT_HEAVY_POLICY_RUN;
    return heavy_policy_for_mode(browser_profile_heavy_pages_mode(profile));
}

/* The engine asks as it creates each page realm, with the page's URL. */
static ScriptHeavyPolicy heavy_resolve(void *opaque, const char *url)
{
    return heavy_policy_for_url((const PspInteractiveState *) opaque, url);
}

void psp_app_heavy_boot(BrowserEngine *engine, const BrowserProfile *profile,
                        PspInteractiveState *interactive, PspUiState *ui)
{
    BrowserHeavyPagesMode mode = browser_profile_heavy_pages_mode(profile);
    interactive->heavy_profile = profile;
    browser_engine_set_heavy_page_policy(engine, heavy_policy_for_mode(mode));
    browser_engine_set_heavy_page_resolver(engine, heavy_resolve,
                                           interactive);
    if (ui != NULL) ui->heavy_pages_mode = (unsigned) mode;
}

static void heavy_format_status(char *out, size_t capacity,
                                const ScriptHeavyState *state,
                                const char *headline)
{
    char size[16];
    psp_ui_format_bytes(size, sizeof(size), state->script_bytes);
    unsigned seconds = (state->estimate_ms + 500u) / 1000u;
    snprintf(out, capacity, "%s: %s, ABOUT %u SECONDS", headline, size,
             seconds);
}

/* (c): scripts that cannot fit. On an empty shell the recovery sheet says
   why and offers Basic view and Reader; on a page that already shows its
   content a status says a large script was skipped, and reading goes on. */
static void heavy_show_over(PspApp *app, const ScriptHeavyState *state)
{
    PspUiState *ui = &app->process->presentation.ui;
    if (state->visible_text_bytes >= SCRIPT_HEAVY_SHELL_TEXT_BYTES) {
        psp_ui_show_status(ui, "A LARGE PAGE SCRIPT WAS SKIPPED: NOT ENOUGH "
                               "MEMORY",
                           PSP_HEAVY_STATUS_FRAMES);
        return;
    }
    char line[96];
    if (state->memory_needed != 0) {
        char needed[16], free_text[16];
        psp_ui_format_bytes(needed, sizeof(needed), state->memory_needed);
        psp_ui_format_bytes(free_text, sizeof(free_text),
                            state->memory_free);
        snprintf(line, sizeof(line),
                 "ITS SCRIPTS NEED ABOUT %s MORE MEMORY; %s IS FREE",
                 needed, free_text);
    } else {
        char size[16];
        psp_ui_format_bytes(size, sizeof(size), state->oversized_bytes);
        snprintf(line, sizeof(line),
                 "ITS APP SCRIPT (OVER %s) IS TOO LARGE FOR THE PSP", size);
    }
    /* The page's DOM is there, so Basic view can show it, unless the
       Basic view fallback setting is Off. */
    psp_present_heavy_recovery(
        app, ui->url, line,
        psp_basic_fallback_failure_actions(
            browser_profile_basic_fallback_mode(app->browser->profile), true,
            ui->basic_mode, false)
            | PSP_UI_FAILURE_READER);
}

bool psp_app_heavy_poll(PspApp *app)
{
    if (app == NULL || app->interactive == NULL || app->views == NULL
        || app->views->navigation == NULL) return false;
    PspInteractiveState *interactive = app->interactive;
    PspUiState *ui = &app->process->presentation.ui;
    BrowserEngine *engine = app->browser->engine;
    if (browser_engine_navigation_pending(engine)) return false;
    uint64_t generation = app->views->navigation->generation;
    if (interactive->heavy_generation != generation) {
        interactive->heavy_generation = generation;
        interactive->heavy_offer_shown = false;
        interactive->heavy_status_shown = false;
        interactive->heavy_low_memory_shown = false;
        interactive->heavy_rescue_shown = false;
        interactive->heavy_shell_since_us = 0;
        interactive->heavy_measure_countdown = 0;
    }
    /* The page's scripts ran out of memory and wiped it; the engine put the
       server's page back and stopped them. Its realm is gone, so this comes
       before the heavy state. */
    if (!interactive->heavy_rescue_shown
        && browser_engine_page_memory_rescued(engine)) {
        interactive->heavy_rescue_shown = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        printf("tilefinch-heavy: memory-rescue=1 screen=%d\n",
               (int) ui->screen);
#endif
        if (ui->screen == PSP_UI_SCREEN_PAGE) {
            psp_ui_show_status(ui, "PAGE SCRIPTS STOPPED: NOT ENOUGH MEMORY",
                               PSP_HEAVY_STATUS_FRAMES);
            return true;
        }
    }
    /* Only the offer's own screen and the page show heavy notices. Once
       the offer has been shown, and the status either was not or its
       low-memory follow-up was too, nothing is left to say for this page. */
    if (ui->screen != PSP_UI_SCREEN_PAGE
        && ui->screen != PSP_UI_SCREEN_HEAVY_OFFER) return false;
    if (ui->screen == PSP_UI_SCREEN_PAGE && interactive->heavy_offer_shown
        && (!interactive->heavy_status_shown
            || interactive->heavy_low_memory_shown)) return false;
    if (interactive->heavy_measure_countdown != 0) {
        interactive->heavy_measure_countdown--;
        return false;
    }
    interactive->heavy_measure_countdown = PSP_HEAVY_MEASURE_INTERVAL - 1u;
    ScriptHeavyState state;
    if (!browser_engine_heavy_page(engine, &state)) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if ((unsigned) state.page_class != interactive->heavy_logged_class
        || state.waiting_scripts != interactive->heavy_logged_waiting) {
        interactive->heavy_logged_class = (unsigned) state.page_class;
        interactive->heavy_logged_waiting = state.waiting_scripts;
        printf("tilefinch-heavy: class=%u script=%zu estimate-ms=%u "
               "waiting=%zu/%zu needed=%zu free=%zu text=%zu "
               "oversized=%zu answered=%d screen=%d\n",
               (unsigned) state.page_class, state.script_bytes,
               (unsigned) state.estimate_ms, state.waiting_scripts,
               state.waiting_bytes, state.memory_needed, state.memory_free,
               state.visible_text_bytes, state.oversized_scripts,
               state.answered ? (state.allowed ? 1 : 0) : -1,
               (int) ui->screen);
    }
#endif
    /* The offer is open and a script that arrived since proves the app
       cannot fit (m.vk.ru: the offer opens on its 1.1 MB vendor chunk,
       then the 3.0 MiB application chunk arrives): replace the offer with
       the explanation instead of offering to start what cannot run. */
    if (ui->screen == PSP_UI_SCREEN_HEAVY_OFFER && !state.answered
        && state.page_class == SCRIPT_HEAVY_CLASS_OVER) {
        (void) browser_engine_answer_heavy_page(engine, false);
        heavy_show_over(app, &state);
        return true;
    }
    if (ui->screen != PSP_UI_SCREEN_PAGE) return false;
    char line[96];
    if (state.waiting && !interactive->heavy_offer_shown) {
        interactive->heavy_offer_shown = true;
        if (heavy_policy_for_url(interactive, ui->url)
            == SCRIPT_HEAVY_POLICY_RUN) {
            (void) browser_engine_answer_heavy_page(engine, true);
            return false;
        }
        snprintf(interactive->heavy_offer_url,
                 sizeof(interactive->heavy_offer_url), "%s", ui->url);
        if (state.page_class == SCRIPT_HEAVY_CLASS_OVER) {
            (void) browser_engine_answer_heavy_page(engine, false);
            heavy_show_over(app, &state);
            return true;
        }
        char site[CONTENT_BLOCKER_HOST_LIMIT];
        psp_ui_show_heavy_offer(
            ui, heavy_site_key(ui->url, site) ? site : ui->url,
            state.script_bytes, state.estimate_ms);
        return true;
    }
    if (state.page_class == SCRIPT_HEAVY_CLASS_OVER && !state.waiting
        && state.oversized_scripts != 0 && !interactive->heavy_offer_shown) {
        interactive->heavy_offer_shown = true;
        heavy_show_over(app, &state);
        return true;
    }
    if (state.refused_scripts != 0 && !interactive->heavy_offer_shown) {
        /* Heavy pages: Basic view refused the big scripts by itself. */
        interactive->heavy_offer_shown = true;
        psp_ui_show_status(ui, "LARGE APP NOT STARTED (HEAVY PAGES SETTING)",
                           PSP_HEAVY_STATUS_FRAMES);
        return true;
    }
    /* (a), or an app shell whose scripts were already running when it
       became heavy (they came with the page, not after it, so there was
       nothing to hold): say what it costs once, and offer the stop. */
    /* A shell's big scripts usually arrive after its first ones (the
       offer then asks first); give them PSP_HEAVY_SHELL_GRACE_US before
       saying the app is starting, so the status does not precede an
       offer to start it. */
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    if (state.page_class == SCRIPT_HEAVY_CLASS_SHELL
        && interactive->heavy_shell_since_us == 0)
        interactive->heavy_shell_since_us = now_us;
    bool shell_settled = state.page_class == SCRIPT_HEAVY_CLASS_SHELL
        && now_us - interactive->heavy_shell_since_us
               >= PSP_HEAVY_SHELL_GRACE_US;
    if ((state.page_class == SCRIPT_HEAVY_CLASS_CONTENT || shell_settled)
        && !interactive->heavy_status_shown && !state.answered
        && !interactive->heavy_offer_shown) {
        interactive->heavy_status_shown = true;
        heavy_format_status(line, sizeof(line), &state,
                            state.page_class == SCRIPT_HEAVY_CLASS_SHELL
                                ? "LARGE APP STARTING" : "PAGE SCRIPTS");
        psp_ui_show_heavy_scripts_status(ui, line, PSP_HEAVY_STATUS_FRAMES);
        return true;
    }
    if (state.page_class == SCRIPT_HEAVY_CLASS_CONTENT
        && interactive->heavy_status_shown
        && !interactive->heavy_low_memory_shown
        && state.memory_free < PSP_HEAVY_LOW_MEMORY_BYTES) {
        interactive->heavy_low_memory_shown = true;
        psp_ui_show_heavy_scripts_status(
            ui, "PAGE SCRIPTS ARE RUNNING LOW ON MEMORY",
            PSP_HEAVY_STATUS_FRAMES);
        return true;
    }
    return false;
}

void psp_app_heavy_action(PspApp *app, PspAppFrameState *frame,
                          const PspUiIntent *intent)
{
    PspUiState *ui = &app->process->presentation.ui;
    BrowserEngine *engine = app->browser->engine;
    const char *url = app->interactive->heavy_offer_url;
    switch (intent->action) {
        case PSP_UI_ACTION_HEAVY_RUN_SESSION:
            heavy_session_allow(app, url);
            (void) browser_engine_answer_heavy_page(engine, true);
            psp_ui_show_status(ui, "STARTING THE SITE'S APP", 180);
            break;
        case PSP_UI_ACTION_HEAVY_RUN_ALWAYS: {
            bool remembered = browser_profile_set_heavy_site_allowed(
                app->browser->profile, url, true);
            if (remembered) {
                psp_profile_store_mark_dirty(
                    &app->browser->profile_store, frame->ui_sample_us);
            } else {
                heavy_session_allow(app, url);
            }
            (void) browser_engine_answer_heavy_page(engine, true);
            psp_ui_show_status(ui,
                               remembered ? "THIS SITE'S APP ALWAYS STARTS"
                                          : "SITE LIST FULL - THIS SESSION",
                               180);
            break;
        }
        case PSP_UI_ACTION_HEAVY_CANCEL:
            (void) browser_engine_answer_heavy_page(engine, false);
            psp_ui_show_status(ui, "APP NOT STARTED. PAGE KEPT AS IS", 180);
            break;
        case PSP_UI_ACTION_HEAVY_STOP_SCRIPTS:
            psp_ui_show_status(ui,
                               browser_engine_stop_page_scripts(engine)
                                   ? "PAGE SCRIPTS STOPPED"
                                   : "PAGE SCRIPTS ALREADY STOPPED",
                               180);
            break;
        default:
            break;
    }
}

bool psp_app_heavy_setting(PspApp *app, PspAppFrameState *frame,
                           const PspUiIntent *intent)
{
    if (intent->setting.id != PSP_UI_SETTING_HEAVY_PAGES) return false;
    BrowserProfile *profile = app->browser->profile;
    BrowserHeavyPagesMode mode = intent->setting.value.heavy_pages_mode;
    if (browser_profile_set_heavy_pages_mode(profile, mode))
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
    PspUiState *ui = &app->process->presentation.ui;
    ui->heavy_pages_mode = (unsigned) browser_profile_heavy_pages_mode(profile);
    browser_engine_set_heavy_page_policy(
        app->browser->engine,
        heavy_policy_for_url(app->interactive, ui->url));
    psp_ui_show_status(
        ui,
        mode == BROWSER_HEAVY_PAGES_RUN ? "HEAVY PAGES: RUN"
            : mode == BROWSER_HEAVY_PAGES_BASIC ? "HEAVY PAGES: BASIC VIEW"
            : "HEAVY PAGES: ASK",
        180);
    return true;
}
