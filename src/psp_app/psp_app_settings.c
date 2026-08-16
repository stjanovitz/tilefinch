/* Executes Options intents, including transactional boot overrides and the
 * content-blocker allowlist import.
 */
#include "psp_app_internal.h"

static bool psp_app_update_trust_configured(
    const BrowserProfile *profile, const PspBootConfig *config)
{
    return tilefinch_update_root_is_configured()
        || (browser_profile_update_channel(profile)
                == BROWSER_UPDATE_CHANNEL_DEVELOPER
            && config != NULL && config->developer_update_url[0] != '\0');
}

static bool psp_app_boot_override_path(
    const TilefinchInstallPaths *paths, char *output, size_t capacity)
{
    if (paths == NULL) return false;
    return tilefinch_install_data_path(
        paths, paths->slotted ? "boot-overrides.cfg" : "boot.cfg",
        output, capacity);
}

/*
 * The Developer endpoint is deliberately local configuration, not profile
 * data: a web page cannot write it, it survives A/B slot changes, and an
 * older slot already knows how to read the same boot override. Keep the
 * editor in this settings receiver so the validation, transactional write,
 * update-client reset, and UI availability bit change as one operation.
 */
__attribute__((noinline))
bool psp_app_edit_developer_update_url(
    PspApp *app, PspAppFrameState *frame)
{
    if (app == NULL || app->process == NULL || frame == NULL
        || app->browser->profile == NULL) return false;

    if (strlen(app->process->config.developer_update_url)
        > PSP_TEXT_INPUT_CAPACITY) {
        psp_ui_show_status(
            &app->process->presentation.ui, "URL TOO LONG FOR IN-APP EDITOR", 240);
        return false;
    }

    char edited[PSP_TEXT_INPUT_CAPACITY + 1u] = {0};
    PspTextInputRequest request = {
        .description = "DEVELOPER UPDATE URL",
        .initial = app->process->config.developer_update_url,
        .keyboard_url_mode = true,
        .suggest_navigation = false
    };
    bool accepted = psp_text_input_request(
        &app->process->text_input, app->views->frame, &app->process->presentation.ui, &request,
        edited, sizeof(edited));
    if (!accepted) return false;
    if (edited[0] != '\0'
        && !tilefinch_update_url_is_valid(edited, sizeof(edited))) {
        psp_ui_show_status(
            &app->process->presentation.ui, "ENTER A PUBLIC HTTPS URL", 240);
        return false;
    }
    if (strcmp(edited, app->process->config.developer_update_url) == 0) {
        psp_ui_show_status(&app->process->presentation.ui, "DEVELOPER URL UNCHANGED", 120);
        return false;
    }

    char override_path[TILEFINCH_INSTALL_PATH_LIMIT];
    char previous[sizeof(app->process->config.developer_update_url)];
    snprintf(previous, sizeof(previous), "%s",
             app->process->config.developer_update_url);
    if (!psp_app_boot_override_path(
            &app->process->install_paths, override_path, sizeof(override_path))) {
        psp_ui_show_status(&app->process->presentation.ui, "UPDATE SETTINGS PATH FAILED", 240);
        return false;
    }
    snprintf(app->process->config.developer_update_url,
             sizeof(app->process->config.developer_update_url), "%s", edited);
    if (!psp_boot_config_write_overrides(&app->process->config, override_path)) {
        snprintf(app->process->config.developer_update_url,
                 sizeof(app->process->config.developer_update_url), "%s", previous);
        psp_ui_show_status(&app->process->presentation.ui, "DEVELOPER URL NOT SAVED", 240);
        return false;
    }

    bool configured = edited[0] != '\0';
    bool endpoint_was_active =
        browser_profile_update_channel(app->browser->profile)
            == BROWSER_UPDATE_CHANNEL_DEVELOPER;
    app->process->presentation.ui.developer_update_available = configured;
    if (!configured && endpoint_was_active) {
        browser_profile_set_update_channel(
            app->browser->profile, BROWSER_UPDATE_CHANNEL_STABLE);
        app->process->presentation.ui.update_channel = BROWSER_UPDATE_CHANNEL_STABLE;
    }
    /* Only an active Developer endpoint owns the current offer/session.
       Editing an unused URL must not discard a Stable or Beta offer. */
    if (endpoint_was_active) {
        browser_profile_set_update_check_available_sequence(app->browser->profile, 0);
        browser_profile_set_update_check_last_unix(app->browser->profile, 0);
        app->process->presentation.ui.update_release_available = false;
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_update_session_destroy(&app->browser->update_session);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        if (app->update_check_running != NULL)
            *app->update_check_running = false;
        if (app->update_check_pending != NULL)
            *app->update_check_pending =
                browser_profile_update_check_enabled(app->browser->profile)
                && strcmp(app->process->config.trace, "none") == 0
                && psp_app_update_trust_configured(
                       app->browser->profile, &app->process->config);
#endif
    }
    printf("tilefinch-update-config: source=in-app configured=%d\n",
           configured ? 1 : 0);
    psp_ui_show_status(
        &app->process->presentation.ui,
        configured ? "DEVELOPER URL SAVED"
                   : endpoint_was_active
                         ? "DEVELOPER URL CLEARED - STABLE SELECTED"
                         : "DEVELOPER URL CLEARED",
        240);
    return true;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Open the picker on the spelling this boot really used. Keep the table walk
   out of line and beside the picker itself. */
__attribute__((noinline))
void psp_app_seed_video_decoder_choice(
    PspUiState *ui, const PspBootConfig *config)
{
    if (ui == NULL || config == NULL) return;
    ui->experimental_decoder_choice =
        psp_media_wide_program_choice_index(config->experimental_wide_video);
}

/*
 * The Experimental screen's decoder-program picker.
 *
 * The knob it writes -- boot.cfg `experimental_wide_video` -- is read once at
 * startup, before the Media Engine pool is reserved and long before a video
 * can open, so it cannot be applied to a running process: this saves it and
 * offers a restart. That is the whole point of the picker. Every device A/B
 * so far has cost a shutdown, a USB cable, and a hand edit per attempt.
 *
 * Two phases through one action, because the receiver owns the bit that tells
 * them apart: the first press saves and raises the prompt, and a press while
 * the prompt is up means "restart now". Declining leaves the value saved.
 *
 * The picker can only offer spellings psp_media_wide_program_choice lists,
 * every one of which psp_media_wide_program_name_valid accepts, so no
 * selection here can reach the boot-time config gate's halt. A hand-edited
 * typo in boot.cfg still halts exactly as it does today.
 */
__attribute__((noinline))
bool psp_app_set_video_decoder(PspApp *app)
{
    if (app == NULL || app->process == NULL
        || app->interactive == NULL) return false;

    if (app->process->presentation.ui.experimental_decoder_restart_prompt) {
        app->process->presentation.ui.experimental_decoder_restart_prompt = 0;
        psp_exit_plan_request(
            &app->interactive->exit, PSP_EXIT_CONFIG_RESTART);
        /* The relaunch itself is the updater's: the clean-exit tail execs the
           installed launcher, which only exists for a slotted install. Say so
           rather than promising a restart that will land on the XMB. */
        psp_ui_show_status(
            &app->process->presentation.ui,
            app->process->install_paths.slotted
                ? "RESTARTING..." : "EXITING - RELAUNCH TO APPLY",
            240);
        return true;
    }

    const char *selected = psp_media_wide_program_choice(
        app->process->presentation.ui.experimental_decoder_choice
        % PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT);
    if (selected == NULL) return false;
    if (psp_media_wide_program_from_name(selected)
            == psp_media_wide_program_configured(
                   app->process->config.experimental_wide_video)) {
        psp_ui_show_status(&app->process->presentation.ui, "VIDEO DECODER UNCHANGED", 120);
        return false;
    }

    char override_path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!psp_app_boot_override_path(
            &app->process->install_paths, override_path, sizeof(override_path))) {
        psp_ui_show_status(&app->process->presentation.ui, "DECODER SETTINGS PATH FAILED", 240);
        return false;
    }
    char previous[sizeof(app->process->config.experimental_wide_video)];
    snprintf(previous, sizeof(previous), "%s",
             app->process->config.experimental_wide_video);
    snprintf(app->process->config.experimental_wide_video,
             sizeof(app->process->config.experimental_wide_video), "%s", selected);
    if (!psp_boot_config_write_overrides(&app->process->config, override_path)) {
        snprintf(app->process->config.experimental_wide_video,
                 sizeof(app->process->config.experimental_wide_video), "%s",
                 previous);
        psp_ui_show_status(&app->process->presentation.ui, "VIDEO DECODER NOT SAVED", 240);
        return false;
    }
    printf("tilefinch-media-decoder: event=knob-saved source=in-app "
           "value=%s previous=%s\n",
           selected, previous[0] == '\0' ? "wide-default" : previous);
    app->process->presentation.ui.experimental_decoder_restart_prompt = 1;
    psp_ui_show_status(&app->process->presentation.ui, "SAVED - RESTART TO APPLY?", 300);
    return true;
}
#endif

static void psp_app_reload_for_site_security_setting(
    PspApp *app, PspAppFrameState *frame,
    const char *reloading_status, const char *saved_status)
{
    char reload_url[NAVIGATION_URL_LIMIT];
    snprintf(reload_url, sizeof(reload_url), "%s", app->process->presentation.ui.url);
    bool local_surface = app->process->presentation.ui.base_screen == PSP_UI_SCREEN_HOME
        || psp_profile_page_kind(reload_url) != PSP_PROFILE_PAGE_NONE
        || psp_offline_url(reload_url);
    if (!local_surface && browser_engine_navigation_pending(app->browser->engine)) {
        browser_engine_cancel_navigation(
            app->browser->engine, "site security exception changed");
    }
    bool started = local_surface || psp_begin_page_load(
        app->browser->engine, &app->process->presentation.ui, app->browser->profile, app->views->frame,
        &app->process->text_input, reload_url, false, 4 * MIB, 30000);
    if (started && !local_surface) {
        app->interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    }
    psp_ui_show_status(
        &app->process->presentation.ui,
        started && !local_surface ? reloading_status
            : (started ? saved_status : "SETTING SAVED - RELOAD FAILED"),
        240);
    (void) frame;
}

/*
 * Options screen commands. Every branch tests intent->setting.id, so
 * this is one command with a payload rather than forty; the chain is
 * kept in source order because the order in which two settings are
 * applied in one frame is observable.
 */
void psp_app_apply_setting(
    PspApp *app, PspAppFrameState *frame, const PspUiIntent *intent)
{
    if (intent == NULL || intent->setting.id == PSP_UI_SETTING_NONE) return;
    BrowserEngine *engine = app->browser->engine;
    BrowserSession *session = app->browser->session;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    const uint16_t *engine_frame = app->views->frame;
    const char *recovery_path = app->process->storage.recovery;
    const char *tab_hibernation_path = app->process->storage.tab_hibernation;
    const char *tab_session_path = app->process->storage.tab_session;
    const char *content_blocker_path = app->process->storage.content_blocker;
    const char *local_storage_path = app->process->storage.local_storage;
    const char *persistent_cache_path = app->process->storage.persistent_cache;
    if (intent->setting.id == PSP_UI_SETTING_PAGE_FONT_PERCENT) {
        unsigned page_font_percent =
            intent->setting.value.unsigned_value;
        const NavigationEntry *entry =
            navigation_current(app->views->navigation);
        if (!psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, app->process->presentation.ui.reader_mode,
                entry == NULL ? NULL : entry->url,
                page_font_percent, true)) {
            unsigned fallback =
                browser_profile_page_font_percent(profile);
            app->process->presentation.ui.page_font_percent = fallback;
            (void) psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, app->process->presentation.ui.reader_mode,
                entry == NULL ? NULL : entry->url,
                fallback, true);
            psp_ui_show_status(
                &app->process->presentation.ui, "WEB FONT SIZE UNAVAILABLE", 240);
        } else {
            (void) psp_engine_views_refresh(app->views, engine);
            frame->page_dirty = true;
            char status[48];
            snprintf(status, sizeof(status), "WEB PAGE TEXT %u%%",
                     page_font_percent);
            psp_ui_show_status(&app->process->presentation.ui, status, 180);
            if (app->process->presentation.ui.reader_mode) {
                if (app->process->presentation.ui.remember_reader_site_scale
                    && entry != NULL
                    && browser_profile_record_reader_site_font_percent(
                           profile, entry->url,
                           page_font_percent)) {
                    psp_profile_store_mark_dirty(
                        &app->browser->profile_store, frame->ui_sample_us);
                }
            } else {
                browser_profile_set_page_font_percent(
                    profile, page_font_percent);
                psp_profile_store_mark_dirty(
                    &app->browser->profile_store, frame->ui_sample_us);
            }
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_READER_FONT) {
        BrowserReaderFont requested =
            intent->setting.value.reader_font;
        BrowserReaderFont previous =
            browser_profile_reader_font(profile);
        browser_profile_set_reader_font(profile, requested);
        bool applied = true;
        if (app->process->presentation.ui.reader_mode) {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            applied = psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, true,
                entry == NULL ? NULL : entry->url,
                app->process->presentation.ui.page_font_percent, true);
        }
        if (!applied) {
            browser_profile_set_reader_font(profile, previous);
            app->process->presentation.ui.reader_font_serif =
                previous == BROWSER_READER_FONT_SERIF;
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            (void) psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, true,
                entry == NULL ? NULL : entry->url,
                app->process->presentation.ui.page_font_percent, true);
            psp_ui_show_status(
                &app->process->presentation.ui, "READER FONT UNAVAILABLE", 240);
        } else {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            frame->page_dirty = app->process->presentation.ui.reader_mode || frame->page_dirty;
            psp_ui_show_status(
                &app->process->presentation.ui,
                requested == BROWSER_READER_FONT_SERIF
                    ? "READER FONT SERIF"
                    : "READER FONT SANS",
                180);
        }
    }
    if (intent->setting.id
        == PSP_UI_SETTING_REMEMBER_READER_SITE_SCALE) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_remember_reader_site_scale(
            profile, enabled);
        if (enabled && app->process->presentation.ui.reader_mode) {
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            if (entry != NULL) {
                (void) browser_profile_record_reader_site_font_percent(
                    profile, entry->url, app->process->presentation.ui.page_font_percent);
            }
        }
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            enabled ? "READER SIZE REMEMBERED PER SITE"
                    : "READER SIZE MEMORY OFF",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_BROWSER_UI_SCALE) {
        browser_profile_set_ui_scale(
            profile, intent->setting.value.unsigned_value);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
    }
    if (intent->setting.id == PSP_UI_SETTING_CUSTOM_HOMEPAGE) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_custom_homepage_enabled(
            profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui, enabled
                ? "HOME OPENS MY LINKS"
                : "HOME OPENS BUILT-IN PAGE",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_HISTORY) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_history_enabled(
            profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui, enabled
                ? "URL HISTORY ON" : "URL HISTORY OFF",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_WAVE_BACKGROUND) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_wave_background(profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui, enabled ? "WAVE BACKGROUND ON"
                         : "WAVE BACKGROUND OFF",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_UPDATE_CHECK) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_update_check_enabled(
            profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /* Re-arm or disarm this boot's deferred check; a check
           already in flight simply completes. */
        (*app->update_check_pending) = enabled
            && !(*app->update_check_running)
            && strcmp(app->process->config.trace, "none") == 0
            && psp_app_update_trust_configured(profile, &app->process->config);
#endif
        psp_ui_show_status(
            &app->process->presentation.ui, enabled
                ? "UPDATE CHECK ON" : "UPDATE CHECK OFF",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_UPDATE_CHANNEL) {
        BrowserUpdateChannel channel = intent->setting.value.update_channel;
        browser_profile_set_update_channel(profile, channel);
        /* An offer belongs to exactly one endpoint. A channel change clears
           it rather than letting stable/beta/developer state bleed across. */
        browser_profile_set_update_check_available_sequence(profile, 0);
        browser_profile_set_update_check_last_unix(profile, 0);
        app->process->presentation.ui.update_release_available = false;
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        (*app->update_check_pending) =
            !(*app->update_check_running)
            && browser_profile_update_check_enabled(profile)
            && strcmp(app->process->config.trace, "none") == 0
            && psp_app_update_trust_configured(profile, &app->process->config);
#endif
        psp_ui_show_status(
            &app->process->presentation.ui,
            channel == BROWSER_UPDATE_CHANNEL_BETA
                ? "BETA UPDATES SELECTED"
                : channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
                      ? "DEVELOPER URL - UNSIGNED CODE"
                      : "STABLE UPDATES SELECTED",
            240);
    }
    if (intent->setting.id == PSP_UI_SETTING_JAVASCRIPT) {
        bool enabled = intent->setting.value.boolean;
        if (browser_engine_navigation_pending(engine))
            browser_engine_cancel_navigation(
                engine, "global JavaScript policy changed");
        bool effective = enabled
            && browser_profile_site_javascript_enabled(
                   profile, app->process->presentation.ui.url);
        if (!browser_engine_set_javascript_enabled(engine, effective)) {
            app->process->presentation.ui.javascript_enabled =
                browser_profile_javascript_enabled(profile);
            psp_ui_show_status(
                &app->process->presentation.ui, "JAVASCRIPT SETTING UNAVAILABLE", 240);
        } else {
            browser_profile_set_javascript_enabled(profile, enabled);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            char reload_url[NAVIGATION_URL_LIMIT];
            snprintf(reload_url, sizeof(reload_url), "%s", app->process->presentation.ui.url);
            bool local_surface =
                app->process->presentation.ui.base_screen == PSP_UI_SCREEN_HOME
                || psp_profile_page_kind(reload_url)
                       != PSP_PROFILE_PAGE_NONE
                || psp_offline_url(reload_url);
            bool started = local_surface
                || psp_begin_page_load(
                    engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                    reload_url, false, 4 * MIB, 30000);
            if (started && !local_surface)
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            psp_ui_show_status(
                &app->process->presentation.ui,
                started && !local_surface
                    ? (enabled
                           ? "JAVASCRIPT ON - RELOADING"
                           : "JAVASCRIPT OFF - RELOADING")
                    : (started
                           ? (enabled ? "JAVASCRIPT ON"
                                      : "JAVASCRIPT OFF")
                           : "JAVASCRIPT SAVED - RELOAD FAILED"),
                240);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_SITE_JAVASCRIPT) {
        bool enabled = intent->setting.value.boolean;
        bool previous = browser_profile_site_javascript_enabled(
            profile, app->process->presentation.ui.url);
        if (browser_engine_navigation_pending(engine))
            browser_engine_cancel_navigation(
                engine, "site JavaScript policy changed");
        bool changed = previous != enabled
            && browser_profile_set_site_javascript_enabled(
                   profile, app->process->presentation.ui.url, enabled);
        bool effective = browser_profile_javascript_enabled(profile)
            && enabled;
        if (!changed
            || !browser_engine_set_javascript_enabled(engine, effective)) {
            if (changed) {
                (void) browser_profile_set_site_javascript_enabled(
                    profile, app->process->presentation.ui.url, previous);
            }
            app->process->presentation.ui.site_javascript_enabled = previous;
            psp_ui_show_status(
                &app->process->presentation.ui, "SITE JAVASCRIPT SETTING UNAVAILABLE", 240);
        } else {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            char reload_url[NAVIGATION_URL_LIMIT];
            snprintf(reload_url, sizeof(reload_url), "%s", app->process->presentation.ui.url);
            bool local_surface =
                app->process->presentation.ui.base_screen == PSP_UI_SCREEN_HOME
                || psp_profile_page_kind(reload_url)
                       != PSP_PROFILE_PAGE_NONE
                || psp_offline_url(reload_url);
            bool started = local_surface
                || psp_begin_page_load(
                    engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                    reload_url, false, 4 * MIB, 30000);
            if (started && !local_surface)
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            psp_ui_show_status(
                &app->process->presentation.ui,
                started && !local_surface
                    ? (enabled
                           ? "SITE JAVASCRIPT ON - RELOADING"
                           : "SITE JAVASCRIPT OFF - RELOADING")
                    : (started
                           ? (enabled ? "SITE JAVASCRIPT ON"
                                      : "SITE JAVASCRIPT OFF")
                           : "SITE JAVASCRIPT SAVED - RELOAD FAILED"),
                240);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_SITE_DATA_ALLOWED) {
        bool allowed = intent->setting.value.boolean;
        browser_session_set_site_data_allowed(session, allowed);
        browser_profile_set_site_data_allowed(profile, allowed);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        if (browser_engine_navigation_pending(engine))
            browser_engine_cancel_navigation(
                engine, "global site-data policy changed");
        char reload_url[NAVIGATION_URL_LIMIT];
        snprintf(reload_url, sizeof(reload_url), "%s", app->process->presentation.ui.url);
        bool local_surface =
            app->process->presentation.ui.base_screen == PSP_UI_SCREEN_HOME
            || psp_profile_page_kind(reload_url)
                   != PSP_PROFILE_PAGE_NONE
            || psp_offline_url(reload_url);
        bool started = local_surface
            || psp_begin_page_load(
                engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                reload_url, false, 4 * MIB, 30000);
        if (started && !local_surface)
            app->interactive->navigation_job_started_us =
                (uint64_t) sceKernelGetSystemTimeWide();
        psp_ui_show_status(
            &app->process->presentation.ui,
            started && !local_surface
                ? (allowed
                       ? "SITE DATA ALLOWED - RELOADING"
                       : "SITE DATA BLOCKED - RELOADING")
                : (started
                       ? (allowed ? "SITE DATA ALLOWED"
                                  : "SITE DATA BLOCKED")
                       : "SITE DATA SAVED - RELOAD FAILED"),
            240);
    }
    if (intent->setting.id == PSP_UI_SETTING_MIXED_CONTENT_SITE) {
        bool allowed = intent->setting.value.boolean;
        bool previous = browser_session_mixed_content_site_allowed(
            session, app->process->presentation.ui.url);
        if (browser_session_set_mixed_content_site_allowed(
                session, app->process->presentation.ui.url, allowed)) {
            psp_app_reload_for_site_security_setting(
                app, frame,
                allowed ? "HTTP ALLOWED THIS SESSION - RELOADING"
                        : "MIXED CONTENT BLOCKED - RELOADING",
                allowed ? "HTTP ALLOWED THIS SESSION"
                        : "MIXED CONTENT BLOCKED");
        } else {
            app->process->presentation.ui.mixed_content_site_allowed = previous;
            psp_ui_show_status(&app->process->presentation.ui, "SITE EXCEPTION LIMIT REACHED", 240);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE) {
        bool allowed = intent->setting.value.boolean;
        bool previous = browser_profile_third_party_cookie_site_allowed(
            profile, app->process->presentation.ui.url);
        bool session_changed =
            browser_session_set_third_party_cookie_site_allowed(
                session, app->process->presentation.ui.url, allowed);
        bool changed = session_changed
            && browser_profile_set_third_party_cookie_site_allowed(
                profile, app->process->presentation.ui.url, allowed);
        if (changed) {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_app_reload_for_site_security_setting(
                app, frame,
                allowed ? "3RD-PARTY COOKIES ON - RELOADING"
                        : "3RD-PARTY COOKIES BLOCKED - RELOADING",
                allowed ? "3RD-PARTY COOKIES ALLOWED"
                        : "3RD-PARTY COOKIES BLOCKED");
        } else {
            if (session_changed) {
                (void) browser_session_set_third_party_cookie_site_allowed(
                    session, app->process->presentation.ui.url, previous);
            }
            app->process->presentation.ui.third_party_cookie_site_allowed = previous;
            psp_ui_show_status(&app->process->presentation.ui, "SITE EXCEPTION LIMIT REACHED", 240);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_SEARCH_ENGINE) {
        BrowserSearchEngine search_engine =
            intent->setting.value.search_engine;
        browser_profile_set_search_engine(
            profile, search_engine);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        char status[48];
        snprintf(
            status, sizeof(status), "SEARCH ENGINE %s",
            browser_search_engine_name(search_engine));
        psp_ui_show_status(&app->process->presentation.ui, status, 180);
    }
    if (intent->setting.id == PSP_UI_SETTING_COLOR_MODE) {
        browser_profile_set_color_mode(
            profile, intent->setting.value.color_mode);
        psp_refresh_page_color_mode(&app->process->presentation.ui);
        (void) browser_engine_set_forced_dark(
            engine, app->process->presentation.ui.page_dark);
        frame->page_dirty = true;
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            app->process->presentation.ui.color_mode == BROWSER_COLOR_MODE_AUTO
                ? (app->process->presentation.ui.page_dark
                       ? "NIGHT MODE AUTO: DARK"
                       : "NIGHT MODE AUTO: LIGHT")
                : (app->process->presentation.ui.page_dark
                       ? "NIGHT MODE DARK"
                       : "NIGHT MODE LIGHT"),
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_CHROME_THEME) {
        BrowserChromeTheme theme =
            intent->setting.value.chrome_theme;
        browser_profile_set_chrome_theme(profile, theme);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        char status[40];
        snprintf(
            status, sizeof(status), "THEME %s",
            theme == BROWSER_CHROME_THEME_OCEAN ? "OCEAN"
            : theme == BROWSER_CHROME_THEME_PLUM ? "PLUM"
            : theme == BROWSER_CHROME_THEME_EMBER ? "EMBER"
            : "FINCH");
        psp_ui_show_status(&app->process->presentation.ui, status, 180);
    }
    if (intent->setting.id == PSP_UI_SETTING_GLYPH_LANGUAGE) {
        BrowserGlyphLanguage language = intent->setting.value.glyph_language;
        browser_profile_set_glyph_language(profile, language);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            language == BROWSER_GLYPH_LANGUAGE_EMBEDDED
                ? "EMBEDDED CJK FALLBACK SELECTED"
                : "LANGUAGE PACK APPLIES AFTER RESTART",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_COLOR_EMOJI) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_color_emoji(profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            enabled ? "COLOR EMOJI APPLIES AFTER RESTART"
                    : "EMBEDDED EMOJI SELECTED",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_VIDEO_SCALING) {
        BrowserVideoScaling scaling = intent->setting.value.video_scaling;
        browser_profile_set_video_scaling(profile, scaling);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        /* Both presenters read the profile on every present, so a change
           lands as soon as a frame is drawn. In practice that is the next
           media open: Options cannot be reached while the player owns the
           display. */
        psp_ui_show_status(
            &app->process->presentation.ui,
            scaling == BROWSER_VIDEO_SCALING_SHARP
                ? "VIDEO SCALING SHARP"
                : "VIDEO SCALING SMOOTH",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_YOUTUBE_QUALITY) {
        BrowserYoutubeQuality quality =
            intent->setting.value.youtube_quality;
        browser_profile_set_youtube_quality(
            profile, quality);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            quality == BROWSER_YOUTUBE_QUALITY_360P
                ? "YOUTUBE QUALITY 360P"
                : "YOUTUBE QUALITY 240P",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS) {
        bool compact = intent->setting.value.boolean;
        browser_profile_set_youtube_compact_results(profile, compact);
        (void) browser_engine_set_youtube_compact_results(engine, compact);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            compact
                ? "YOUTUBE RESULTS COMPACT - RELOAD TO APPLY"
                : "YOUTUBE RESULTS DETAILED - RELOAD TO APPLY",
            240);
    }
    if (intent->setting.id
        == PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_video_startup_buffering(profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            enabled ? "VIDEO START BUFFERED"
                    : "VIDEO START IMMEDIATE",
            180);
    }
    if (intent->setting.id
        == PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_resume_offline_downloads(
            profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui,
            enabled ? "AUTO-RESUME SAVES ON"
                    : "AUTO-RESUME SAVES OFF",
            180);
    }
    if (intent->setting.id
        == PSP_UI_SETTING_CONTENT_BLOCKER_MODE) {
        ContentBlockerMode requested =
            intent->setting.value.content_blocker_mode;
        ContentBlockerMode previous =
            browser_profile_content_blocker_mode(profile);
        if (!browser_engine_content_blocker_configure(
                engine, requested, content_blocker_path)) {
            ContentBlockerMetrics retained = {0};
            app->process->presentation.ui.content_blocker_mode = (uint8_t)
                (browser_engine_content_blocker_metrics(
                     engine, &retained)
                    ? retained.mode : previous);
            app->process->presentation.ui.content_blocker_site_allowed =
                app->process->presentation.ui.content_blocker_mode != CONTENT_BLOCKER_OFF
                && retained.allowed_site_count
                       == browser_profile_content_blocker_allowed_site_count(
                              profile)
                && browser_profile_content_blocker_site_allowed(
                       profile, app->process->presentation.ui.url);
            psp_ui_show_status(
                &app->process->presentation.ui, "CUSTOM LIST NOT AVAILABLE", 240);
        } else {
            browser_profile_set_content_blocker_mode(
                profile, requested);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            app->process->presentation.ui.content_blocker_site_allowed =
                requested != CONTENT_BLOCKER_OFF
                && browser_profile_content_blocker_site_allowed(
                       profile, app->process->presentation.ui.url);
            ContentBlockerMetrics blocker_metrics = {0};
            (void) browser_engine_content_blocker_metrics(
                engine, &blocker_metrics);
            printf(
                "tilefinch-content-blocker: mode=%u rules=%zu "
                "allow-rules=%zu ignored=%zu retained=%zu\n",
                (unsigned) blocker_metrics.mode,
                blocker_metrics.rule_count,
                blocker_metrics.allow_rule_count,
                blocker_metrics.ignored_rule_count,
                blocker_metrics.retained_bytes);
            char status[64];
            snprintf(
                status, sizeof(status),
                requested == CONTENT_BLOCKER_OFF
                    ? "AD BLOCKING OFF"
                    : (requested == CONTENT_BLOCKER_BASIC
                           ? "AD BLOCKING BASIC"
                           : "CUSTOM %u RULES - %u IGNORED"),
                (unsigned) blocker_metrics.rule_count,
                (unsigned) blocker_metrics.ignored_rule_count);
            psp_ui_show_status(&app->process->presentation.ui, status, 180);
            const NavigationEntry *entry =
                navigation_current(app->views->navigation);
            (void) psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, app->process->presentation.ui.reader_mode,
                entry == NULL ? app->process->presentation.ui.url : entry->url,
                app->process->presentation.ui.page_font_percent, true);
            (void) psp_engine_views_refresh(app->views, engine);
            frame->page_dirty = true;
        }
    }
    if (intent->setting.id
        == PSP_UI_SETTING_CONTENT_BLOCKER_COSMETIC_HIDING) {
        bool enabled = intent->setting.value.boolean;
        bool previous =
            browser_profile_content_blocker_cosmetic_hiding(profile);
        browser_profile_set_content_blocker_cosmetic_hiding(
            profile, enabled);
        const NavigationEntry *entry =
            navigation_current(app->views->navigation);
        bool applied = psp_set_presentation_css(
            engine, &app->process->presentation.ui, profile, app->process->presentation.ui.reader_mode,
            entry == NULL ? app->process->presentation.ui.url : entry->url,
            app->process->presentation.ui.page_font_percent, true);
        if (!applied) {
            browser_profile_set_content_blocker_cosmetic_hiding(
                profile, previous);
            app->process->presentation.ui.content_blocker_cosmetic_hiding = previous;
            (void) psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile, app->process->presentation.ui.reader_mode,
                entry == NULL ? app->process->presentation.ui.url : entry->url,
                app->process->presentation.ui.page_font_percent, true);
        } else {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
        }
        (void) psp_engine_views_refresh(app->views, engine);
        frame->page_dirty = true;
        psp_ui_show_status(
            &app->process->presentation.ui,
            applied ? (enabled ? "PAGE AD HIDING ON"
                               : "PAGE AD HIDING OFF")
                    : "PAGE AD HIDING UNAVAILABLE",
            180);
    }
    if (intent->setting.id
        == PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED) {
        bool desired = intent->setting.value.boolean;
        bool previous =
            browser_profile_content_blocker_site_allowed(
                profile, app->process->presentation.ui.url);
        bool changed =
            browser_profile_set_content_blocker_site_allowed(
                profile, app->process->presentation.ui.url, desired);
        if (changed)
            changed = psp_content_blocker_apply_allowed_sites(
                engine, profile);
        if (!changed) {
            (void) browser_profile_set_content_blocker_site_allowed(
                profile, app->process->presentation.ui.url, previous);
            (void) psp_content_blocker_apply_allowed_sites(
                engine, profile);
            app->process->presentation.ui.content_blocker_site_allowed = previous;
            psp_ui_show_status(
                &app->process->presentation.ui, "SITE ALLOWLIST IS FULL", 240);
        } else {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            char reload_url[NAVIGATION_URL_LIMIT];
            snprintf(reload_url, sizeof(reload_url), "%s", app->process->presentation.ui.url);
            bool started = psp_begin_page_load(
                engine, &app->process->presentation.ui, profile, engine_frame, &app->process->text_input,
                reload_url, false, 4 * MIB, 30000);
            if (started) {
                app->interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
                psp_ui_show_status(
                    &app->process->presentation.ui,
                    desired ? "SITE ALLOWED - RELOADING"
                            : "BLOCKING SITE - RELOADING",
                    240);
            } else {
                psp_ui_show_status(
                    &app->process->presentation.ui, "SITE SETTING SAVED - RELOAD FAILED", 300);
            }
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_COOKIE_BANNER_HIDDEN) {
        bool hidden = intent->setting.value.boolean;
        bool previous = browser_profile_cookie_banner_hidden(
            profile, app->process->presentation.ui.url);
        bool changed = browser_profile_set_cookie_banner_hidden(
            profile, app->process->presentation.ui.url, hidden);
        const NavigationEntry *entry =
            navigation_current(app->views->navigation);
        const char *url = entry == NULL
            ? app->process->presentation.ui.url : entry->url;
        bool applied = changed && psp_set_presentation_css(
            engine, &app->process->presentation.ui, profile,
            app->process->presentation.ui.reader_mode, url,
            app->process->presentation.ui.page_font_percent, true);
        if (!applied) {
            (void) browser_profile_set_cookie_banner_hidden(
                profile, app->process->presentation.ui.url, previous);
            app->process->presentation.ui.cookie_banner_hidden = previous;
            (void) psp_set_presentation_css(
                engine, &app->process->presentation.ui, profile,
                app->process->presentation.ui.reader_mode, url,
                app->process->presentation.ui.page_font_percent, true);
        } else {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
        }
        (void) psp_engine_views_refresh(app->views, engine);
        frame->page_dirty = true;
        psp_ui_show_status(
            &app->process->presentation.ui,
            applied ? (hidden ? "COOKIE NOTICES HIDDEN"
                              : "COOKIE NOTICES SHOWN")
                    : "COOKIE NOTICE SETTING UNAVAILABLE",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_RESTORE_LAST_PAGE) {
        bool restore = intent->setting.value.boolean;
        browser_profile_set_restore_last_page(
            profile, restore);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        if (restore) {
            bool saved = psp_recovery_record_current(
                profile, recovery_path, app->views->navigation);
            app->interactive->recovery.entry = navigation_current(app->views->navigation);
            if (saved && app->interactive->recovery.entry != NULL) {
                app->interactive->recovery.observed_scroll =
                    app->interactive->recovery.entry->scroll_y;
                app->interactive->recovery.observed_generation =
                    app->views->navigation->generation;
                app->interactive->recovery.last_change_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
                app->interactive->recovery.dirty = false;
            }
            psp_ui_show_status(
                &app->process->presentation.ui, saved ? "LAST PAGE RESTORE ON"
                           : "RESTORE ON - SAVES NEXT PAGE",
                180);
        } else {
            (void) browser_recovery_clear(recovery_path);
            (void) remove(tab_session_path);
            app->interactive->recovery.dirty = false;
            psp_ui_show_status(
                &app->process->presentation.ui, "LAST PAGE RESTORE OFF", 180);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_TAB_HIBERNATION) {
        bool enabled = intent->setting.value.boolean;
        bool applied = true;
        if (!enabled && tabs != NULL) {
            for (size_t index = 0;
                 index < browser_tabs_count(tabs); index++) {
                if (browser_tabs_hibernated(tabs, index)
                    && !browser_tabs_rehydrate(
                           tabs, index,
                           tab_hibernation_path)) {
                    applied = false;
                    break;
                }
            }
        }
        if (!applied) {
            app->process->presentation.ui.tab_hibernation_enabled = true;
            psp_ui_show_status(
                &app->process->presentation.ui, "TAB NEEDS MEMORY BEFORE DISABLING", 240);
        } else {
            browser_profile_set_tab_hibernation_enabled(
                profile, enabled);
            if (!enabled) (void) remove(tab_hibernation_path);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
            psp_ui_show_status(
                &app->process->presentation.ui,
                enabled ? "TAB HIBERNATION ON"
                        : "TAB HIBERNATION OFF",
                180);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_EXPERIMENTAL_VOICE) {
        bool enabled = intent->setting.value.boolean;
        bool applied = psp_text_input_set_voice_enabled(
            &app->process->text_input, enabled);
        if (!applied && enabled) {
            app->process->presentation.ui.experimental_voice_input = false;
            browser_profile_set_experimental_voice_input(
                profile, false);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_ui_show_status(
                &app->process->presentation.ui, "VOICE MEMORY IS NOT AVAILABLE", 180);
        } else {
            browser_profile_set_experimental_voice_input(
                profile, enabled);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            psp_ui_show_status(
                &app->process->presentation.ui, enabled
                    ? "VOICE ON - SQUARE IN A TEXT FIELD"
                    : "EXPERIMENTAL VOICE OFF",
                180);
        }
    }
    if (intent->setting.id
        == PSP_UI_SETTING_ADAPTIVE_VOICE_MEMORY) {
        bool adaptive = intent->setting.value.boolean;
        browser_profile_set_adaptive_voice_memory(
            profile, adaptive);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_text_input_set_adaptive_voice_memory(
            &app->process->text_input, adaptive);
        psp_ui_show_status(
            &app->process->presentation.ui, adaptive
                ? "VOICE MEMORY ADAPTIVE"
                : "VOICE MEMORY FULL SPEED",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_ANALOG_CURSOR) {
        bool enabled = intent->setting.value.boolean;
        browser_profile_set_analog_cursor_enabled(
            profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_ui_show_status(
            &app->process->presentation.ui, enabled
                ? "ANALOG CURSOR ON"
                : "ANALOG SCROLLING ON",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_TEXT_ENTRY_MODE) {
        BrowserTextEntryMode mode =
            intent->setting.value.text_entry_mode;
        browser_profile_set_text_entry_mode(profile, mode);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        psp_text_input_set_danzeff_enabled(
            &app->process->text_input, mode == BROWSER_TEXT_ENTRY_DANZEFF);
        psp_ui_show_status(
            &app->process->presentation.ui, mode == BROWSER_TEXT_ENTRY_DANZEFF
                ? "DANZEFF ON - START OPENS IT"
                : "PSP KEYBOARD ON",
            180);
    }
    if (intent->setting.id == PSP_UI_SETTING_PERSISTENT_CACHE_MB) {
        unsigned persistent_cache_mb =
            intent->setting.value.unsigned_value;
        unsigned previous_cache_mb =
            browser_profile_persistent_cache_mb(profile);
        BrowserSessionPersistenceStatus remove_status =
            BROWSER_SESSION_PERSISTENCE_OK;
        if (persistent_cache_mb == 0
            && app->process->persistent_site_data_available) {
            remove_status = browser_session_persistence_remove(
                persistent_cache_path);
        }
        browser_profile_set_persistent_cache_mb(
            profile, persistent_cache_mb);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        if (remove_status != BROWSER_SESSION_PERSISTENCE_OK
            || !psp_profile_store_flush(&app->browser->profile_store)) {
            browser_profile_set_persistent_cache_mb(
                profile, previous_cache_mb);
            app->process->presentation.ui.persistent_cache_mb = previous_cache_mb;
            psp_ui_show_status(
                &app->process->presentation.ui, remove_status
                         != BROWSER_SESSION_PERSISTENCE_OK
                     ? "CACHE FILE COULD NOT BE REMOVED"
                     : "CACHE SETTING NOT SAVED",
                240);
        } else if (persistent_cache_mb == 0) {
            psp_ui_show_status(&app->process->presentation.ui, "DISK CACHE OFF", 180);
        } else {
            char status[48];
            snprintf(
                status, sizeof(status),
                "DISK CACHE %u MB - SAVES ON EXIT",
                persistent_cache_mb);
            psp_ui_show_status(&app->process->presentation.ui, status, 240);
        }
    }
    if (intent->setting.id == PSP_UI_SETTING_LIVE_CACHE_KIB) {
        unsigned live_cache_kib =
            intent->setting.value.unsigned_value;
        size_t new_limit = (size_t) live_cache_kib * KIB;
        if (browser_session_cache_set_maximum_bytes(
                session, new_limit)) {
            browser_profile_set_live_cache_kib(
                profile, live_cache_kib);
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, frame->ui_sample_us);
            char status[48];
            if (live_cache_kib < 1024u) {
                snprintf(
                    status, sizeof(status),
                    "MEMORY CACHE %u KB",
                    live_cache_kib);
            } else {
                snprintf(
                    status, sizeof(status),
                    "MEMORY CACHE %u MB",
                    live_cache_kib / 1024u);
            }
            psp_ui_show_status(&app->process->presentation.ui, status, 180);
        } else {
            app->process->presentation.ui.live_cache_kib =
                browser_profile_live_cache_kib(profile);
            psp_ui_show_status(
                &app->process->presentation.ui, "MEMORY CACHE SIZE UNAVAILABLE", 240);
        }
    }
    if (intent->setting.id
        == PSP_UI_SETTING_PERSIST_LOCAL_STORAGE) {
        bool persist = intent->setting.value.boolean;
        bool previous =
            browser_profile_persist_local_storage(profile);
        BrowserSessionPersistenceStatus remove_status =
            BROWSER_SESSION_PERSISTENCE_OK;
        if (!persist
            && app->process->persistent_site_data_available) {
            remove_status = browser_session_persistence_remove(
                local_storage_path);
        }
        browser_profile_set_persist_local_storage(
            profile, persist);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        if (remove_status != BROWSER_SESSION_PERSISTENCE_OK
            || !psp_profile_store_flush(&app->browser->profile_store)) {
            browser_profile_set_persist_local_storage(
                profile, previous);
            app->process->presentation.ui.persist_local_storage = previous;
            psp_ui_show_status(
                &app->process->presentation.ui, remove_status
                         != BROWSER_SESSION_PERSISTENCE_OK
                     ? "LOCAL FILE COULD NOT BE REMOVED"
                     : "LOCAL SETTING NOT SAVED",
                240);
        } else {
            psp_ui_show_status(
                &app->process->presentation.ui, persist
                    ? "LOCAL STORAGE SAVES ON EXIT"
                    : "LOCAL STORAGE STAYS IN THIS SESSION",
            240);
        }
    }
    if (intent->setting.id
        == PSP_UI_SETTING_TLS_SESSION_PERSISTENCE) {
        bool enabled = intent->setting.value.boolean;
        bool previous =
            browser_profile_tls_session_persistence(profile);
        browser_profile_set_tls_session_persistence(profile, enabled);
        psp_profile_store_mark_dirty(
            &app->browser->profile_store, frame->ui_sample_us);
        if (!psp_profile_store_flush(&app->browser->profile_store)) {
            browser_profile_set_tls_session_persistence(profile, previous);
            app->process->presentation.ui.tls_session_persistence = previous;
            psp_ui_show_status(
                &app->process->presentation.ui,
                "TLS TICKET SETTING NOT SAVED", 240);
        } else {
            bool store_ok =
                fetch_set_tls_session_persistence_enabled(enabled);
            psp_ui_show_status(
                &app->process->presentation.ui,
                enabled ? "TLS TICKET SAVING ON"
                        : (store_ok ? "TLS TICKET SAVING OFF"
                                    : "OFF - OLD TICKETS NOT REMOVED"),
                240);
        }
    }
}
