#include "psp_app_internal.h"

/* The glyphs the interface language needs, attached beside (never instead
   of) the page glyph selection. */
BrowserGlyphLanguage psp_ui_language_glyphs(const BrowserProfile *profile)
{
    unsigned active = tilefinch_ui_translation_bound_language();
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(
        active ? active : browser_profile_ui_language(profile));
    return spec ? spec->glyphs : BROWSER_GLYPH_LANGUAGE_EMBEDDED;
}

bool psp_ui_language_font_available(const PspBrowserResources *browser)
{
    unsigned language = browser_profile_ui_language(browser->profile);
    TilefinchGlyphPack pack;
    return tilefinch_ui_language_spec(language)
        && (!psp_ui_language_pack(language, &pack)
            || ((browser->glyph_component_session->attached_mask & (1u << pack)) != 0
                && psp_ui_language_pack_current(browser->glyph_component_session, language)));
}

static void language_release_download(PspBrowserResources *browser);

void psp_ui_language_destroy(PspBrowserResources *browser)
{
    language_release_download(browser);
    browser->ui_language_waiting_font = false;
    tilefinch_ui_translation_bind(NULL);
    tilefinch_ui_translation_destroy(browser->ui_translation);
    browser->ui_translation = NULL;
}

/* A saved language whose catalog or pack went missing (an update moved to a
   new catalog version, say) falls back to English silently at boot. Say
   what is missing once, when the UI is first idle, and let X take the
   reader to the Interface language row, whose ordinary download fixes it.
   Scripted and replayed runs keep their deterministic frames. */
bool psp_ui_language_repair_poll(PspApp *app)
{
    PspBrowserResources *browser = app->browser;
    if (browser->ui_language_repair_checked) return false;
    if (strcmp(app->process->config.trace, "none") != 0) {
        browser->ui_language_repair_checked = true;
        return false;
    }
    unsigned language = browser_profile_ui_language(browser->profile);
    bool busy = browser->ui_language_request || browser->ui_language_waiting_font
        || psp_glyph_component_session_active(browser->glyph_component_session)
        || browser_engine_navigation_pending(browser->engine);
    return psp_ui_language_repair_offer_once(&browser->ui_language_repair_checked,
        &app->process->presentation.ui, &app->process->install_paths,
        browser->glyph_component_session, language,
        tilefinch_ui_translation_bound_language() == language, busy);
}

/* The Interface language row previews a choice with Left/Right. Whenever no
   selection is being saved or downloaded, and the row is not on screen, it
   shows the saved language again. */
static void language_row_restore(PspApp *app)
{
    app->process->presentation.ui.ui_language =
        browser_profile_ui_language(app->browser->profile);
}

/* Cancels a catalog download still in flight and frees its buffer. */
static void language_release_download(PspBrowserResources *browser)
{
    if (browser->ui_language_request)
        (void)fetch_background_transport_cancel(browser->ui_language_request, "language selection cancelled");
    browser->ui_language_request = 0;
    budget_free(browser->budget, browser->ui_language_download);
    browser->ui_language_download = NULL;
}

/* Ends a selection that did not complete: whatever is still marked in
   flight (download, font wait) is cancelled, the row shows the saved
   language again, and the toast says why. Callers clear work that already
   ended before calling. */
static void language_abort(PspApp *app, const char *status, unsigned frames)
{
    PspBrowserResources *browser = app->browser;
    language_release_download(browser);
    if (browser->ui_language_waiting_font)
        (void)psp_glyph_component_session_cancel(browser->glyph_component_session);
    browser->ui_language_waiting_font = false;
    language_row_restore(app);
    psp_ui_show_status(&app->process->presentation.ui, status, frames);
}

static void language_finish(PspApp *app, uint64_t now_us)
{
    PspBrowserResources *browser = app->browser;
    browser_profile_set_ui_language(browser->profile, browser->ui_language_requested);
    app->process->presentation.ui.ui_language = browser->ui_language_requested;
    psp_profile_store_mark_dirty(&browser->profile_store, now_us);
    browser->ui_language_waiting_font = false;
    psp_ui_show_status(&app->process->presentation.ui,
        "UI LANGUAGE SAVED - RESTART TO APPLY", 240);
}

static bool language_font_ready(PspApp *app, uint64_t now_us)
{
    unsigned language = app->browser->ui_language_requested;
    TilefinchGlyphPack pack;
    if (!tilefinch_ui_language_spec(language)) return false;
    if (!psp_ui_language_pack(language, &pack)) {
        language_finish(app, now_us);
        return true;
    }
    psp_glyph_component_session_probe(app->browser->glyph_component_session,
        &app->process->install_paths);
    if (psp_ui_language_pack_current(app->browser->glyph_component_session, language)) {
        language_finish(app, now_us);
        return true;
    }
    const char *failure = psp_glyph_component_request_pack(app, pack);
    app->browser->ui_language_waiting_font = false;
    if (failure) language_abort(app, failure, 240);
    else app->browser->ui_language_waiting_font = true;
    return true;
}

bool psp_ui_language_handle_frame(PspApp *app, const PspUiIntent *intent, uint64_t now_us)
{
    PspBrowserResources *browser = app->browser;
    PspUiState *ui = &app->process->presentation.ui;
    if (ui->screen != PSP_UI_SCREEN_GLYPH_OPTIONS
        && !browser->ui_language_request && !browser->ui_language_waiting_font)
        language_row_restore(app);
    if (intent->setting.id == PSP_UI_SETTING_UI_LANGUAGE) {
        if (browser->ui_language_request || browser->ui_language_waiting_font) {
            language_abort(app, "LANGUAGE DOWNLOAD CANCELLED", 180);
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
        if (tilefinch_ui_translation_installed(&app->process->install_paths, language))
            return language_font_ready(app, now_us);
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
            psp_ui_show_status(ui, "INSTALLING UI LANGUAGE", 180);
            return true;
        }
#endif
        language_abort(app, "LANGUAGE DOWNLOAD FAILED - TRY AGAIN", 240);
        return true;
    }
    if (browser->ui_language_request) {
        FetchBackgroundProgress progress;
        if (!fetch_background_transport_progress(browser->ui_language_request, &progress)) {
            browser->ui_language_request = 0; /* Already ended. */
            language_abort(app, "LANGUAGE DOWNLOAD FAILED - TRY AGAIN", 240);
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
        language_release_download(browser);
        if (okay) return language_font_ready(app, now_us);
        language_abort(app, "LANGUAGE DOWNLOAD FAILED - TRY AGAIN", 240);
        return true;
    }
    if (browser->ui_language_waiting_font
        && !psp_glyph_component_session_active(browser->glyph_component_session)) {
        if (psp_ui_language_pack_current(browser->glyph_component_session,
                browser->ui_language_requested))
            language_finish(app, now_us);
        else {
            browser->ui_language_waiting_font = false; /* The pack work ended. */
            language_abort(app, "LANGUAGE DOWNLOAD FAILED - TRY AGAIN", 240);
        }
        return true;
    }
    return psp_ui_language_repair_poll(app);
}
