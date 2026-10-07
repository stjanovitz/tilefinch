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
            && pack == TILEFINCH_GLYPH_PACK_LATIN_EXTENDED)
        || (language == BROWSER_GLYPH_LANGUAGE_ARABIC
            && pack == TILEFINCH_GLYPH_PACK_ARABIC)
        || (language == BROWSER_GLYPH_LANGUAGE_HEBREW
            && pack == TILEFINCH_GLYPH_PACK_HEBREW);
}

/* ---- In-page language-pack offer (docs/TEXT_BIDI.md#glyph-components) */

typedef struct {
    PspApp *app;
    const FontFace *face;
} GlyphOfferProbe;

static bool glyph_offer_has_glyph(void *opaque, unsigned codepoint)
{
    const GlyphOfferProbe *probe = opaque;
    return font_face_has_codepoint(probe->face, codepoint);
}

/* The session's mask is complete only after a probe; the resolver's few
   stat calls cover a pack installed earlier but not attached this boot.
   Called at most once per page by the offer's page-identity gate. */
static bool glyph_offer_installed(void *opaque, TilefinchGlyphPack pack)
{
    const GlyphOfferProbe *probe = opaque;
    if (psp_glyph_component_session_installed(
            probe->app->browser->glyph_component_session, pack)) return true;
    char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
    return tilefinch_glyph_component_resolve(
        &probe->app->process->install_paths, pack, resolved,
        sizeof(resolved));
}

/* Shows the offer on a settled page that needs it. Ordinary pages leave at
   the census load; a page that qualified costs one URL hash per frame. */
static bool glyph_offer_poll(PspApp *app, PspGlyphComponentSession *session,
                             PspUiState *ui)
{
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    const DocumentGlyphCensus *census =
        browser_engine_glyph_census(app->browser->engine);
    if (census == NULL || census->offer_mask == 0) return false;
    /* Never over a menu, a loading page, another notice, a game that owns
       the controls, or a running pack operation. Scripted and replayed runs
       have no network to install from, so they never see it. */
    if (ui->screen != PSP_UI_SCREEN_PAGE || ui->loading
        || ui->toast_frames != 0 || ui->page_fullscreen
        || ui->page_gamepad_capture || ui->captive_portal_active
        || !app->process->install_paths.slotted
        || strcmp(app->process->config.trace, "none") != 0
        || session->offer_install_pending || session->offer_size_pending
        || psp_glyph_component_session_active(session)
        || browser_engine_navigation_pending(app->browser->engine))
        return false;
    GlyphOfferProbe probe = {
        .app = app,
        .face = browser_engine_font_face(app->browser->engine, FONT_SANS)
    };
    if (probe.face == NULL) return false;
    TilefinchGlyphOfferPage page = {
        .url = ui->url,
        .census = census,
        .declined_mask = browser_profile_glyph_offer_declined_mask(
            app->browser->profile),
        .offers_off = !browser_profile_glyph_offers_enabled(
            app->browser->profile),
        .has_glyph = glyph_offer_has_glyph,
        .installed = glyph_offer_installed,
        .opaque = &probe
    };
    TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_COUNT;
    if (!tilefinch_glyph_offer_poll(&session->offer, &page, &pack))
        return false;
    psp_ui_show_glyph_offer(ui, (uint8_t) pack);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-glyph-offer: offered pack=%u scripts=0x%04x\n",
           (unsigned) pack, (unsigned) census->offer_mask);
#endif
    return true;
#else
    (void) app;
    (void) session;
    (void) ui;
    return false;
#endif
}

/* The client's signed-metadata check, started exactly as the menu's
   download starts it. NULL on success, otherwise the existing notice. */
static const char *glyph_begin_check(PspApp *app,
                                     PspGlyphComponentSession *session,
                                     PspUiState *ui, TilefinchGlyphPack pack)
{
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    char metadata_url[768];
    bool have_url = psp_glyph_component_session_metadata_url(
        pack, metadata_url, sizeof(metadata_url));
    bool network_ready =
        strcmp(app->process->config.trace, "none") == 0
        && have_url
        && psp_ensure_network_for_navigation(
               app->network, app->network_lifecycle,
               (int) app->process->config.network_profile,
               "GET", metadata_url, false,
               app->views->frame, ui);
    time_t now = time(NULL);
    if (!network_ready) return "NETWORK NOT READY FOR FONT PACK";
    (void) psp_glyph_component_session_begin_check(
        session, now > 0 ? (uint64_t) now : 0, now > 0);
    return NULL;
#else
    (void) app;
    (void) session;
    (void) ui;
    (void) pack;
    return "LIVE NETWORKING IS NOT IN THIS BUILD";
#endif
}

/* Publishes the confirmation's signed size or failure once known. */
static bool glyph_offer_size_track(PspGlyphComponentSession *session,
                                   PspUiState *ui)
{
    if (!session->offer_size_pending) return false;
    TilefinchGlyphPack pack = session->offer_install_pack;
    uint8_t showing = 0;
    if (!psp_ui_glyph_offer_confirming(ui, &showing)
        || showing != (uint8_t) pack) {
        session->offer_size_pending = false;
        return false;
    }
    uint64_t bytes = 0;
    const char *message = NULL;
    switch (psp_glyph_component_session_size(
                session, pack, &bytes, &message)) {
        case PSP_GLYPH_COMPONENT_SIZE_CHECKING:
            return false;
        case PSP_GLYPH_COMPONENT_SIZE_READY:
            psp_ui_set_glyph_offer_size(ui, (uint8_t) pack, bytes);
            break;
        case PSP_GLYPH_COMPONENT_SIZE_FAILED:
        case PSP_GLYPH_COMPONENT_SIZE_NONE:
            psp_ui_set_glyph_offer_error(ui, (uint8_t) pack, message);
            break;
    }
    session->offer_size_pending = false;
    return true;
}

static void glyph_offer_answer(PspApp *app,
                               PspGlyphComponentSession *session,
                               PspUiState *ui, const PspUiIntent *intent,
                               uint64_t now_us)
{
    TilefinchGlyphPack pack = (TilefinchGlyphPack) intent->glyph_component_pack;
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    if (spec == NULL) return;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-glyph-offer: answer=%u pack=%u\n",
           (unsigned) intent->glyph_offer_answer, (unsigned) pack);
#endif
    switch ((PspUiGlyphOfferAnswer) intent->glyph_offer_answer) {
        case PSP_UI_GLYPH_OFFER_CONFIRM: {
            /* Verify the signed metadata for its size; stop there. */
            session->offer_install_pack = pack;
            if (!psp_glyph_component_session_prepare_size_check(
                    session, app->browser->budget,
                    &app->process->install_paths, pack)) {
                psp_ui_set_glyph_offer_error(
                    ui, (uint8_t) pack, "SIGNED PACK DOWNLOAD UNAVAILABLE");
                break;
            }
            session->offer_size_pending = true;
            if (psp_glyph_component_session_size(session, pack, NULL, NULL)
                    == PSP_GLYPH_COMPONENT_SIZE_READY) break;
            const char *failure = glyph_begin_check(app, session, ui, pack);
            if (failure != NULL) {
                session->offer_size_pending = false;
                psp_ui_set_glyph_offer_error(ui, (uint8_t) pack, failure);
            }
            break;
        }
        case PSP_UI_GLYPH_OFFER_CANCEL:
            if (session->offer_size_pending
                && psp_glyph_component_session_size(
                       session, pack, NULL, NULL)
                       == PSP_GLYPH_COMPONENT_SIZE_CHECKING)
                (void) psp_glyph_component_session_cancel(session);
            session->offer_size_pending = false;
            break;
        case PSP_UI_GLYPH_OFFER_INSTALL: {
            /* The second X. The same intent carries
               glyph_component_primary_requested, so the menu's own download,
               verification and Memory Stick install run below, with their
               space checks and refusal messages. */
            session->offer_size_pending = false;
            session->offer_install_pending = true;
            session->offer_install_pack = pack;
            char status[PSP_UI_STATUS_CAPACITY];
            snprintf(status, sizeof(status), "%s pack: downloading...",
                     spec->label);
            psp_ui_show_status(ui, status, 240);
            break;
        }
        case PSP_UI_GLYPH_OFFER_NEVER:
            browser_profile_set_glyph_offer_declined_mask(
                app->browser->profile,
                (uint16_t) (browser_profile_glyph_offer_declined_mask(
                                app->browser->profile)
                            | (1u << (unsigned) pack)));
            psp_profile_store_mark_dirty(
                &app->browser->profile_store, now_us);
            psp_ui_show_status(ui, "Won't ask again. Install it later in\n"
                                   "Settings > Appearance > Language & emoji",
                               300);
            break;
        case PSP_UI_GLYPH_OFFER_NONE:
            break;
    }
}

/* Any completed install, from the menu or the offer, detached every runtime
   pack. Put back the selected language and emoji, attach installed packs
   for the page, and relayout in place: text measured against blank cells
   needs new advances, not only a repaint. */
static void glyph_install_finish(PspApp *app,
                                 PspGlyphComponentSession *session,
                                 PspUiState *ui)
{
    TilefinchGlyphPack pack = session->operation_pack;
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    BrowserEngine *engine = app->browser->engine;
    bool from_offer = session->offer_install_pending
        && session->offer_install_pack == pack;
    uint16_t scripts = browser_engine_glyph_script_mask(engine);
    if (from_offer && spec != NULL) scripts = spec->page_scripts;
    (void) psp_glyph_component_session_reattach(
        session, app->browser->budget, &app->process->install_paths,
        browser_profile_glyph_language(app->browser->profile),
        browser_profile_color_emoji(app->browser->profile), scripts,
        engine);
    if (from_offer) session->offer_install_pending = false;
    char status[PSP_UI_STATUS_CAPACITY];
    if (spec == NULL
        || (session->attached_mask & (1u << (unsigned) pack)) == 0) {
        snprintf(status, sizeof(status), "%s", "FONT PACK READY AFTER RESTART");
    } else {
        const NavigationEntry *entry =
            navigation_current(app->views->navigation);
        bool relaid = entry != NULL && psp_set_presentation_css(
            engine, ui, app->browser->profile,
            ui->reader_mode || ui->basic_mode, entry->url,
            ui->page_font_percent, true);
        if (relaid) (void) psp_engine_views_refresh(app->views, engine);
        snprintf(status, sizeof(status),
                 relaid ? "%s pack installed"
                        : "%s pack installed\nReload the page to redraw it",
                 spec->label);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-glyph-component: installed pack=%u offer=%u "
           "attached=0x%04x\n", (unsigned) pack, from_offer ? 1u : 0u,
           (unsigned) session->attached_mask);
#endif
    psp_ui_show_status(ui, status, 240);
}

/* Follows an offer-started operation to its end. Progress never displaces
   another notice; a failure shows the operation's own message. */
static bool glyph_offer_track(PspGlyphComponentSession *session,
                              PspUiState *ui)
{
    if (!session->offer_install_pending) return false;
    TilefinchGlyphPack pack = session->offer_install_pack;
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    if (spec == NULL) {
        session->offer_install_pending = false;
        return false;
    }
    if (psp_glyph_component_session_installed(session, pack)) {
        session->offer_install_pending = false;
        return false;
    }
    char status[PSP_UI_STATUS_CAPACITY];
    int family = snprintf(status, sizeof(status), "%s pack:", spec->label);
    if (psp_glyph_component_session_active(session)) {
        /* glyph_refresh_ui() has already published phase and per-mille
           progress for the menu row; the toast reads the same state. */
        unsigned progress = ui->glyph_component_progress_plus_one == 0 ? 0u
            : ((unsigned) ui->glyph_component_progress_plus_one - 1u) / 10u;
        switch ((PspUiGlyphComponentPhase) ui->glyph_component_phase) {
            case PSP_UI_GLYPH_COMPONENT_DOWNLOADING:
                snprintf(status + family, sizeof(status) - (size_t) family,
                         " downloading %u%%", progress);
                break;
            case PSP_UI_GLYPH_COMPONENT_INSTALLING:
                snprintf(status + family, sizeof(status) - (size_t) family,
                         " installing %u%%", progress);
                break;
            default:
                snprintf(status + family, sizeof(status) - (size_t) family,
                         " checking...");
                break;
        }
        psp_ui_keep_progress_status(ui, status, (size_t) family, 240);
        return true;
    }
    /* Not installed and nothing running: refused, offline, failed or
       cancelled. Offline already raised the menu path's own notice. */
    session->offer_install_pending = false;
    const char *message = NULL;
    if (session->installer != NULL
        && (session->install_snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
            || session->install_snapshot.phase
                   == TILEFINCH_UPDATE_INSTALL_CANCELLED))
        message = session->install_snapshot.message;
    else if (session->client != NULL
             && session->client_snapshot.phase
                    == TILEFINCH_UPDATE_CLIENT_ERROR)
        message = session->client_snapshot.message;
    if (message != NULL && message[0] != '\0')
        psp_ui_show_status(ui, message, 240);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-glyph-offer: install-ended pack=%u message=\"%s\"\n",
           (unsigned) pack, message == NULL ? "" : message);
#endif
    return true;
}

bool psp_glyph_component_handle_frame(
    PspApp *app, const PspUiIntent *intent, uint64_t now_us)
{
    if (app == NULL || app->process == NULL || app->browser == NULL
        || app->browser->glyph_component_session == NULL
        || app->browser->profile == NULL || intent == NULL) return false;
    if (psp_captive_portal_active(app->interactive)) return false;
    PspGlyphComponentSession *session =
        app->browser->glyph_component_session;
    PspUiState *ui = &app->process->presentation.ui;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint16_t attached_before_hint = session->attached_mask;
#endif
    bool visual_changed = psp_glyph_component_session_attach_hinted(
        session, &app->process->install_paths,
        browser_engine_glyph_script_mask(app->browser->engine),
        app->browser->engine);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (session->attached_mask != attached_before_hint) {
        printf("tilefinch-glyph-component: lazy-attached=0x%04x "
               "total=0x%04x\n",
               (unsigned) (session->attached_mask &
                           (uint16_t) ~attached_before_hint),
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

    if (intent->glyph_offer_answer != PSP_UI_GLYPH_OFFER_NONE) {
        glyph_offer_answer(app, session, ui, intent, now_us);
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
                const char *failure =
                    glyph_begin_check(app, session, ui, requested);
                if (failure != NULL) psp_ui_show_status(ui, failure, 240);
            }
        }
        glyph_refresh_ui(session, ui);
        visual_changed = true;
    }

    if (session->operation_initialized) {
        (void) psp_glyph_component_session_pump_operation(
            session, &app->process->install_paths, app->browser->engine);
        glyph_refresh_ui(session, ui);
        visual_changed |= (ui->screen == PSP_UI_SCREEN_GLYPH_OPTIONS
                           || ui->screen == PSP_UI_SCREEN_GLYPH_OFFER)
            && psp_glyph_component_session_active(session);
    }
    if (session->reattach_pending) {
        glyph_install_finish(app, session, ui);
        glyph_refresh_ui(session, ui);
        visual_changed = true;
    }
    visual_changed |= glyph_offer_size_track(session, ui);
    visual_changed |= glyph_offer_track(session, ui);
    if (glyph_offer_poll(app, session, ui)) visual_changed = true;
    return visual_changed;
}
