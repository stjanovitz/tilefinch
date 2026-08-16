#include "psp_app_internal.h"

bool psp_voice_component_handle_frame(
    PspApp *app, const PspUiIntent *intent, uint64_t now_us)
{
    if (app == NULL || app->process == NULL || app->browser == NULL
        || app->browser->voice_component_session == NULL
        || app->browser->profile == NULL
        || intent == NULL) return false;
    PspVoiceComponentSession *session = app->browser->voice_component_session;
    PspUiState *ui = &app->process->presentation.ui;
    bool visual_changed = false;

    if (intent->voice_component_probe_requested) {
        psp_voice_component_session_probe(session, &app->process->install_paths);
        if (session->installed
            && browser_profile_experimental_voice_input(app->browser->profile)) {
            (void) psp_text_input_set_voice_model_root(
                &app->process->text_input, session->model_path);
            ui->experimental_voice_input = psp_text_input_set_voice_enabled(
                &app->process->text_input, true);
        }
        psp_voice_component_session_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (intent->voice_component_remove_requested) {
        bool was_requested =
            browser_profile_experimental_voice_input(app->browser->profile);
        browser_profile_set_experimental_voice_input(app->browser->profile, false);
        psp_profile_store_mark_dirty(&app->browser->profile_store, now_us);
        bool preference_saved =
            psp_profile_store_flush(&app->browser->profile_store);
        if (!preference_saved) {
            browser_profile_set_experimental_voice_input(
                app->browser->profile, was_requested);
            psp_profile_store_mark_dirty(&app->browser->profile_store, now_us);
        }
        bool removed = preference_saved;
        if (removed) {
            ui->experimental_voice_input = false;
            (void) psp_text_input_set_voice_enabled(&app->process->text_input, false);
            removed = psp_voice_component_session_remove(
                session, &app->process->install_paths);
        }
        if (removed) {
            psp_ui_show_status(ui, "VOICE MODEL REMOVED", 180);
        } else {
            psp_ui_show_status(
                ui, "VOICE MODEL COULD NOT BE REMOVED", 240);
        }
        psp_voice_component_session_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (intent->voice_component_cancel_requested) {
        if (!psp_voice_component_session_cancel(session))
            psp_ui_show_status(
                ui, "VOICE MODEL IS FINISHING SAFELY", 180);
        visual_changed = true;
    }

    if (intent->voice_component_primary_requested) {
        if (!psp_voice_component_session_initialize(
                session, app->browser->budget, &app->process->install_paths)) {
            psp_ui_set_voice_component(
                ui, PSP_UI_VOICE_COMPONENT_ERROR, -1);
            psp_ui_show_status(
                ui, "SIGNED MODEL DOWNLOAD UNAVAILABLE", 240);
        } else {
            PspVoiceComponentPrimaryResult primary =
                psp_voice_component_session_primary(
                    session, &app->process->install_paths);
            if (primary == PSP_VOICE_COMPONENT_PRIMARY_CHECK_REQUIRED) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                char model_url[768];
                bool have_url = psp_voice_component_session_metadata_url(
                    model_url, sizeof(model_url));
                bool network_ready = strcmp(app->process->config.trace, "none") == 0
                    && have_url
                    && psp_ensure_network_for_navigation(
                           app->network, app->network_lifecycle,
                           (int) app->process->config.network_profile,
                           "GET", model_url, false, app->views->frame, ui);
                time_t now = time(NULL);
                if (network_ready) {
                    (void) psp_voice_component_session_begin_check(
                        session, now > 0 ? (uint64_t) now : 0, now > 0);
                } else {
                    psp_ui_show_status(
                        ui, "NETWORK NOT READY FOR MODEL", 240);
                }
#else
                psp_ui_show_status(
                    ui, "LIVE NETWORKING IS NOT IN THIS BUILD", 240);
#endif
            }
        }
        psp_voice_component_session_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (session->initialized) {
        bool was_installed = session->installed;
        (void) psp_voice_component_session_pump(
            session, &app->process->install_paths);
        psp_voice_component_session_refresh_ui(session, ui);
        if (!was_installed && session->installed) {
            (void) psp_text_input_set_voice_model_root(
                &app->process->text_input, session->model_path);
            bool requested =
                browser_profile_experimental_voice_input(app->browser->profile);
            bool enabled = requested
                && psp_text_input_set_voice_enabled(&app->process->text_input, true);
            ui->experimental_voice_input = enabled;
            if (requested && !enabled) {
                browser_profile_set_experimental_voice_input(
                    app->browser->profile, false);
                psp_profile_store_mark_dirty(
                    &app->browser->profile_store, now_us);
            }
            psp_ui_show_status(
                ui, "VOICE MODEL READY", 240);
        }
        visual_changed |=
            ui->screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS
            && psp_voice_component_session_active(session);
    }
    return visual_changed;
}
