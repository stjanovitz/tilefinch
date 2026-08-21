#include "psp_app_internal.h"

static void glyph_refresh_ui(
    const PspGlyphComponentSession *session, PspUiState *ui)
{
    if (session == NULL || ui == NULL) return;
    PspUiGlyphComponentPhase phase =
        psp_glyph_component_session_installed(
            session, session->operation_pack)
        ? PSP_UI_GLYPH_COMPONENT_READY
        : PSP_UI_GLYPH_COMPONENT_NOT_INSTALLED;
    int progress = -1;
    if (session->installer != NULL) {
        if (session->install_snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
            || session->install_snapshot.phase
                   == TILEFINCH_UPDATE_INSTALL_CANCELLED) {
            phase = PSP_UI_GLYPH_COMPONENT_ERROR;
        } else if (session->install_snapshot.phase
                       < TILEFINCH_UPDATE_INSTALL_COMPLETE) {
            phase = PSP_UI_GLYPH_COMPONENT_INSTALLING;
            if (session->install_snapshot.bytes_total != 0)
                progress = (int) (
                    session->install_snapshot.bytes_processed * 1000u
                    / session->install_snapshot.bytes_total);
        }
    } else if (session->client != NULL) {
        switch (session->client_snapshot.phase) {
            case TILEFINCH_UPDATE_CLIENT_CHECKING:
                phase = PSP_UI_GLYPH_COMPONENT_CHECKING;
                break;
            case TILEFINCH_UPDATE_CLIENT_DOWNLOADING:
            case TILEFINCH_UPDATE_CLIENT_CANCELLING:
                phase = PSP_UI_GLYPH_COMPONENT_DOWNLOADING;
                if (session->client_snapshot.bytes_total != 0)
                    progress = (int) (
                        session->client_snapshot.bytes_received * 1000u
                        / session->client_snapshot.bytes_total);
                break;
            case TILEFINCH_UPDATE_CLIENT_ERROR:
                phase = PSP_UI_GLYPH_COMPONENT_ERROR;
                break;
            default:
                break;
        }
    }
    psp_ui_set_glyph_component(
        ui, session->installed_mask, (uint8_t) session->operation_pack,
        phase, progress);
}

static bool profile_language_uses_pack(
    const BrowserProfile *profile, TilefinchGlyphPack pack)
{
    BrowserGlyphLanguage language = browser_profile_glyph_language(profile);
    return (language == BROWSER_GLYPH_LANGUAGE_JAPANESE
            && pack == TILEFINCH_GLYPH_PACK_JAPANESE)
        || (language == BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED
            && pack == TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED)
        || (language == BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL
            && pack == TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL)
        || (language == BROWSER_GLYPH_LANGUAGE_KOREAN
            && pack == TILEFINCH_GLYPH_PACK_KOREAN)
        || (language == BROWSER_GLYPH_LANGUAGE_CYRILLIC
            && pack == TILEFINCH_GLYPH_PACK_CYRILLIC)
        || (language == BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED
            && pack == TILEFINCH_GLYPH_PACK_LATIN_EXTENDED);
}

bool psp_glyph_component_handle_frame(
    PspApp *app, const PspUiIntent *intent, uint64_t now_us)
{
    if (app == NULL || app->process == NULL || app->browser == NULL
        || app->browser->glyph_component_session == NULL
        || app->browser->profile == NULL || intent == NULL) return false;
    PspGlyphComponentSession *session =
        app->browser->glyph_component_session;
    PspUiState *ui = &app->process->presentation.ui;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint8_t attached_before_hint = session->attached_mask;
#endif
    bool visual_changed = psp_glyph_component_session_attach_hinted(
        session, &app->process->install_paths,
        browser_engine_glyph_script_mask(app->browser->engine),
        app->browser->engine);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (session->attached_mask != attached_before_hint) {
        printf("tilefinch-glyph-component: lazy-attached=0x%02x "
               "total=0x%02x\n",
               (unsigned) (session->attached_mask &
                           (uint8_t) ~attached_before_hint),
               (unsigned) session->attached_mask);
    }
#endif
    visual_changed = psp_glyph_component_session_pump_runtime(
        session, app->browser->engine) || visual_changed;

    if (intent->glyph_component_probe_requested) {
        psp_glyph_component_session_probe(
            session, &app->process->install_paths);
        glyph_refresh_ui(session, ui);
        visual_changed = true;
    }

    TilefinchGlyphPack requested =
        intent->glyph_component_pack < TILEFINCH_GLYPH_PACK_COUNT
        ? (TilefinchGlyphPack) intent->glyph_component_pack
        : TILEFINCH_GLYPH_PACK_JAPANESE;
    if (intent->glyph_component_remove_requested) {
        bool requested_attached =
            (session->attached_mask & (1u << (unsigned) requested)) != 0;
        bool selection_changed = false;
        BrowserGlyphLanguage previous_language =
            browser_profile_glyph_language(app->browser->profile);
        bool previous_emoji =
            browser_profile_color_emoji(app->browser->profile);
        if (profile_language_uses_pack(app->browser->profile, requested)) {
            browser_profile_set_glyph_language(
                app->browser->profile, BROWSER_GLYPH_LANGUAGE_EMBEDDED);
            ui->glyph_language = BROWSER_GLYPH_LANGUAGE_EMBEDDED;
            selection_changed = true;
        }
        if (requested == TILEFINCH_GLYPH_PACK_COLOR_EMOJI
            && previous_emoji) {
            browser_profile_set_color_emoji(app->browser->profile, false);
            ui->color_emoji = false;
            selection_changed = true;
        }
        if (selection_changed) {
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, now_us);
        }
        bool preference_saved = !selection_changed
            || psp_profile_store_flush(&app->browser->profile_store);
        if (!preference_saved) {
            browser_profile_set_glyph_language(
                app->browser->profile, previous_language);
            browser_profile_set_color_emoji(
                app->browser->profile, previous_emoji);
            ui->glyph_language = previous_language;
            ui->color_emoji = previous_emoji;
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, now_us);
        }
        bool removed = preference_saved
            && psp_glyph_component_session_remove(
                session, &app->process->install_paths, requested,
                app->browser->engine);
        if (removed && requested_attached) {
            (void) psp_glyph_component_session_attach_selected(
                session, app->browser->budget,
                &app->process->install_paths,
                browser_profile_glyph_language(app->browser->profile),
                browser_profile_color_emoji(app->browser->profile));
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        printf("tilefinch-glyph-component: remove pack=%u result=%s "
               "selection=%u\n",
               (unsigned) requested, removed ? "ok" : "failed",
               (unsigned) browser_profile_glyph_language(
                   app->browser->profile));
#endif
        psp_ui_show_status(
            ui, removed ? "PACK REMOVED - EMBEDDED FALLBACK ACTIVE"
                        : "FONT PACK COULD NOT BE REMOVED",
            240);
        glyph_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (intent->glyph_component_cancel_requested) {
        if (!psp_glyph_component_session_cancel(session))
            psp_ui_show_status(ui, "FONT PACK IS FINISHING SAFELY", 180);
        visual_changed = true;
    }

    if (intent->glyph_component_primary_requested) {
        bool selected = session->operation_initialized
            && session->operation_pack == requested;
        if (!selected && !psp_glyph_component_session_select_operation(
                session, app->browser->budget,
                &app->process->install_paths, requested)) {
            psp_ui_set_glyph_component(
                ui, session->installed_mask, (uint8_t) requested,
                PSP_UI_GLYPH_COMPONENT_ERROR, -1);
            psp_ui_show_status(ui, "SIGNED PACK DOWNLOAD UNAVAILABLE", 240);
        } else {
            PspGlyphComponentPrimaryResult primary =
                psp_glyph_component_session_primary(
                    session, &app->process->install_paths,
                    app->browser->engine);
            if (primary == PSP_GLYPH_COMPONENT_PRIMARY_CHECK_REQUIRED) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                char metadata_url[768];
                bool have_url = psp_glyph_component_session_metadata_url(
                    requested, metadata_url, sizeof(metadata_url));
                bool network_ready =
                    strcmp(app->process->config.trace, "none") == 0
                    && have_url
                    && psp_ensure_network_for_navigation(
                           app->network, app->network_lifecycle,
                           (int) app->process->config.network_profile,
                           "GET", metadata_url, false,
                           app->views->frame, ui);
                time_t now = time(NULL);
                if (network_ready) {
                    (void) psp_glyph_component_session_begin_check(
                        session, now > 0 ? (uint64_t) now : 0,
                        now > 0);
                } else {
                    psp_ui_show_status(
                        ui, "NETWORK NOT READY FOR FONT PACK", 240);
                }
#else
                psp_ui_show_status(
                    ui, "LIVE NETWORKING IS NOT IN THIS BUILD", 240);
#endif
            }
        }
        glyph_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (session->operation_initialized) {
        uint8_t before = session->installed_mask;
        (void) psp_glyph_component_session_pump_operation(
            session, &app->process->install_paths, app->browser->engine);
        glyph_refresh_ui(session, ui);
        if (before != session->installed_mask)
            psp_ui_show_status(ui, "FONT PACK READY AFTER RESTART", 240);
        visual_changed |= ui->screen == PSP_UI_SCREEN_GLYPH_OPTIONS
            && psp_glyph_component_session_active(session);
    }
    return visual_changed;
}
