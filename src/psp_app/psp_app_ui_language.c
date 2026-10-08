#include "psp_app_internal.h"

BrowserGlyphLanguage psp_ui_language_glyphs(const BrowserProfile *profile)
{
    unsigned active = tilefinch_ui_translation_bound_language();
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(
        active ? active : browser_profile_ui_language(profile));
    return spec && spec->glyphs != BROWSER_GLYPH_LANGUAGE_EMBEDDED
        ? spec->glyphs : browser_profile_glyph_language(profile);
}

bool psp_ui_language_font_available(const PspBrowserResources *browser)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(
        browser_profile_ui_language(browser->profile));
    TilefinchGlyphPack pack;
    return spec
        && (!tilefinch_glyph_pack_for_language(spec->glyphs, &pack)
            || ((browser->glyph_component_session->attached_mask & (1u << pack)) != 0
                && psp_glyph_component_session_installed_at_least(
                    browser->glyph_component_session, pack,
                    spec->minimum_glyph_sequence)));
}

void psp_ui_language_destroy(PspBrowserResources *browser)
{
    if (browser->ui_language_request)
        (void)fetch_background_transport_cancel(browser->ui_language_request, "language selection cancelled");
    browser->ui_language_request = 0;
    budget_free(browser->budget, browser->ui_language_download);
    browser->ui_language_download = NULL;
    browser->ui_language_waiting_font = false;
    tilefinch_ui_translation_bind(NULL);
    tilefinch_ui_translation_destroy(browser->ui_translation);
    browser->ui_translation = NULL;
}

static void language_finish(PspApp *app, uint64_t now_us)
{
    PspBrowserResources *browser = app->browser;
    browser_profile_set_ui_language(browser->profile, browser->ui_language_requested);
    app->process->presentation.ui.ui_language = browser->ui_language_requested;
    psp_profile_store_mark_dirty(&browser->profile_store, now_us);
    browser->ui_language_waiting_font = false;
    psp_ui_show_status(&app->process->presentation.ui,
        tilefinch_ui_text("UI LANGUAGE SAVED - RESTART TO APPLY"), 240);
}

static bool language_font_ready(PspApp *app, uint64_t now_us)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(app->browser->ui_language_requested);
    TilefinchGlyphPack pack;
    if (!spec) return false;
    if (!tilefinch_glyph_pack_for_language(spec->glyphs, &pack)) {
        language_finish(app, now_us);
        return true;
    }
    psp_glyph_component_session_probe(app->browser->glyph_component_session,
        &app->process->install_paths);
    if (psp_glyph_component_session_installed_at_least(
            app->browser->glyph_component_session, pack, spec->minimum_glyph_sequence)) {
        language_finish(app, now_us);
        return true;
    }
    const char *failure = psp_glyph_component_request_pack(app, pack);
    if (failure) {
        app->browser->ui_language_waiting_font = false;
        psp_ui_show_status(&app->process->presentation.ui, failure, 240);
    } else app->browser->ui_language_waiting_font = true;
    return true;
}

bool psp_ui_language_handle_frame(PspApp *app, const PspUiIntent *intent, uint64_t now_us)
{
    PspBrowserResources *browser = app->browser;
    PspUiState *ui = &app->process->presentation.ui;
    if (intent->setting.id == PSP_UI_SETTING_UI_LANGUAGE) {
        if (browser->ui_language_request || browser->ui_language_waiting_font) {
            if (browser->ui_language_request)
                (void)fetch_background_transport_cancel(browser->ui_language_request, "language selection cancelled");
            if (browser->ui_language_waiting_font)
                (void)psp_glyph_component_session_cancel(browser->glyph_component_session);
            browser->ui_language_request = 0;
            browser->ui_language_waiting_font = false;
            budget_free(browser->budget, browser->ui_language_download);
            browser->ui_language_download = NULL;
            ui->ui_language = browser_profile_ui_language(browser->profile);
            psp_ui_show_status(ui, tilefinch_ui_text("LANGUAGE DOWNLOAD CANCELLED"), 180);
            return true;
        }
        unsigned language = intent->setting.value.unsigned_value;
        if (!tilefinch_ui_language_spec(language)) return false;
        browser->ui_language_requested = language;
        if (language == TILEFINCH_UI_LANGUAGE_ENGLISH) {
            language_finish(app, now_us);
            return true;
        }
        if (psp_glyph_component_session_active(browser->glyph_component_session)) {
            psp_ui_show_status(ui, "FINISH THE CURRENT FONT DOWNLOAD FIRST", 180);
            return true;
        }
        TilefinchUiTranslation *installed = tilefinch_ui_translation_load(browser->budget,
            &app->process->install_paths, language);
        if (installed) {
            tilefinch_ui_translation_destroy(installed);
            return language_font_ready(app, now_us);
        }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        char url[256];
        if (strcmp(app->process->config.trace, "none") == 0
            && app->process->install_paths.slotted
            && tilefinch_ui_language_url(language, url, sizeof(url))
            && psp_ensure_network_for_navigation(app->network, app->network_lifecycle,
                (int)app->process->config.network_profile, "GET", url, false,
                app->views->frame, ui)) {
            browser->ui_language_download = budget_malloc(browser->budget, TILEFINCH_UI_TRANSLATION_BYTES);
            if (browser->ui_language_download) {
                FetchRequest request = {.method="GET", .credentials=FETCH_CREDENTIALS_OMIT,
                    .page_redirect_mode=FETCH_PAGE_REDIRECT_ERROR};
                browser->ui_language_request = fetch_background_transport_enqueue(url, &request,
                    TILEFINCH_UI_TRANSLATION_BYTES, 20000);
            }
        }
        if (browser->ui_language_request) {
            psp_ui_show_status(ui, tilefinch_ui_text("INSTALLING UI LANGUAGE"), 180);
            return true;
        }
#endif
        budget_free(browser->budget, browser->ui_language_download);
        browser->ui_language_download = NULL;
        psp_ui_show_status(ui, tilefinch_ui_text("LANGUAGE DOWNLOAD FAILED - TRY AGAIN"), 240);
        return true;
    }
    if (browser->ui_language_request) {
        FetchBackgroundProgress progress;
        if (!fetch_background_transport_progress(browser->ui_language_request, &progress)) {
            browser->ui_language_request = 0;
            budget_free(browser->budget, browser->ui_language_download);
            browser->ui_language_download = NULL;
            psp_ui_show_status(ui, tilefinch_ui_text("LANGUAGE DOWNLOAD FAILED - TRY AGAIN"), 240);
            return true;
        }
        if (!progress.complete) return false;
        FetchBackgroundMediaResponse response;
        bool okay = fetch_background_transport_take_media(browser->ui_language_request,
            browser->ui_language_download, TILEFINCH_UI_TRANSLATION_BYTES, &response);
        browser->ui_language_request = 0;
        okay = okay && response.success && response.status_code == 200
            && tilefinch_ui_translation_install(browser->budget, &app->process->install_paths,
                browser->ui_language_requested, browser->ui_language_download, response.length);
        budget_free(browser->budget, browser->ui_language_download);
        browser->ui_language_download = NULL;
        if (okay) return language_font_ready(app, now_us);
        psp_ui_show_status(ui, tilefinch_ui_text("LANGUAGE DOWNLOAD FAILED - TRY AGAIN"), 240);
        return true;
    }
    if (browser->ui_language_waiting_font
        && !psp_glyph_component_session_active(browser->glyph_component_session)) {
        TilefinchGlyphPack pack;
        const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(browser->ui_language_requested);
        if (spec && tilefinch_glyph_pack_for_language(spec->glyphs, &pack)
            && psp_glyph_component_session_installed_at_least(
                browser->glyph_component_session, pack, spec->minimum_glyph_sequence))
            language_finish(app, now_us);
        else {
            browser->ui_language_waiting_font = false;
            psp_ui_show_status(ui, tilefinch_ui_text("LANGUAGE DOWNLOAD FAILED - TRY AGAIN"), 240);
        }
        return true;
    }
    return false;
}
