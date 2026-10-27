/*
 * Settings shared between the PSP chrome and the app: the plain on/off
 * settings' descriptors, and the profile -> chrome mirror boot fills before
 * the first frame. Host-built with the chrome so both are unit-tested.
 */
#include "tilefinch/psp_ui_settings.h"

#include "tilefinch/build_version.h"

#define PSP_UI_BOOL_MIRROR(field) \
    static void psp_ui_mirror_##field(PspUiState *ui, bool enabled) \
    { \
        ui->field = enabled; \
    }

PSP_UI_BOOL_MIRROR(reader_auto_mode)
PSP_UI_BOOL_MIRROR(custom_homepage_enabled)
PSP_UI_BOOL_MIRROR(history_enabled)
PSP_UI_BOOL_MIRROR(color_emoji)
PSP_UI_BOOL_MIRROR(rotate_hint_always)
PSP_UI_BOOL_MIRROR(youtube_compact_results)
PSP_UI_BOOL_MIRROR(youtube_audio_only)
PSP_UI_BOOL_MIRROR(video_startup_buffering)
PSP_UI_BOOL_MIRROR(resume_offline_downloads)
PSP_UI_BOOL_MIRROR(analog_cursor_enabled)
PSP_UI_BOOL_MIRROR(youtube_topics)

/* The chrome stores the inverse: offers are on unless switched off. */
static void psp_ui_mirror_glyph_offers(PspUiState *ui, bool enabled)
{
    ui->glyph_offers_off = !enabled;
}

static const PspBoolSetting psp_bool_setting_table[] = {
    {
        .id = PSP_UI_SETTING_READER_AUTO_MODE,
        .profile_get = browser_profile_reader_auto_mode,
        .profile_set = browser_profile_set_reader_auto_mode,
        .ui_set = psp_ui_mirror_reader_auto_mode,
        .on_status = "AUTO READER ON",
        .off_status = "AUTO READER OFF",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_CUSTOM_HOMEPAGE,
        .profile_get = browser_profile_custom_homepage_enabled,
        .profile_set = browser_profile_set_custom_homepage_enabled,
        .ui_set = psp_ui_mirror_custom_homepage_enabled,
        .on_status = "HOME OPENS MY LINKS",
        .off_status = "HOME OPENS BUILT-IN PAGE",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_HISTORY,
        .profile_get = browser_profile_history_enabled,
        .profile_set = browser_profile_set_history_enabled,
        .ui_set = psp_ui_mirror_history_enabled,
        .on_status = "URL HISTORY ON",
        .off_status = "URL HISTORY OFF",
        .status_frames = 180
    },
    {
        /* The chrome keeps the serialized preference but spends no CPU or
           presentation bandwidth on ambient motion, so it has no mirror.
           A future GPU-owned version can adopt the same preference. */
        .id = PSP_UI_SETTING_WAVE_BACKGROUND,
        .profile_get = browser_profile_wave_background,
        .profile_set = browser_profile_set_wave_background,
        .on_status = "WAVE BACKGROUND ON",
        .off_status = "WAVE BACKGROUND OFF",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_COLOR_EMOJI,
        .profile_get = browser_profile_color_emoji,
        .profile_set = browser_profile_set_color_emoji,
        .ui_set = psp_ui_mirror_color_emoji,
        .on_status = "COLOR EMOJI APPLIES AFTER RESTART",
        .off_status = "EMBEDDED EMOJI SELECTED",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_GLYPH_OFFERS,
        .profile_get = browser_profile_glyph_offers_enabled,
        .profile_set = browser_profile_set_glyph_offers_enabled,
        .ui_set = psp_ui_mirror_glyph_offers,
        .on_status = "LANGUAGE PACK OFFERS ON",
        .off_status = "LANGUAGE PACK OFFERS OFF",
        .status_frames = 150
    },
    {
        .id = PSP_UI_SETTING_ROTATE_HINT_ALWAYS,
        .profile_get = browser_profile_rotate_hint_always,
        .profile_set = browser_profile_set_rotate_hint_always,
        .ui_set = psp_ui_mirror_rotate_hint_always,
        .on_status = "ROTATE HINT ALWAYS",
        .off_status = "ROTATE HINT AUTO",
        .status_frames = 150
    },
    {
        .id = PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS,
        .profile_get = browser_profile_youtube_compact_results,
        .profile_set = browser_profile_set_youtube_compact_results,
        .ui_set = psp_ui_mirror_youtube_compact_results,
        .engine_set = browser_engine_set_youtube_compact_results,
        .engine_get = browser_engine_youtube_compact_results,
        .on_status = "YOUTUBE RESULTS COMPACT - RELOAD TO APPLY",
        .off_status = "YOUTUBE RESULTS DETAILED - RELOAD TO APPLY",
        .status_frames = 240
    },
    {
        .id = PSP_UI_SETTING_YOUTUBE_AUDIO_ONLY,
        .profile_get = browser_profile_youtube_audio_only,
        .profile_set = browser_profile_set_youtube_audio_only,
        .ui_set = psp_ui_mirror_youtube_audio_only,
        .on_status = "AUDIO-ONLY ON - NEXT VIDEO",
        .off_status = "AUDIO-ONLY OFF - NEXT VIDEO",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING,
        .profile_get = browser_profile_video_startup_buffering,
        .profile_set = browser_profile_set_video_startup_buffering,
        .ui_set = psp_ui_mirror_video_startup_buffering,
        .on_status = "VIDEO START BUFFERED",
        .off_status = "VIDEO START IMMEDIATE",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS,
        .profile_get = browser_profile_resume_offline_downloads,
        .profile_set = browser_profile_set_resume_offline_downloads,
        .ui_set = psp_ui_mirror_resume_offline_downloads,
        .on_status = "AUTO-RESUME SAVES ON",
        .off_status = "AUTO-RESUME SAVES OFF",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_ANALOG_CURSOR,
        .profile_get = browser_profile_analog_cursor_enabled,
        .profile_set = browser_profile_set_analog_cursor_enabled,
        .ui_set = psp_ui_mirror_analog_cursor_enabled,
        .on_status = "ANALOG CURSOR ON",
        .off_status = "ANALOG SCROLLING ON",
        .status_frames = 180
    },
    {
        .id = PSP_UI_SETTING_YOUTUBE_TOPICS,
        .profile_get = browser_profile_youtube_topics,
        .profile_set = browser_profile_set_youtube_topics,
        .ui_set = psp_ui_mirror_youtube_topics,
        .engine_set = browser_engine_set_youtube_topics,
        .engine_get = browser_engine_youtube_topics,
        .on_status = "YOUTUBE HOME TOPICS ON",
        .off_status = "YOUTUBE HOME TOPICS OFF - RELOAD TO HIDE",
        .status_frames = 240
    }
};

#define PSP_BOOL_SETTING_COUNT \
    (sizeof(psp_bool_setting_table) / sizeof(psp_bool_setting_table[0]))

const PspBoolSetting *psp_bool_setting_find(PspUiSettingId id)
{
    for (size_t i = 0; i < PSP_BOOL_SETTING_COUNT; i++) {
        if (psp_bool_setting_table[i].id == id)
            return &psp_bool_setting_table[i];
    }
    return NULL;
}

const PspBoolSetting *psp_bool_settings(size_t *count)
{
    if (count != NULL) *count = PSP_BOOL_SETTING_COUNT;
    return psp_bool_setting_table;
}

PspSettingResult psp_bool_setting_apply(
    const PspBoolSetting *setting, BrowserProfile *profile,
    BrowserEngine *engine, PspUiState *ui, bool enabled)
{
    bool previous = setting->profile_get(profile);
    setting->profile_set(profile, enabled);
    PspSettingResult result = PSP_SETTING_APPLIED;
    if (setting->profile_get(profile) != enabled) {
        result = PSP_SETTING_PROFILE_REFUSED;
    } else if (setting->engine_set != NULL
               && !setting->engine_set(engine, enabled)) {
        setting->profile_set(profile, previous);
        result = PSP_SETTING_ENGINE_REFUSED;
    }
    bool in_effect = setting->profile_get(profile);
    if (setting->ui_set != NULL) setting->ui_set(ui, in_effect);
    psp_ui_show_status(
        ui, in_effect ? setting->on_status : setting->off_status,
        setting->status_frames);
    return result;
}

PspSettingResult psp_web_font_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    WebFontMode mode)
{
    WebFontMode previous = browser_profile_web_font_mode(profile);
    PspSettingResult result = PSP_SETTING_APPLIED;
    if (!browser_profile_set_web_font_mode(profile, mode)) {
        result = PSP_SETTING_PROFILE_REFUSED;
    } else if (!browser_engine_set_web_font_mode(engine, mode)) {
        (void) browser_profile_set_web_font_mode(profile, previous);
        result = PSP_SETTING_ENGINE_REFUSED;
    }
    WebFontMode in_effect = browser_profile_web_font_mode(profile);
    ui->web_font_mode = (unsigned) in_effect;
    psp_ui_show_status(ui,
        in_effect == WEB_FONTS_ON ? "WEBSITE FONTS ON"
            : in_effect == WEB_FONTS_OFF ? "WEBSITE FONTS OFF" : "WEBSITE FONTS SMART", 180);
    return result;
}

PspSettingResult psp_color_mode_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    BrowserColorMode mode, bool previous_dark)
{
    BrowserColorMode previous = browser_profile_color_mode(profile);
    browser_profile_set_color_mode(profile, mode);
    PspSettingResult result = PSP_SETTING_APPLIED;
    if (browser_profile_color_mode(profile) != mode) {
        result = PSP_SETTING_PROFILE_REFUSED;
    } else if (!browser_engine_set_forced_dark(engine, ui->page_dark)) {
        browser_profile_set_color_mode(profile, previous);
        result = PSP_SETTING_ENGINE_REFUSED;
    }
    if (result != PSP_SETTING_APPLIED) {
        ui->color_mode = browser_profile_color_mode(profile);
        ui->page_dark = previous_dark;
    }
    psp_ui_show_status(
        ui,
        ui->color_mode == BROWSER_COLOR_MODE_AUTO
            ? (ui->page_dark
                   ? "NIGHT MODE AUTO: DARK"
                   : "NIGHT MODE AUTO: LIGHT")
            : (ui->page_dark
                   ? "NIGHT MODE DARK"
                   : "NIGHT MODE LIGHT"),
        180);
    return result;
}

bool psp_color_mode_refresh_apply(
    BrowserEngine *engine, PspUiState *ui, bool was_dark)
{
    if (ui->page_dark == was_dark) return false;
    if (browser_engine_set_forced_dark(engine, ui->page_dark)) return true;
    ui->page_dark = was_dark;
    return false;
}

PspSettingResult psp_web_font_setting_boot(
    BrowserProfile *profile, BrowserEngine *engine)
{
    if (browser_engine_set_web_font_mode(
            engine, browser_profile_web_font_mode(profile)))
        return PSP_SETTING_APPLIED;
    const BrowserConfig *config = browser_engine_config(engine);
    if (config != NULL)
        (void) browser_profile_set_web_font_mode(
            profile, config->resources.web_font_mode);
    return PSP_SETTING_ENGINE_REFUSED;
}

unsigned psp_engine_settings_boot(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui)
{
    unsigned refused = 0u;
    if (!browser_engine_set_forced_dark(engine, ui->page_dark)) {
        bool dark = browser_engine_forced_dark(engine);
        BrowserColorMode mode = browser_profile_color_mode(profile);
        if (mode != BROWSER_COLOR_MODE_AUTO
            && (mode == BROWSER_COLOR_MODE_DARK) != dark)
            browser_profile_set_color_mode(
                profile,
                dark ? BROWSER_COLOR_MODE_DARK : BROWSER_COLOR_MODE_LIGHT);
        ui->color_mode = browser_profile_color_mode(profile);
        ui->page_dark = dark;
        refused++;
    }
    for (size_t i = 0; i < PSP_BOOL_SETTING_COUNT; i++) {
        const PspBoolSetting *setting = &psp_bool_setting_table[i];
        if (setting->engine_set == NULL
            || setting->engine_set(engine, setting->profile_get(profile)))
            continue;
        setting->profile_set(profile, setting->engine_get(engine));
        if (setting->ui_set != NULL)
            setting->ui_set(ui, setting->profile_get(profile));
        refused++;
    }
    return refused;
}

PspSettingResult psp_site_javascript_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    bool enabled)
{
    bool previous = browser_profile_site_javascript_enabled(profile, ui->url);
    if (previous == enabled) {
        ui->site_javascript_enabled = previous;
        psp_ui_show_status(
            ui, previous ? "SITE JAVASCRIPT ON" : "SITE JAVASCRIPT OFF", 240);
        return PSP_SETTING_UNCHANGED;
    }
    bool changed = browser_profile_set_site_javascript_enabled(
        profile, ui->url, enabled);
    bool effective = browser_profile_javascript_enabled(profile) && enabled;
    /* The engine takes no policy during a load: cancel it only for a
       change, which the caller follows with a reload. */
    if (changed && browser_engine_navigation_pending(engine))
        browser_engine_cancel_navigation(
            engine, "site JavaScript policy changed");
    if (changed && browser_engine_set_javascript_enabled(engine, effective))
        return PSP_SETTING_APPLIED;
    if (changed) {
        (void) browser_profile_set_site_javascript_enabled(
            profile, ui->url, previous);
    }
    ui->site_javascript_enabled = previous;
    psp_ui_show_status(ui, "SITE JAVASCRIPT SETTING UNAVAILABLE", 240);
    return changed ? PSP_SETTING_ENGINE_REFUSED : PSP_SETTING_PROFILE_REFUSED;
}

PspSettingResult psp_basic_fallback_setting_apply(
    BrowserProfile *profile, PspUiState *ui, BrowserBasicFallbackMode mode)
{
    PspSettingResult result =
        browser_profile_set_basic_fallback_mode(profile, mode)
            ? PSP_SETTING_APPLIED : PSP_SETTING_PROFILE_REFUSED;
    BrowserBasicFallbackMode in_effect =
        browser_profile_basic_fallback_mode(profile);
    ui->basic_fallback_mode = (unsigned) in_effect;
    psp_ui_show_status(
        ui,
        in_effect == BROWSER_BASIC_FALLBACK_ASK
            ? "BASIC VIEW FALLBACK: ASK"
            : (in_effect == BROWSER_BASIC_FALLBACK_OFF
                   ? "BASIC VIEW FALLBACK OFF"
                   : "BASIC VIEW FALLBACK: AUTOMATIC"),
        180);
    return result;
}

void psp_app_ui_sync_from_profile(
    PspUiState *ui, const BrowserProfile *profile)
{
    if (ui == NULL || profile == NULL) return;
    for (size_t i = 0; i < PSP_BOOL_SETTING_COUNT; i++) {
        const PspBoolSetting *setting = &psp_bool_setting_table[i];
        if (setting->ui_set != NULL)
            setting->ui_set(ui, setting->profile_get(profile));
    }
    ui->browser_ui_scale = browser_profile_ui_scale(profile);
    ui->page_font_percent = browser_profile_page_font_percent(profile);
    ui->reader_font_serif =
        browser_profile_reader_font(profile) == BROWSER_READER_FONT_SERIF;
    ui->remember_reader_site_scale =
        browser_profile_remember_reader_site_scale(profile);
    ui->basic_fallback_mode =
        (unsigned) browser_profile_basic_fallback_mode(profile);
    ui->web_font_mode = (unsigned) browser_profile_web_font_mode(profile);
    ui->restore_last_page = browser_profile_restore_last_page(profile);
    ui->tab_hibernation_enabled =
        browser_profile_tab_hibernation_enabled(profile);
    ui->adaptive_voice_memory =
        browser_profile_adaptive_voice_memory(profile);
    ui->danzeff_text_input =
        browser_profile_text_entry_mode(profile)
            == BROWSER_TEXT_ENTRY_DANZEFF;
    ui->gamepad_circle_primary =
        browser_profile_gamepad_face_mapping(profile)
            == TILEFINCH_GAMEPAD_FACE_O_PRIMARY;
    ui->persist_local_storage =
        browser_profile_persist_local_storage(profile);
    ui->keep_compiled_scripts =
        browser_profile_keep_compiled_scripts(profile);
    ui->save_diagnostic_reports =
        browser_profile_save_diagnostic_reports(profile);
    ui->wifi_diagnostics = browser_profile_wifi_diagnostics(profile);
    ui->youtube_login_policy = (uint8_t)
        browser_profile_youtube_login_policy(profile);
    ui->search_engine = browser_profile_search_engine(profile);
    ui->color_mode = browser_profile_color_mode(profile);
    ui->glyph_language = (unsigned) browser_profile_glyph_language(profile);
    ui->ui_language = browser_profile_ui_language(profile);
    ui->youtube_240p =
        browser_profile_youtube_quality(profile)
            == BROWSER_YOUTUBE_QUALITY_240P;
    ui->video_language = (uint8_t) browser_profile_video_language(profile);
    ui->subtitle_language = (uint8_t)
        browser_profile_subtitle_language(profile);
    ui->alternate_language = (uint8_t)
        browser_profile_alternate_language(profile);
    ui->subtitle_size = (uint8_t) browser_profile_subtitle_size(profile);
    ui->subtitle_background = (uint8_t)
        browser_profile_subtitle_background(profile);
    ui->video_scaling_sharp =
        browser_profile_video_scaling(profile) == BROWSER_VIDEO_SCALING_SHARP;
    ui->save_playback_positions =
        browser_profile_save_playback_positions(profile);
    ui->content_blocker_cosmetic_hiding =
        browser_profile_content_blocker_cosmetic_hiding(profile);
    ui->update_check_enabled = browser_profile_update_check_enabled(profile);
    ui->update_channel = (uint8_t) browser_profile_update_channel(profile);
    /* A release recorded by an earlier boot's background check stays marked
       until this build's own sequence catches up to it. */
    ui->update_release_available =
        browser_profile_update_check_available_sequence(profile)
            > TILEFINCH_RELEASE_SEQUENCE;
}
