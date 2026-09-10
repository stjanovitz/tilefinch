#include "tilefinch/psp_ui.h"
#include "tilefinch/psp_ui_theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
#include <pspkernel.h>
#endif

#include "tilefinch/danzeff_input.h"
#include "tilefinch/build_version.h"
#include "tilefinch/glyph_component_store.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/update_history.h"
#include "psp_boot_mark.h"
#include "psp_ui_menu.h"
#include "psp_ui_theme.h"
#include "tilefinch_compiler.h"
/* The decoder-program picker offers exactly the spellings the boot config
   gate accepts, from the one table that defines them, so no selection can
   ever produce the halt a hand-edited typo still gets. */
#include "media_backend_psp_policy.h"

_Static_assert(TILEFINCH_GLYPH_PACK_COUNT
                   <= PSP_UI_GLYPH_INSTALLED_MASK_BITS,
               "installed glyph packs exceed PspUiState mask width");
_Static_assert(BROWSER_CHROME_THEME_COUNT <= 8,
               "chrome themes exceed PspUiState representation");

/* The private token sheet supplies geometry and the built-in palette source.
   Painters use the selected runtime palette for colors. Presentation is
   serialized on PSP, so selection and painting cannot race. */
#undef PSP_THEME_GROUND
#undef PSP_THEME_GROUND_READER
#undef PSP_THEME_CHROME_BAR
#undef PSP_THEME_HINT_BAR
#undef PSP_THEME_PANEL
#undef PSP_THEME_SURFACE
#undef PSP_THEME_SURFACE_FOCUS
#undef PSP_THEME_LINE
#undef PSP_THEME_TEXT
#undef PSP_THEME_TEXT_BODY
#undef PSP_THEME_TEXT_MUTED
#undef PSP_THEME_TEXT_FAINT
#undef PSP_THEME_ACCENT_EMBER
#undef PSP_THEME_ACCENT_EMBER_HI
#undef PSP_THEME_ON_ACCENT
#undef PSP_THEME_OK
#undef PSP_THEME_WARN
#define PSP_THEME_GROUND (psp_ui_theme_active_palette->ground)
#define PSP_THEME_GROUND_READER (psp_ui_theme_active_palette->ground_reader)
#define PSP_THEME_CHROME_BAR (psp_ui_theme_active_palette->chrome_bar)
#define PSP_THEME_HINT_BAR (psp_ui_theme_active_palette->hint_bar)
#define PSP_THEME_PANEL (psp_ui_theme_active_palette->panel)
#define PSP_THEME_SURFACE (psp_ui_theme_active_palette->surface)
#define PSP_THEME_SURFACE_FOCUS (psp_ui_theme_active_palette->surface_focus)
#define PSP_THEME_LINE (psp_ui_theme_active_palette->line)
#define PSP_THEME_TEXT (psp_ui_theme_active_palette->text)
#define PSP_THEME_TEXT_BODY (psp_ui_theme_active_palette->text_body)
#define PSP_THEME_TEXT_MUTED (psp_ui_theme_active_palette->text_muted)
#define PSP_THEME_TEXT_FAINT (psp_ui_theme_active_palette->text_faint)
#define PSP_THEME_ACCENT_EMBER (psp_ui_theme_active_palette->accent)
#define PSP_THEME_ACCENT_EMBER_HI (psp_ui_theme_active_palette->accent_high)
#define PSP_THEME_ON_ACCENT (psp_ui_theme_active_palette->on_accent)
#define PSP_THEME_OK (psp_ui_theme_active_palette->ok)
#define PSP_THEME_WARN (psp_ui_theme_active_palette->warn)
#define UI_COLLECTIONS_ACTIVATION_FEEDBACK UINT8_C(0x80)

#define UI_TOP_HEIGHT 39
#define UI_BOTTOM_HEIGHT 21
#define UI_BROWSER_CHROME_CACHE_WIDTH 480
#define UI_BROWSER_CHROME_CACHE_BOTTOM_HEIGHT 29
#define UI_AUTOHIDE_FRAMES 240u
#define UI_TOAST_DEFAULT_FRAMES 180u
#define UI_MEDIA_CONTROLS_MS 3000u
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
#define UI_OPTIONS_ITEM_COUNT 42u
#else
#define UI_OPTIONS_ITEM_COUNT 40u
#endif
#define UI_DATA_OPTIONS_ITEM_COUNT 7u
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
/* The decoder-program picker rides with the other validation-only diagnostic
   rows (Power test, Video test): a shipping build has no A/B to run and must
   not offer a knob whose non-default settings hardware has already rejected. */
#define UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT 7u
#define UI_EXPERIMENTAL_ROW_VIDEO_DECODER 6u
#else
#define UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT 6u
#endif
#define UI_OPTIONS_VISIBLE_ROWS 7u
#define UI_ROUTED_LIST_VISIBLE_ROWS 7u
#define UI_ANALOG_DEAD_ZONE 24
#define UI_ANALOG_MAX_ELAPSED_MS 64u
#define UI_ANALOG_MAX_HOLD_MS 1500u
#define UI_CURSOR_HIDE_MS 2000u
#define UI_FOCUS_REPEAT_DELAY_MS 360u
#define UI_FOCUS_REPEAT_INTERVAL_MS 120u
#define UI_CURSOR_MAX_X 479000
#define UI_CURSOR_MAX_Y 271000
#define UI_TEXT_SUGGESTION_LABEL_CAPACITY 64
/*
 * HOME geometry. Fixed at build time so hit-testing, focus traversal, and
 * drawing all read the same numbers and no frame has to measure anything.
 * Three columns of tiles in two rows, then the CONTINUE list.
 *
 * The margin is PSP_THEME_SPACE_XL and the numbers below are derived from
 * it so the two rails agree: 3 * 144 + 2 * 8 gutter = 448 = 480 - 2 * 16,
 * exactly the CONTINUE row width, so the grid and the list share both
 * edges. Bars stay full-bleed at inset PSP_THEME_SPACE_L -- that chrome
 * -vs-surface distinction is deliberate.
 */
#define UI_HOME_MARGIN 16
#define UI_HOME_TILE_COLUMNS 3
#define UI_HOME_TILE_WIDTH 144
#define UI_HOME_TILE_HEIGHT 56
#define UI_HOME_TILE_STRIDE_X 152
#define UI_HOME_TILE_STRIDE_Y 64
#define UI_HOME_TILE_TOP 26
#define UI_HOME_CONTINUE_LABEL_Y 152
#define UI_HOME_CONTINUE_TOP 166
#define UI_HOME_CONTINUE_HEIGHT 19
#define UI_HOME_CONTINUE_STRIDE 20

/* COLLECTIONS geometry: a section strip, then a fixed window of rows. Rows
   carry a 15px title over a 15px detail, so the box is tall enough for two
   scale-2 lines with a little air between them and between neighbours. */
#define UI_COLLECTIONS_SECTION_TOP 22
#define UI_COLLECTIONS_SECTION_HEIGHT 16
#define UI_COLLECTIONS_ROW_TOP 46
#define UI_COLLECTIONS_ROW_HEIGHT 32
#define UI_COLLECTIONS_ROW_STRIDE 35
#define UI_COLLECTIONS_ROW_DETAIL_DY 17
#define UI_COLLECTIONS_VISIBLE_ROWS 6u

#define UI_CHROME_GLYPH_FIRST 32u
#define UI_CHROME_GLYPH_LAST 126u
#define UI_CHROME_GLYPH_COUNT \
    (UI_CHROME_GLYPH_LAST - UI_CHROME_GLYPH_FIRST + 1u)
#define UI_CHROME_UNICODE_GLYPH_LIMIT 16u

typedef struct {
    FontGlyph glyph;
    uint32_t codepoint;
    uint32_t age;
    uint8_t weight;
    uint8_t size;
    uint8_t loaded;
} UiChromeUnicodeGlyph;

typedef struct {
    FontGlyph glyphs[2][2][UI_CHROME_GLYPH_COUNT];
    uint8_t loaded[2][2][UI_CHROME_GLYPH_COUNT];
    UiChromeUnicodeGlyph unicode[UI_CHROME_UNICODE_GLYPH_LIMIT];
    uint32_t unicode_clock;
    const FontFace *faces[2];
} UiChromeFontCache;

static UiChromeFontCache chrome_font_cache;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t battery_percent;
    bool charging;
    /* Mirrors PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT; the device reads it once
       at startup because it cannot change mid-session. */
    bool twelve_hour;
    bool valid;
    /* Wifi strength as 0..4 filled bars. Gated separately from `valid`
       because the interface report only answers while the link is READY:
       the clock and battery can be valid with no signal to show. When
       `wifi_valid` is false the bars are absent, not zero -- the same
       discipline the clock keeps when it has no reading. */
    bool wifi_valid;
    uint8_t wifi_bars;
    char clock[9];
} UiDeviceStatus;

#define UI_WIFI_BARS_MAX 4u

static UiDeviceStatus device_status;

static void ui_format_clock(char *out, size_t size);

typedef enum {
    UI_OPTION_BROWSER_UI_SCALE = 0,
    UI_OPTION_PAGE_FONT_PERCENT,
    UI_OPTION_READER_FONT,
    UI_OPTION_REMEMBER_READER_SCALE,
    UI_OPTION_CUSTOM_HOMEPAGE,
    UI_OPTION_HISTORY,
    UI_OPTION_RESTORE_LAST_PAGE,
    UI_OPTION_TAB_HIBERNATION,
    UI_OPTION_EXPERIMENTAL,
    UI_OPTION_TEXT_ENTRY,
    UI_OPTION_ANALOG_CURSOR,
    UI_OPTION_JAVASCRIPT,
    UI_OPTION_SITE_JAVASCRIPT,
    UI_OPTION_SEARCH_ENGINE,
    UI_OPTION_COLOR_MODE,
    UI_OPTION_CHROME_THEME,
    UI_OPTION_LANGUAGE_AND_EMOJI,
    UI_OPTION_VIDEO_SCALING,
    UI_OPTION_YOUTUBE_QUALITY,
    UI_OPTION_YOUTUBE_AUDIO_ONLY,
    UI_OPTION_YOUTUBE_RESULTS,
    UI_OPTION_VIDEO_STARTUP_BUFFERING,
    UI_OPTION_RESUME_DOWNLOADS,
    UI_OPTION_CONTENT_BLOCKER,
    UI_OPTION_COSMETIC_HIDING,
    UI_OPTION_COOKIE_BANNERS,
    UI_OPTION_ALLOW_SITE,
    UI_OPTION_LOAD_ALLOWLIST,
    UI_OPTION_SITE_DATA_ALLOWED,
    UI_OPTION_TLS_SESSION_PERSISTENCE,
    UI_OPTION_MIXED_CONTENT_SITE,
    UI_OPTION_THIRD_PARTY_COOKIES_SITE,
    UI_OPTION_NETWORK_PROFILE,
    UI_OPTION_DIAGNOSTIC_QR,
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
    UI_OPTION_POWER_TEST,
    UI_OPTION_MEDIA_TEST,
#endif
    UI_OPTION_UPDATE_CHECK,
    UI_OPTION_UPDATE,
    UI_OPTION_SITE_DATA,
    UI_OPTION_READER_AUTO_MODE,
    UI_OPTION_VIDEO_LANGUAGE,
    UI_OPTION_GAMEPAD_FACE_MAPPING
} UiOptionId;

_Static_assert(BROWSER_VIDEO_LANGUAGE_COUNT <= 16,
               "video language no longer fits PspUiState");
_Static_assert(BROWSER_SUBTITLE_LANGUAGE_COUNT <= 16,
               "subtitle language no longer fits PspUiState");
_Static_assert(BROWSER_ALTERNATE_LANGUAGE_COUNT <= 16,
               "alternate language no longer fits PspUiState");
_Static_assert(BROWSER_SUBTITLE_SIZE_COUNT <= 2
               && BROWSER_SUBTITLE_BACKGROUND_COUNT <= 2,
               "subtitle style no longer fits PspUiState");

static const UiOptionId ui_option_order[UI_OPTIONS_ITEM_COUNT] = {
    UI_OPTION_BROWSER_UI_SCALE,
    UI_OPTION_PAGE_FONT_PERCENT,
    UI_OPTION_READER_FONT,
    UI_OPTION_REMEMBER_READER_SCALE,
    UI_OPTION_CUSTOM_HOMEPAGE,
    UI_OPTION_HISTORY,
    UI_OPTION_RESTORE_LAST_PAGE,
    UI_OPTION_TAB_HIBERNATION,
    UI_OPTION_EXPERIMENTAL,
    UI_OPTION_TEXT_ENTRY,
    UI_OPTION_ANALOG_CURSOR,
    UI_OPTION_JAVASCRIPT,
    UI_OPTION_SITE_JAVASCRIPT,
    UI_OPTION_SEARCH_ENGINE,
    UI_OPTION_COLOR_MODE,
    UI_OPTION_CHROME_THEME,
    UI_OPTION_LANGUAGE_AND_EMOJI,
    UI_OPTION_VIDEO_SCALING,
    UI_OPTION_YOUTUBE_QUALITY,
    UI_OPTION_YOUTUBE_AUDIO_ONLY,
    UI_OPTION_YOUTUBE_RESULTS,
    UI_OPTION_VIDEO_STARTUP_BUFFERING,
    UI_OPTION_RESUME_DOWNLOADS,
    UI_OPTION_CONTENT_BLOCKER,
    UI_OPTION_COSMETIC_HIDING,
    UI_OPTION_COOKIE_BANNERS,
    UI_OPTION_ALLOW_SITE,
    UI_OPTION_LOAD_ALLOWLIST,
    UI_OPTION_SITE_DATA_ALLOWED,
    UI_OPTION_TLS_SESSION_PERSISTENCE,
    UI_OPTION_MIXED_CONTENT_SITE,
    UI_OPTION_THIRD_PARTY_COOKIES_SITE,
    UI_OPTION_NETWORK_PROFILE,
    UI_OPTION_DIAGNOSTIC_QR,
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
    UI_OPTION_POWER_TEST,
    UI_OPTION_MEDIA_TEST,
#endif
    UI_OPTION_UPDATE_CHECK,
    UI_OPTION_UPDATE,
    UI_OPTION_SITE_DATA,
    /* Appended so existing option indices remain stable for input scripts
       and deterministic previews. Group navigation still presents each row
       under its declared category. */
    UI_OPTION_READER_AUTO_MODE,
    UI_OPTION_VIDEO_LANGUAGE,
    UI_OPTION_GAMEPAD_FACE_MAPPING
};

static UiOptionId ui_option_id(size_t selection)
{
    return ui_option_order[
        selection < UI_OPTIONS_ITEM_COUNT ? selection : 0u];
}

static bool ui_glyph_language_pack(
    unsigned language, TilefinchGlyphPack *pack);

static const char *ui_option_group(UiOptionId option)
{
    switch (option) {
        case UI_OPTION_BROWSER_UI_SCALE:
        case UI_OPTION_PAGE_FONT_PERCENT:
        case UI_OPTION_READER_FONT:
        case UI_OPTION_REMEMBER_READER_SCALE:
        case UI_OPTION_READER_AUTO_MODE:
        case UI_OPTION_COLOR_MODE:
        case UI_OPTION_CHROME_THEME:
        case UI_OPTION_LANGUAGE_AND_EMOJI:
            return "APPEARANCE";
        case UI_OPTION_CUSTOM_HOMEPAGE:
        case UI_OPTION_HISTORY:
        case UI_OPTION_RESTORE_LAST_PAGE:
        case UI_OPTION_TAB_HIBERNATION:
        case UI_OPTION_TEXT_ENTRY:
        case UI_OPTION_ANALOG_CURSOR:
        case UI_OPTION_GAMEPAD_FACE_MAPPING:
        case UI_OPTION_JAVASCRIPT:
        case UI_OPTION_SEARCH_ENGINE:
            return "BROWSING & INPUT";
        case UI_OPTION_VIDEO_SCALING:
        case UI_OPTION_YOUTUBE_QUALITY:
        case UI_OPTION_VIDEO_LANGUAGE:
        case UI_OPTION_YOUTUBE_AUDIO_ONLY:
        case UI_OPTION_YOUTUBE_RESULTS:
        case UI_OPTION_VIDEO_STARTUP_BUFFERING:
        case UI_OPTION_RESUME_DOWNLOADS:
            return "VIDEO";
        case UI_OPTION_CONTENT_BLOCKER:
        case UI_OPTION_COSMETIC_HIDING:
        case UI_OPTION_LOAD_ALLOWLIST:
        case UI_OPTION_SITE_DATA_ALLOWED:
        case UI_OPTION_TLS_SESSION_PERSISTENCE:
            return "PRIVACY & SECURITY";
        case UI_OPTION_SITE_JAVASCRIPT:
        case UI_OPTION_COOKIE_BANNERS:
        case UI_OPTION_ALLOW_SITE:
        case UI_OPTION_MIXED_CONTENT_SITE:
        case UI_OPTION_THIRD_PARTY_COOKIES_SITE:
            return "SITE";
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        case UI_OPTION_POWER_TEST:
        case UI_OPTION_MEDIA_TEST:
#endif
        case UI_OPTION_NETWORK_PROFILE:
        case UI_OPTION_SITE_DATA:
            return "DEVICE & STORAGE";
        case UI_OPTION_UPDATE_CHECK:
        case UI_OPTION_UPDATE:
            return "UPDATES";
        case UI_OPTION_DIAGNOSTIC_QR:
            return "HELP";
        case UI_OPTION_EXPERIMENTAL:
            return "ADVANCED & EXPERIMENTAL";
    }
    return "";
}

static const char *ui_option_group_at(size_t group)
{
    static const char *groups[] = {
        "Appearance", "Browsing & input", "Video",
        "Privacy & security", "Device & storage", "Updates",
        "Advanced & experimental"
    };
    return groups[group < UI_SETTINGS_GROUP_COUNT ? group : 0u];
}

static size_t ui_option_group_index(UiOptionId option)
{
    const char *group = ui_option_group(option);
    if (strcmp(group, "BROWSING & INPUT") == 0) return 1u;
    if (strcmp(group, "VIDEO") == 0) return 2u;
    if (strcmp(group, "PRIVACY & SECURITY") == 0) return 3u;
    if (strcmp(group, "DEVICE & STORAGE") == 0) return 4u;
    if (strcmp(group, "UPDATES") == 0) return 5u;
    if (strcmp(group, "ADVANCED & EXPERIMENTAL") == 0) return 6u;
    /* SITE and HELP are routed through dedicated surfaces. */
    if (strcmp(group, "SITE") == 0 || strcmp(group, "HELP") == 0)
        return UI_SETTINGS_GROUP_COUNT;
    return 0u;
}

size_t psp_ui_menu_option_first_in_group(size_t group)
{
    /* Keyboard choice is the entry point to the browsing group. It used to
       sit behind four unrelated rows even though Start/text-field activation
       is the only way users encounter it, making the shipped Danzeff option
       effectively undiscoverable. Keep the serialized option IDs and the
       remaining navigation order stable; only choose the useful first row. */
    if (group == 1u) {
        for (size_t at = 0; at < UI_OPTIONS_ITEM_COUNT; at++) {
            if (ui_option_id(at) == UI_OPTION_TEXT_ENTRY) return at;
        }
    }
    for (size_t at = 0; at < UI_OPTIONS_ITEM_COUNT; at++) {
        if (ui_option_group_index(ui_option_id(at)) == group) return at;
    }
    return 0u;
}

static size_t ui_option_step_in_group(size_t selection, int direction)
{
    size_t group = ui_option_group_index(ui_option_id(selection));
    size_t at = selection;
    for (size_t visited = 0; visited < UI_OPTIONS_ITEM_COUNT; visited++) {
        at = direction < 0
            ? (at == 0 ? UI_OPTIONS_ITEM_COUNT - 1u : at - 1u)
            : (at + 1u) % UI_OPTIONS_ITEM_COUNT;
        if (ui_option_group_index(ui_option_id(at)) == group) return at;
    }
    return selection;
}

static const char *ui_option_description(UiOptionId option)
{
    switch (option) {
        case UI_OPTION_BROWSER_UI_SCALE:
            return "Changes browser controls, not page text";
        case UI_OPTION_PAGE_FONT_PERCENT:
            return "Larger page text means more scrolling";
        case UI_OPTION_READER_FONT:
            return "Typeface used by Reader mode";
        case UI_OPTION_REMEMBER_READER_SCALE:
            return "Remember Reader size for this site";
        case UI_OPTION_READER_AUTO_MODE:
            return "Use Reader automatically on clear matches";
        case UI_OPTION_CUSTOM_HOMEPAGE:
            return "Use your bookmark launch page";
        case UI_OPTION_HISTORY:
            return "Keep at most 100 recent addresses";
        case UI_OPTION_RESTORE_LAST_PAGE:
            return "Restore tabs after a controlled exit";
        case UI_OPTION_TAB_HIBERNATION:
            return "Trade Memory Stick latency for free RAM";
        case UI_OPTION_EXPERIMENTAL:
            return "Optional features still being validated";
        case UI_OPTION_TEXT_ENTRY:
            return "Danzeff uses analog groups and face buttons";
        case UI_OPTION_ANALOG_CURSOR:
            return "Move a pointer with the analog stick";
        case UI_OPTION_GAMEPAD_FACE_MAPPING:
            return "Primary face button while Page controls are on";
        case UI_OPTION_JAVASCRIPT:
            return "Run scripts on pages across all sites";
        case UI_OPTION_SITE_JAVASCRIPT:
            return "Override scripts for the current website";
        case UI_OPTION_SEARCH_ENGINE:
            return "Used when the address is not a URL";
        case UI_OPTION_COLOR_MODE:
            return "Auto follows the PSP clock";
        case UI_OPTION_CHROME_THEME:
            return "X chooses built-in or data/themes file";
        case UI_OPTION_VIDEO_SCALING:
            return "Smooth uses the graphics chip; Sharp the CPU";
        case UI_OPTION_LANGUAGE_AND_EMOJI:
            return "Optional signed packs; embedded fallback remains";
        case UI_OPTION_YOUTUBE_QUALITY:
            return "360p is sharper; 240p uses less memory";
        case UI_OPTION_VIDEO_LANGUAGE:
            return "Rank YouTube audio and subtitle tracks";
        case UI_OPTION_YOUTUBE_AUDIO_ONLY:
            return "Skip video downloads and decoding on YouTube";
        case UI_OPTION_YOUTUBE_RESULTS:
            return "Choose detailed cards or denser rows";
        case UI_OPTION_VIDEO_STARTUP_BUFFERING:
            return "Wait for a stable source prefix before play";
        case UI_OPTION_RESUME_DOWNLOADS:
            return "Continue queued saves after startup";
        case UI_OPTION_CONTENT_BLOCKER:
            return "Block common ad and tracking requests";
        case UI_OPTION_COSMETIC_HIDING:
            return "Hide empty ad spaces in page layout";
        case UI_OPTION_COOKIE_BANNERS:
            return "Hide common cookie notices on this site";
        case UI_OPTION_ALLOW_SITE:
            return "Allow this site and reload its content";
        case UI_OPTION_LOAD_ALLOWLIST:
            return "Read more allowed sites from the stick";
        case UI_OPTION_SITE_DATA_ALLOWED:
            return "Allow cookies and web storage globally";
        case UI_OPTION_TLS_SESSION_PERSISTENCE:
            return "Keep short-lived TLS tickets between starts";
        case UI_OPTION_MIXED_CONTENT_SITE:
            return "Compatibility: allow HTTP resources here";
        case UI_OPTION_THIRD_PARTY_COOKIES_SITE:
            return "Compatibility: allow unpartitioned cookies here";
        case UI_OPTION_NETWORK_PROFILE:
            return "Left/right choose; X saves next connection";
        case UI_OPTION_DIAGNOSTIC_QR:
            return "Photograph logs without removing the stick";
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        case UI_OPTION_POWER_TEST:
            return "Run the controlled clocking test";
        case UI_OPTION_MEDIA_TEST:
            return "Run the controlled video test";
#endif
        case UI_OPTION_UPDATE_CHECK:
            return "Look for new releases once a week";
        case UI_OPTION_UPDATE:
            return "Check and install a signed release";
        case UI_OPTION_SITE_DATA:
            return "Manage cache, cookies, and storage";
    }
    return "";
}

static const char *ui_search_engine_name(BrowserSearchEngine engine)
{
    switch (engine) {
        case BROWSER_SEARCH_BING: return "Bing";
        case BROWSER_SEARCH_DUCKDUCKGO: return "DuckDuckGo Lite";
        case BROWSER_SEARCH_GOOGLE:
        default: return "Google";
    }
}

#define UI_CHROME_THEME_CYCLE_COUNT ((unsigned) BROWSER_CHROME_THEME_CUSTOM)

static const char *ui_chrome_theme_name(BrowserChromeTheme theme)
{
    switch (theme) {
        case BROWSER_CHROME_THEME_FINCH: return "Midnight";
        case BROWSER_CHROME_THEME_OCEAN: return "Cobalt";
        case BROWSER_CHROME_THEME_PLUM: return "Slate";
        case BROWSER_CHROME_THEME_EMBER: return "Ember";
        case BROWSER_CHROME_THEME_LIGHT: return "Daylight";
        case BROWSER_CHROME_THEME_CUSTOM:
            return psp_ui_theme_custom_label();
        default: return "Midnight";
    }
}

static unsigned ui_chrome_theme_step(unsigned theme, int direction)
{
    int selected = (int) theme;
    int count = (int) UI_CHROME_THEME_CYCLE_COUNT;
    if (selected < 0 || selected >= count) selected = 0;
    selected = (selected + count + direction) % count;
    return (unsigned) selected;
}

static bool ui_has_overlay(const PspUiState *ui)
{
    return ui != NULL && ui->screen != PSP_UI_SCREEN_PAGE;
}

static void ui_open_overlay(PspUiState *ui, PspUiScreen screen)
{
    if (ui == NULL) return;
    ui->screen = screen;
    ui->overlay_animation_frames = PSP_THEME_MOTION_PANEL_FRAMES;
    ui->overlay_motion = 0u;
    ui->chrome_visible = true;
}

static void ui_open_child_overlay(PspUiState *ui, PspUiScreen screen)
{
    ui_open_overlay(ui, screen);
    if (ui != NULL) ui->overlay_motion = 1u;
}

static void ui_open_parent_overlay(PspUiState *ui, PspUiScreen screen)
{
    ui_open_overlay(ui, screen);
    if (ui != NULL) ui->overlay_motion = 2u;
}

/*
 * Motion is offsets read out of small precomputed tables, never computed per
 * frame and never floating point. Each table is indexed by frames remaining,
 * so the last entry is always zero and the panel lands exactly where the
 * static layout puts it.
 */
static const uint8_t ui_panel_rise[PSP_THEME_MOTION_PANEL_FRAMES + 1] = {
    0, 1, 3, 5, 8, 12, 16, 21, 26
};

/* A 26px side entrance read as a flash on the 480px panel.  Nested screens
 * now travel far enough to make direction unmistakable, while root panels
 * retain the restrained vertical rise above.  Both tables have the same
 * eight-frame, integer-only budget. */
static const uint8_t ui_panel_slide[PSP_THEME_MOTION_PANEL_FRAMES + 1] = {
    0, 2, 6, 12, 21, 32, 45, 59, 72
};

static const uint8_t ui_toast_rise[PSP_THEME_MOTION_TOAST_FRAMES + 1] = {
    0, 1, 3, 5, 8, 11
};

/*
 * The boot entrance's glide, in sixty-fourths of the way there, indexed by
 * frames elapsed since the glide began. Ease-out: the mark leaves the centre
 * quickly and settles into the status line, rather than arriving at speed.
 * The mark's path and the tiles' rise are both read off this one table, so
 * they cannot drift apart.
 */
static const uint8_t ui_entrance_ease[
    PSP_UI_BOOT_ENTRANCE_FRAMES - PSP_UI_BOOT_ENTRANCE_GLIDE_FRAME + 1u] = {
    0, 8, 19, 30, 39, 47, 54, 59, 62, 63, 64
};

static int ui_overlay_slide(const PspUiState *ui)
{
    if (ui == NULL) return 0;
    unsigned frames = ui->overlay_animation_frames;
    if (frames > PSP_THEME_MOTION_PANEL_FRAMES)
        frames = PSP_THEME_MOTION_PANEL_FRAMES;
    return (int) (ui != NULL && ui->overlay_motion != 0u
        ? ui_panel_slide[frames] : ui_panel_rise[frames]);
}

/* Dismiss an overlay to whatever surface is actually underneath it. */
static void ui_close_overlay(PspUiState *ui)
{
    if (ui == NULL) return;
    ui->screen = (PspUiScreen) ui->base_screen;
}

typedef struct {
    int x;
    int y;
    int width;
    int height;
} UiRect;

static void ui_apply_overlay_motion(const PspUiState *ui, UiRect *rect)
{
    if (rect == NULL) return;
    int offset = ui_overlay_slide(ui);
    if (ui != NULL && ui->overlay_motion == 1u)
        rect->x += offset;
    else if (ui != NULL && ui->overlay_motion == 2u)
        rect->x -= offset;
    else
        rect->y += offset;
}

static uint16_t rgb565(unsigned red, unsigned green, unsigned blue)
{
    return tilefinch_rgb565_pack_u8(red, green, blue);
}

typedef struct {
    uint16_t panel;
    uint16_t accent;
    uint16_t text;
    uint16_t muted;
} UiChromePalette;

static UiChromePalette ui_chrome_palette(BrowserChromeTheme theme)
{
    psp_ui_theme_select(theme);
    UiChromePalette palette = {
        .panel = PSP_THEME_PANEL,
        .accent = PSP_THEME_ACCENT_EMBER,
        .text = PSP_THEME_TEXT,
        .muted = PSP_THEME_TEXT_MUTED
    };
    return palette;
}

/*
 * The native surfaces are drawn entirely from the token sheet: they are new
 * chrome, so nothing about them has to stay compatible with the older
 * per-theme panel colors above. Only the accent pair follows the theme.
 */
typedef struct {
    uint16_t accent;
    uint16_t accent_high;
} UiSurfaceAccent;

static UiSurfaceAccent ui_surface_accent(BrowserChromeTheme theme)
{
    psp_ui_theme_select(theme);
    return (UiSurfaceAccent) {
        PSP_THEME_ACCENT_EMBER, PSP_THEME_ACCENT_EMBER_HI };
}

/*
 * Integer alpha-over in RGB565. `foreground_parts` runs 0..8 (eighths):
 * the fill/outline paths speak in quarters and scale up, while glyph
 * antialiasing needs the finer ladder so a faint coverage edge does not
 * jump straight to a quarter-strength fringe on the dark grounds.
 */
#define UI_BLEND_PARTS 8u

static uint16_t blend565(uint16_t foreground, uint16_t background,
                         unsigned foreground_parts)
{
    unsigned inverse = UI_BLEND_PARTS - foreground_parts;
    unsigned fr = tilefinch_rgb565_red_code(foreground);
    unsigned fg = tilefinch_rgb565_green_code(foreground);
    unsigned fb = tilefinch_rgb565_blue_code(foreground);
    unsigned br = tilefinch_rgb565_red_code(background);
    unsigned bg = tilefinch_rgb565_green_code(background);
    unsigned bb = tilefinch_rgb565_blue_code(background);
    return tilefinch_rgb565_pack_codes(
        (fr * foreground_parts + br * inverse) / UI_BLEND_PARTS,
        (fg * foreground_parts + bg * inverse) / UI_BLEND_PARTS,
        (fb * foreground_parts + bb * inverse) / UI_BLEND_PARTS);
}

static void put_pixel(uint16_t *pixels, int width, int height, int stride,
                      int x, int y, uint16_t color)
{
    if (pixels == NULL || x < 0 || y < 0 || x >= width || y >= height) return;
    pixels[(size_t) y * (size_t) stride + (size_t) x] = color;
}

static void fill_rect(uint16_t *pixels, int width, int height, int stride,
                      UiRect rect, uint16_t color, unsigned opacity)
{
    int left = rect.x < 0 ? 0 : rect.x;
    int top = rect.y < 0 ? 0 : rect.y;
    int right = rect.x + rect.width;
    int bottom = rect.y + rect.height;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (left >= right || top >= bottom) return;
    /* RGB565 half-black is exactly each channel divided by two. Preserve
       blend565()'s floor semantics with a channel-safe mask, while avoiding
       three extracts, multiplies, divisions, and a repack for every shadow
       pixel. Toasts and panel shells use this path heavily. */
    if (color == 0u && opacity == 2u) {
        for (int y = top; y < bottom; y++) {
            uint16_t *row = pixels + (size_t) y * (size_t) stride;
            for (int x = left; x < right; x++)
                row[x] = (uint16_t) ((row[x] & 0xf7deu) >> 1u);
        }
        return;
    }
    for (int y = top; y < bottom; y++) {
        uint16_t *row = pixels + (size_t) y * (size_t) stride;
        for (int x = left; x < right; x++) {
            row[x] = opacity >= 4u
                ? color : blend565(color, row[x], opacity * 2u);
        }
    }
}

static void outline_rect(uint16_t *pixels, int width, int height, int stride,
                         UiRect rect, uint16_t color, int thickness)
{
    if (rect.width <= 0 || rect.height <= 0 || thickness <= 0) return;
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x, rect.y, rect.width, thickness }, color, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x, rect.y + rect.height - thickness,
                         rect.width, thickness }, color, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x, rect.y, thickness, rect.height }, color, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x + rect.width - thickness, rect.y,
                         thickness, rect.height }, color, 4);
}

static void fill_round_rect(uint16_t *pixels, int width, int height, int stride,
                            UiRect rect, int radius, uint16_t color,
                            unsigned opacity)
{
    if (radius < 1) {
        fill_rect(pixels, width, height, stride, rect, color, opacity);
        return;
    }
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x + radius, rect.y,
                         rect.width - radius * 2, rect.height },
              color, opacity);
    fill_rect(pixels, width, height, stride,
              (UiRect) { rect.x, rect.y + radius,
                         rect.width, rect.height - radius * 2 },
              color, opacity);
    int radius_squared = radius * radius;
    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            int dx = radius - x - 1;
            int dy = radius - y - 1;
            if (dx * dx + dy * dy > radius_squared) continue;
            int points[4][2] = {
                { rect.x + x, rect.y + y },
                { rect.x + rect.width - x - 1, rect.y + y },
                { rect.x + x, rect.y + rect.height - y - 1 },
                { rect.x + rect.width - x - 1,
                  rect.y + rect.height - y - 1 }
            };
            for (size_t at = 0; at < 4; at++) {
                int px = points[at][0], py = points[at][1];
                if (px < 0 || py < 0 || px >= width || py >= height) continue;
                uint16_t *target =
                    &pixels[(size_t) py * (size_t) stride + (size_t) px];
                *target = opacity >= 4u
                    ? color : blend565(color, *target, opacity * 2u);
            }
        }
    }
}

/*
 * How far row `y` of a `radius`-rounded `bounds` is inset from its own
 * straight edge. Uses the same corner test fill_round_rect uses, so an
 * accent drawn through it lands exactly on the rounded silhouette instead
 * of standing proud of the corner.
 */
static int round_rect_inset(UiRect bounds, int radius, int y)
{
    if (radius < 1) return 0;
    int dy;
    if (y < bounds.y + radius) {
        dy = radius - (y - bounds.y) - 1;
    } else if (y >= bounds.y + bounds.height - radius) {
        dy = radius - (bounds.y + bounds.height - y);
    } else {
        return 0;
    }
    if (dy < 0) dy = 0;
    for (int inset = 0; inset < radius; inset++) {
        int dx = radius - inset - 1;
        if (dx * dx + dy * dy <= radius * radius) return inset;
    }
    return radius;
}

/*
 * A vertical accent bar hugging the left edge of a rounded surface: the
 * bar's own top and bottom follow the surface's corners.
 */
static void fill_round_edge_bar(
    uint16_t *pixels, int width, int height, int stride,
    UiRect bounds, int radius, int thickness, uint16_t color)
{
    for (int y = bounds.y; y < bounds.y + bounds.height; y++) {
        int inset = round_rect_inset(bounds, radius, y);
        if (inset >= thickness) continue;
        fill_rect(pixels, width, height, stride,
                  (UiRect) {bounds.x + inset, y, thickness - inset, 1},
                  color, 4);
    }
}

/*
 * Overlay furniture, shared by every panel so the family reads as one.
 *
 * Geometry is deliberately taken from the caller unchanged -- these helpers
 * recolor what each panel already drew (its shadow, its ground, its rule,
 * and the band its legend sits on) rather than re-laying anything out.
 */
static void draw_panel_shell(uint16_t *pixels, int width, int height,
                             int stride, UiRect box)
{
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) {box.x + 4, box.y + 5, box.width, box.height},
                    PSP_THEME_RADIUS_PANEL, rgb565(0, 0, 0), 2);
    fill_round_rect(pixels, width, height, stride, box,
                    PSP_THEME_RADIUS_PANEL, PSP_THEME_PANEL, 4);
}

/* The separator under a panel title. */
static void draw_panel_rule(uint16_t *pixels, int width, int height,
                            int stride, UiRect box, int y)
{
    fill_rect(pixels, width, height, stride,
              (UiRect) {box.x, y, box.width, 1}, PSP_THEME_LINE, 4);
}

/*
 * The hint row a panel's legend sits on: a HINT_BAR band running to the
 * panel's bottom edge, its corners following the panel's own silhouette.
 * `text_top` is the y the legend already draws at, so the band is sized to
 * the text instead of the text being moved to fit a band.
 */
static void draw_panel_hint_bar(uint16_t *pixels, int width, int height,
                                int stride, UiRect box, int text_top)
{
    int top = text_top - 3;
    if (top < box.y) top = box.y;
    for (int y = top; y < box.y + box.height; y++) {
        int inset = round_rect_inset(box, PSP_THEME_RADIUS_PANEL, y);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {box.x + inset, y, box.width - 2 * inset, 1},
                  PSP_THEME_HINT_BAR, 4);
    }
}

static int triangle_edge(
    int ax, int ay, int bx, int by, int px, int py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void fill_triangle(
    uint16_t *pixels, int width, int height, int stride,
    int ax, int ay, int bx, int by, int cx, int cy, uint16_t color)
{
    int left = ax < bx ? ax : bx;
    if (cx < left) left = cx;
    int right = ax > bx ? ax : bx;
    if (cx > right) right = cx;
    int top = ay < by ? ay : by;
    if (cy < top) top = cy;
    int bottom = ay > by ? ay : by;
    if (cy > bottom) bottom = cy;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= width) right = width - 1;
    if (bottom >= height) bottom = height - 1;
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            int first = triangle_edge(ax, ay, bx, by, x, y);
            int second = triangle_edge(bx, by, cx, cy, x, y);
            int third = triangle_edge(cx, cy, ax, ay, x, y);
            bool negative = first < 0 || second < 0 || third < 0;
            bool positive = first > 0 || second > 0 || third > 0;
            if (negative && positive) continue;
            pixels[(size_t) y * (size_t) stride + (size_t) x] = color;
        }
    }
}

/* Compact 5x7 uppercase font. Each byte is one five-bit row. */
static const uint8_t *glyph_rows(char character)
{
    static const uint8_t blank[7] = {0};
    static const uint8_t unknown[7] = {31, 17, 2, 4, 4, 0, 4};
    static const uint8_t glyphs[][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14},      {7,2,2,2,2,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31},     {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},     {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };
    static const uint8_t period[7] = {0,0,0,0,0,6,6};
    static const uint8_t comma[7] = {0,0,0,0,0,6,4};
    static const uint8_t colon[7] = {0,6,6,0,6,6,0};
    static const uint8_t slash[7] = {1,2,2,4,8,8,16};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t plus[7] = {0,4,4,31,4,4,0};
    static const uint8_t percent[7] = {17,2,4,8,17,0,0};
    static const uint8_t open[7] = {2,4,8,8,8,4,2};
    static const uint8_t close[7] = {8,4,2,2,2,4,8};
    static const uint8_t exclaim[7] = {4,4,4,4,4,0,4};
    static const uint8_t underscore[7] = {0,0,0,0,0,0,31};
    static const uint8_t ampersand[7] = {12,18,20,8,21,18,13};
    static const uint8_t apostrophe[7] = {4,4,2,0,0,0,0};
    static const uint8_t quote[7] = {10,10,5,0,0,0,0};
    unsigned char value = (unsigned char) character;
    value = (unsigned char) toupper(value);
    if (value >= 'A' && value <= 'Z') return glyphs[value - 'A'];
    if (value >= '0' && value <= '9') return digits[value - '0'];
    switch (value) {
        case ' ': return blank;
        case '.': return period;
        case ',': return comma;
        case ':': return colon;
        case '/': return slash;
        case '-': return dash;
        case '+': return plus;
        case '%': return percent;
        case '(': return open;
        case ')': return close;
        case '!': return exclaim;
        case '_': return underscore;
        case '&': return ampersand;
        case '\'': return apostrophe;
        case '"': return quote;
        default: return unknown;
    }
}

static void draw_character(uint16_t *pixels, int width, int height, int stride,
                           int x, int y, char character, uint16_t color,
                           int scale)
{
    const uint8_t *rows = glyph_rows(character);
    for (int row = 0; row < 7; row++) {
        for (int column = 0; column < 5; column++) {
            if ((rows[row] & (1u << (4 - column))) == 0) continue;
            fill_rect(pixels, width, height, stride,
                      (UiRect) { x + column * scale, y + row * scale,
                                 scale, scale }, color, 4);
        }
    }
}

static void draw_unicode_fallback(uint16_t *pixels, int width, int height,
                                  int stride, int x, int y, uint16_t color,
                                  int scale)
{
    outline_rect(pixels, width, height, stride,
                 (UiRect) {x, y, 5 * scale, 7 * scale}, color, scale);
}

static void draw_font_glyph(uint16_t *pixels, int width, int height,
                            int stride, int x, int baseline,
                            const FontGlyph *glyph, uint16_t color)
{
    if (glyph == NULL || glyph->pixels == NULL) return;
    int left = x + glyph->x_offset;
    int top = baseline + glyph->y_offset;
    for (int row = 0; row < glyph->height; row++) {
        int destination_y = top + row;
        if (destination_y < 0 || destination_y >= height) continue;
        for (int column = 0; column < glyph->width; column++) {
            unsigned coverage =
                glyph->pixels[(size_t) row * (size_t) glyph->width
                              + (size_t) column];
            if (coverage == 0) continue;
            int destination_x = left + column;
            if (destination_x < 0 || destination_x >= width) continue;
            uint16_t *destination =
                &pixels[(size_t) destination_y * (size_t) stride
                        + (size_t) destination_x];
            /*
             * Round coverage to the nearest eighth. The old ceiling
             * (`(coverage + 63) / 64`) promoted a 2%-coverage edge pixel
             * to a full quarter blend, so light-on-dark 11px text grew a
             * wide gray fringe; rounding keeps near-zero coverage near
             * zero and halves the visible step between fringe levels.
             */
            unsigned parts = (coverage + 16u) / 32u;
            if (parts > UI_BLEND_PARTS) parts = UI_BLEND_PARTS;
            if (parts == 0u) continue;
            *destination = parts == UI_BLEND_PARTS
                ? color : blend565(color, *destination, parts);
        }
    }
}

void psp_ui_clear_chrome_font(void)
{
    for (size_t weight = 0; weight < 2u; weight++) {
        for (size_t size = 0; size < 2u; size++) {
            for (size_t at = 0; at < UI_CHROME_GLYPH_COUNT; at++) {
                if (chrome_font_cache.loaded[weight][size][at] == 1u)
                    font_glyph_destroy(
                        chrome_font_cache.faces[weight],
                        &chrome_font_cache.glyphs[weight][size][at]);
            }
        }
    }
    for (size_t at = 0; at < UI_CHROME_UNICODE_GLYPH_LIMIT; at++) {
        UiChromeUnicodeGlyph *entry = &chrome_font_cache.unicode[at];
        if (entry->loaded == 1u && entry->weight < 2u)
            font_glyph_destroy(
                chrome_font_cache.faces[entry->weight], &entry->glyph);
    }
    memset(&chrome_font_cache, 0, sizeof(chrome_font_cache));
}

void psp_ui_set_chrome_fonts(
    const FontFace *regular, const FontFace *bold)
{
    if (chrome_font_cache.faces[0] == regular
        && chrome_font_cache.faces[1] == bold) return;
    psp_ui_clear_chrome_font();
    chrome_font_cache.faces[0] = regular;
    chrome_font_cache.faces[1] = bold != NULL ? bold : regular;
}

void psp_ui_set_device_status(
    unsigned hour, unsigned minute, int battery_percent, bool charging,
    bool twelve_hour, int wifi_bars)
{
    if (hour >= 24u || minute >= 60u || battery_percent < 0) {
        memset(&device_status, 0, sizeof(device_status));
        return;
    }
    /*
     * Some packs report over 100% transiently while charging. That is a
     * reading to clamp, not a reason to blank the clock and the battery
     * along with it.
     */
    if (battery_percent > 100) battery_percent = 100;
    device_status.hour = (uint8_t) hour;
    device_status.minute = (uint8_t) minute;
    device_status.battery_percent = (uint8_t) battery_percent;
    device_status.charging = charging;
    device_status.twelve_hour = twelve_hour;
    device_status.valid = true;
    ui_format_clock(device_status.clock, sizeof(device_status.clock));
    /* A negative count means the link is not READY (or this is not a
       live-network build): show no bars at all rather than an empty
       cluster. Otherwise clamp into the drawable 0..MAX range. */
    if (wifi_bars < 0) {
        device_status.wifi_valid = false;
        device_status.wifi_bars = 0;
    } else {
        device_status.wifi_valid = true;
        device_status.wifi_bars =
            (uint8_t) (wifi_bars > (int) UI_WIFI_BARS_MAX
                           ? UI_WIFI_BARS_MAX : (unsigned) wifi_bars);
    }
}

unsigned psp_ui_ratio_extent_u64(
    uint64_t value, uint64_t total, unsigned extent)
{
    if (total == 0 || extent == 0) return 0;
    if (value >= total) return extent;
    uint64_t maximum = UINT32_MAX / extent;
    unsigned shift = 0;
    while ((total >> shift) > maximum && shift < 32u) shift++;
    uint32_t scaled_total = (uint32_t) (total >> shift);
    uint32_t scaled_value = (uint32_t) (value >> shift);
    if (scaled_total == 0) return 0;
    return scaled_value * extent / scaled_total;
}

/*
 * Formats the cached wall clock into `out` honouring the PSP's
 * PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT preference: "13:42" in 24-hour mode,
 * "1:42 PM" in 12-hour mode (no leading zero on the hour, as the XMB
 * renders it). Needs 9 bytes.
 */
static void ui_format_clock(char *out, size_t size)
{
    unsigned hour = device_status.hour;
    if (!device_status.twelve_hour) {
        snprintf(out, size, "%u:%02u", hour,
                 (unsigned) device_status.minute);
        return;
    }
    const char *suffix = hour < 12u ? "AM" : "PM";
    unsigned display = hour % 12u;
    if (display == 0u) display = 12u;
    snprintf(out, size, "%u:%02u %s", display,
             (unsigned) device_status.minute, suffix);
}

/*
 * Load one chrome glyph, reporting 1 for a hit and 2 for a miss.
 *
 * The coverage question has to be asked separately: font_glyph_load()
 * succeeds for a codepoint the face does not have, because .notdef is a
 * legal glyph. The face the device ships, fonts/DejaVuSans-Latin.ttf, has an
 * empty .notdef, so a missing character came back as a blank cell with a real
 * advance; its bold sibling's .notdef is a box, so the same character came
 * back as tofu. Either way the load "succeeded", `known_missing` stayed false
 * for every caller, and the substitutions below in draw_text_with_font --
 * U+2212 to '-', an unmapped codepoint to the fallback tile -- became
 * unreachable the moment a chrome face was set. Out of line so the
 * composite's instruction-cache ratchet does not pay for it.
 */
static __attribute__((noinline)) uint8_t chrome_glyph_load(
    size_t weight, unsigned codepoint, int pixel_height, FontGlyph *glyph)
{
    const FontFace *face = chrome_font_cache.faces[weight];
    if (!font_face_has_codepoint(face, codepoint)) return 2u;
    return font_glyph_load(face, codepoint, pixel_height, false, glyph)
        ? 1u : 2u;
}

static const FontGlyph *chrome_font_glyph(
    unsigned codepoint, int scale, bool bold, bool *known_missing)
{
    if (known_missing != NULL) *known_missing = false;
    size_t weight = bold ? 1u : 0u;
    if (chrome_font_cache.faces[weight] == NULL) return NULL;
    size_t size = scale >= 2 ? 1u : 0u;
    if (codepoint < UI_CHROME_GLYPH_FIRST
        || codepoint > UI_CHROME_GLYPH_LAST) {
        UiChromeUnicodeGlyph *victim = NULL;
        for (size_t at = 0; at < UI_CHROME_UNICODE_GLYPH_LIMIT; at++) {
            UiChromeUnicodeGlyph *entry = &chrome_font_cache.unicode[at];
            if (entry->loaded != 0u && entry->codepoint == codepoint
                && entry->weight == weight && entry->size == size) {
                entry->age = ++chrome_font_cache.unicode_clock;
                if (entry->loaded != 1u) {
                    if (known_missing != NULL) *known_missing = true;
                    return NULL;
                }
                return &entry->glyph;
            }
            if (victim == NULL || entry->loaded == 0u
                || (victim->loaded != 0u && entry->age < victim->age)) {
                victim = entry;
            }
        }
        if (victim == NULL) return NULL;
        if (victim->loaded == 1u)
            font_glyph_destroy(
                chrome_font_cache.faces[victim->weight], &victim->glyph);
        memset(victim, 0, sizeof(*victim));
        victim->codepoint = codepoint;
        victim->weight = (uint8_t) weight;
        victim->size = (uint8_t) size;
        victim->age = ++chrome_font_cache.unicode_clock;
        int pixel_height = size == 0u ? 11 : 15;
        victim->loaded = chrome_glyph_load(
            weight, codepoint, pixel_height, &victim->glyph);
        if (victim->loaded != 1u) {
            if (known_missing != NULL) *known_missing = true;
            return NULL;
        }
        return &victim->glyph;
    }
    size_t at = (size_t) (codepoint - UI_CHROME_GLYPH_FIRST);
    if (chrome_font_cache.loaded[weight][size][at] == 0u) {
        /* Compact chrome is viewed on a physical 4.3-inch panel. One extra
           pixel materially improves it without changing panel geometry. */
        int pixel_height = size == 0u ? 11 : 15;
        chrome_font_cache.loaded[weight][size][at] = chrome_glyph_load(
            weight, codepoint, pixel_height,
            &chrome_font_cache.glyphs[weight][size][at]);
    }
    if (chrome_font_cache.loaded[weight][size][at] != 1u) {
        if (known_missing != NULL) *known_missing = true;
        return NULL;
    }
    return &chrome_font_cache.glyphs[weight][size][at];
}

static size_t utf8_character_count(const char *text, size_t bytes)
{
    /* Chrome labels are overwhelmingly ASCII. Preserve the decoder's exact
       malformed-UTF-8 behavior for every non-ASCII string, but avoid walking
       the font decoder for the ordinary one-byte case. */
    bool ascii = true;
    for (size_t at = 0; at < bytes; at++) {
        if ((unsigned char) text[at] >= 0x80u) {
            ascii = false;
            break;
        }
    }
    if (ascii) return bytes;
    size_t characters = 0;
    for (size_t at = 0; at < bytes;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, bytes - at, &codepoint);
        if (used == 0) break;
        at += used;
        characters++;
    }
    return characters;
}

/*
 * Truncation ellipsis metrics. The dots belong to whichever face is
 * drawing the run: builtin dots advanced a flat 6 * scale span ~36px at
 * scale 2 and read as three separate periods, and on the scale-1 grid
 * they sit above the proportional face's baseline, so an ellipsis floated
 * mid-height beside a real period. Returns the per-dot advance and hands
 * back the glyph when the chrome face can serve it.
 */
static int ui_ellipsis_advance(int scale, bool bold, const FontGlyph **dot)
{
    bool missing = false;
    const FontGlyph *glyph = chrome_font_glyph('.', scale, bold, &missing);
    if (dot != NULL) *dot = glyph;
    if (glyph != NULL && glyph->advance > 0) return glyph->advance;
    return 6 * scale;
}

static int draw_text_with_font(
    uint16_t *pixels, int width, int height, int stride,
    int x, int y, const char *text, size_t maximum_characters,
    int maximum_x, uint16_t color, int scale, const FontFace *font,
    bool chrome_bold)
{
    if (text == NULL) return x;
    const FontGlyph *dot = NULL;
    int dot_advance = ui_ellipsis_advance(scale, chrome_bold, &dot);
    int ellipsis_reserve = 3 * dot_advance;
    size_t bytes = strlen(text);
    size_t characters = utf8_character_count(text, bytes);
    bool ellipsis = characters > maximum_characters;
    size_t visible = characters < maximum_characters
        ? characters : maximum_characters;
    if (ellipsis && visible > 3u) visible -= 3u;
    size_t byte_at = 0;
    for (size_t at = 0; at < visible && byte_at < bytes; at++) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(
            text + byte_at, bytes - byte_at, &codepoint);
        if (used == 0) break;
        byte_at += used;
        FontGlyph transient_glyph = {0};
        const FontGlyph *glyph = NULL;
        /* Always prefer the retained chrome cache for ASCII and any Unicode
           glyph it can serve. A preferred face is a fallback, not a reason
           to bypass the cache: doing so made ASCII media titles such as
           "Retrying at 240p" fall back to the blocky boot bitmap. */
        glyph = chrome_font_glyph(
            codepoint, scale, chrome_bold, NULL);
        bool transient_loaded = glyph == NULL && font != NULL
            && font_face_has_codepoint(font, codepoint)
            && font_glyph_load(
                font, codepoint, 7 * scale, false, &transient_glyph);
        if (transient_loaded) glyph = &transient_glyph;
        int advance = glyph != NULL ? glyph->advance : 6 * scale;
        if (advance < 0) advance = 0;
        bool more = at + 1u < visible || byte_at < bytes;
        int limit = maximum_x - (more ? ellipsis_reserve : 0);
        if (x > limit - advance) {
            ellipsis = true;
            font_glyph_destroy(font, &transient_glyph);
            break;
        }
        if (glyph != NULL) {
            int baseline = y + (scale >= 2 ? 13 : 10);
            draw_font_glyph(pixels, width, height, stride, x,
                            baseline, glyph, color);
        } else if (codepoint <= 0x7fu) {
            draw_character(pixels, width, height, stride, x, y,
                           (char) codepoint, color, scale);
        } else {
            /*
             * Browser chrome deliberately uses the tiny built-in face so it
             * remains available before page fonts are loaded. Map common
             * Unicode punctuation to its readable ASCII equivalent instead
             * of drawing the generic '?' tile. Page titles commonly contain
             * U+2013 between the article and site name.
             */
            char fallback = '\0';
            switch (codepoint) {
                case 0x00a0: fallback = ' '; break;
                case 0x2010:
                case 0x2011:
                case 0x2012:
                case 0x2013:
                case 0x2014:
                case 0x2212: fallback = '-'; break;
                case 0x2018:
                case 0x2019: fallback = '\''; break;
                case 0x201c:
                case 0x201d: fallback = '"'; break;
                case 0x2022:
                case 0x00b7: fallback = '.'; break;
                default: break;
            }
            if (fallback != '\0') {
                draw_character(pixels, width, height, stride, x, y,
                               fallback, color, scale);
            } else if (codepoint == 0x2026) {
                draw_character(pixels, width, height, stride, x, y,
                               '.', color, scale);
            } else {
                draw_unicode_fallback(pixels, width, height, stride,
                                      x, y, color, scale);
            }
        }
        font_glyph_destroy(font, &transient_glyph);
        x += advance;
    }
    if (ellipsis) {
        for (int at = 0; at < 3 && x <= maximum_x - dot_advance; at++) {
            if (dot != NULL) {
                draw_font_glyph(pixels, width, height, stride, x,
                                y + (scale >= 2 ? 13 : 10), dot, color);
            } else {
                draw_character(pixels, width, height, stride, x, y, '.',
                               color, scale);
            }
            x += dot_advance;
        }
    }
    return x;
}

static int draw_text(uint16_t *pixels, int width, int height, int stride,
                     int x, int y, const char *text, size_t maximum_characters,
                     uint16_t color, int scale)
{
    return draw_text_with_font(
        pixels, width, height, stride, x, y, text, maximum_characters,
        0x3fffffff, color, scale, NULL, false);
}

static int draw_text_bold(
    uint16_t *pixels, int width, int height, int stride,
    int x, int y, const char *text, size_t maximum_characters,
    uint16_t color, int scale)
{
    return draw_text_with_font(
        pixels, width, height, stride, x, y, text, maximum_characters,
        0x3fffffff, color, scale, NULL, true);
}

static int chrome_text_width_bytes(
    const char *text, size_t maximum_bytes, int scale, bool bold)
{
    if (text == NULL) return 0;
    size_t bytes = strlen(text);
    if (bytes > maximum_bytes) bytes = maximum_bytes;
    int width = 0;
    for (size_t at = 0; at < bytes;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, bytes - at, &codepoint);
        if (used == 0 || at + used > bytes) break;
        at += used;
        bool known_missing = false;
        const FontGlyph *glyph = chrome_font_glyph(
            codepoint, scale, bold, &known_missing);
        int advance = glyph != NULL ? glyph->advance : 6 * scale;
        if (advance > 0) width += advance;
    }
    return width;
}

static int draw_text_right_aligned(
    uint16_t *pixels, int width, int height, int stride,
    int right, int y, const char *text, size_t maximum_characters,
    uint16_t color, int scale, bool bold)
{
    size_t bytes = text == NULL ? 0u : strlen(text);
    int text_width = chrome_text_width_bytes(text, bytes, scale, bold);
    int x = right - text_width;
    if (x < 0) x = 0;
    /* The run is already measured to end on `right`, so cancel out the
       helper's ellipsis reserve rather than truncating a string that fits. */
    return draw_text_with_font(
        pixels, width, height, stride, x, y, text, maximum_characters,
        right + 3 * ui_ellipsis_advance(scale, bold, NULL),
        color, scale, NULL, bold);
}

static int browser_ui_scale(const PspUiState *ui)
{
    return ui != NULL && ui->browser_ui_scale >= 2u ? 2 : 1;
}

static int browser_bottom_height(const PspUiState *ui)
{
    return browser_ui_scale(ui) == 2 ? 29 : UI_BOTTOM_HEIGHT;
}

void psp_ui_opaque_chrome_rows(
    const PspUiState *ui, unsigned *top_rows, unsigned *bottom_rows)
{
    unsigned top = 0u, bottom = 0u;
    /* Diagnostic QR owns its full surface and bypasses browser chrome. Native
       HOME/Collections likewise do not use the page base-copy path. */
    if (ui != NULL && ui->chrome_visible
        && ui->screen != PSP_UI_SCREEN_DIAGNOSTIC_QR
        && !psp_ui_screen_is_native_surface(ui->screen)) {
        top = UI_TOP_HEIGHT;
        if (ui->screen == PSP_UI_SCREEN_PAGE)
            bottom = (unsigned) browser_bottom_height(ui);
    }
    if (top_rows != NULL) *top_rows = top;
    if (bottom_rows != NULL) *bottom_rows = bottom;
}

static void draw_chevron(uint16_t *pixels, int width, int height, int stride,
                         int center_x, int center_y, int direction,
                         uint16_t color)
{
    for (int at = -4; at <= 4; at++) {
        int distance = at < 0 ? -at : at;
        int x = center_x + direction * (2 - distance / 2);
        put_pixel(pixels, width, height, stride, x, center_y + at, color);
        put_pixel(pixels, width, height, stride, x + direction,
                  center_y + at, color);
    }
}

static void draw_vertical_chevron(
    uint16_t *pixels, int width, int height, int stride,
    int center_x, int center_y, int direction, uint16_t color, int half)
{
    for (int at = -half; at <= half; at++) {
        int distance = at < 0 ? -at : at;
        int y = center_y + direction * (half / 2 - distance / 2);
        put_pixel(pixels, width, height, stride, center_x + at, y, color);
        put_pixel(pixels, width, height, stride, center_x + at,
                  y + direction, color);
    }
}

static bool ui_content_blocker_site_available(const PspUiState *ui)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    return ui != NULL && content_blocker_site_from_url(ui->url, site);
}

static void draw_lock(uint16_t *pixels, int width, int height, int stride,
                      int x, int y, bool secure, uint16_t color)
{
    if (!secure) {
        outline_rect(pixels, width, height, stride,
                     (UiRect) { x, y + 4, 8, 7 }, color, 1);
        draw_text(pixels, width, height, stride, x + 2, y + 4, "!", 1,
                  color, 1);
        return;
    }
    outline_rect(pixels, width, height, stride,
                 (UiRect) { x, y + 4, 8, 7 }, color, 1);
    for (int at = 0; at < 4; at++) {
        put_pixel(pixels, width, height, stride, x + 2 + at, y + 1, color);
    }
    put_pixel(pixels, width, height, stride, x + 1, y + 2, color);
    put_pixel(pixels, width, height, stride, x + 6, y + 2, color);
    put_pixel(pixels, width, height, stride, x + 1, y + 3, color);
    put_pixel(pixels, width, height, stride, x + 6, y + 3, color);
}

static void copy_string(char *destination, size_t capacity,
                        const char *source);

static void ui_url_presentation(
    const PspUiState *ui, char *domain, size_t domain_capacity,
    char *detail, size_t detail_capacity)
{
    if (domain == NULL || domain_capacity == 0
        || detail == NULL || detail_capacity == 0) return;
    domain[0] = '\0';
    detail[0] = '\0';
    const char *url = ui == NULL ? NULL : ui->url;
    if (url == NULL || url[0] == '\0') {
        copy_string(domain, domain_capacity, ui == NULL ? "" : ui->title);
        return;
    }
    const char *at = url;
    if (strncmp(at, "https://", 8u) == 0) at += 8;
    else if (strncmp(at, "http://", 7u) == 0) at += 7;
    else if (strncmp(at, "tilefinch://", 12u) == 0) {
        copy_string(domain, domain_capacity, "Tilefinch");
        if (strcmp(url, "tilefinch://home") == 0
            || strcmp(url, "tilefinch://home/") == 0)
            copy_string(detail, detail_capacity, "Home");
        else
            copy_string(detail, detail_capacity,
                        ui->title[0] == '\0' ? "Page" : ui->title);
        return;
    }
    const char *end = at;
    while (*end != '\0' && *end != '/' && *end != '?' && *end != '#')
        end++;
    size_t host_length = (size_t) (end - at);
    if (host_length >= domain_capacity) host_length = domain_capacity - 1u;
    memcpy(domain, at, host_length);
    domain[host_length] = '\0';
    if (*end != '\0') copy_string(detail, detail_capacity, end);
    else if (ui != NULL && ui->title[0] != '\0')
        copy_string(detail, detail_capacity, ui->title);
}

static void copy_string(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0) return;
    snprintf(destination, capacity, "%s", source == NULL ? "" : source);
}

PspUiToolbarGestureResult psp_ui_toolbar_gesture_update(
    PspUiToolbarGesture *gesture, bool held, bool page_eligible,
    unsigned elapsed_ms)
{
    if (gesture == NULL) return PSP_UI_TOOLBAR_GESTURE_NONE;
    PspUiToolbarGestureResult result = PSP_UI_TOOLBAR_GESTURE_NONE;
    if (held && !gesture->held) {
        /* elapsed_ms precedes this first observation: charging it can turn
           a tap after a slow layout into an immediate Reader hold. */
        gesture->hold_ms = 0;
        gesture->triggered = false;
        gesture->started_on_page = page_eligible;
    }
    else if (held && gesture->started_on_page) {
        unsigned remaining = 700u - gesture->hold_ms;
        gesture->hold_ms += elapsed_ms < remaining ? elapsed_ms : remaining;
        if (gesture->hold_ms == 700u && !gesture->triggered) {
            gesture->triggered = true;
            result = PSP_UI_TOOLBAR_GESTURE_READER;
        }
    } else if (!held && gesture->held) {
        if (gesture->started_on_page && !gesture->triggered)
            result = PSP_UI_TOOLBAR_GESTURE_TAP;
        gesture->hold_ms = 0;
        gesture->triggered = false;
        gesture->started_on_page = false;
    }
    gesture->held = held;
    return result;
}

bool psp_ui_filter_toolbar_input(PspUiToolbarInputState *state, PspUiInput *input,
                                bool page_eligible, uint64_t now_ms)
{
    if (state == NULL || input == NULL) return false;
    uint64_t delta = now_ms >= state->sample_ms ? now_ms - state->sample_ms : 0;
    state->sample_ms = now_ms;
    bool held = (input->held & PSP_UI_BUTTON_TOOLBAR) != 0;
    PspUiToolbarGestureResult result = psp_ui_toolbar_gesture_update(
        &state->gesture, held, page_eligible,
        (unsigned) (delta > 700u ? 700u : delta));
    if (held && state->gesture.started_on_page)
        input->pressed &= ~PSP_UI_BUTTON_TOOLBAR;
    else if (result == PSP_UI_TOOLBAR_GESTURE_TAP)
        input->pressed |= PSP_UI_BUTTON_TOOLBAR;
    return result == PSP_UI_TOOLBAR_GESTURE_READER;
}

void psp_ui_init(PspUiState *ui)
{
    if (ui == NULL) return;
    memset(ui, 0, sizeof(*ui));
    ui->chrome_visible = true;
    ui->progress_per_mille = -1;
    ui->update_progress_per_mille = -1;
    ui->activity_frames = UI_AUTOHIDE_FRAMES;
    ui->browser_ui_scale = 1;
    ui->page_font_percent = 100;
    ui->analog_cursor_enabled = true;
    ui->javascript_enabled = 1u;
    ui->site_javascript_enabled = 1u;
    ui->site_data_allowed = 1u;
    ui->content_blocker_mode = CONTENT_BLOCKER_BASIC;
    ui->content_blocker_cosmetic_hiding = true;
    ui->cookie_banner_hidden = true;
    ui->update_check_enabled = true;
    /* Frontends replace this from the profile before the menu is shown. */
    ui->live_cache_kib = 512u;
    ui->cursor_x_milli = 240000;
    ui->cursor_y_milli = 136000;
    copy_string(ui->title, sizeof(ui->title), "TILEFINCH");
    copy_string(ui->status, sizeof(ui->status),
                "START SEARCH/URL   SELECT MENU");
    copy_string(
        ui->update_status, sizeof(ui->update_status),
        "PRESS X TO CHECK FOR UPDATES");
    copy_string(
        ui->update_primary_label, sizeof(ui->update_primary_label),
        "CHECK NOW");
}

void psp_ui_set_page(PspUiState *ui, const char *title, const char *url,
                     bool secure)
{
    if (ui == NULL) return;
    copy_string(ui->title, sizeof(ui->title),
                title == NULL || title[0] == '\0' ? "UNTITLED PAGE" : title);
    copy_string(ui->url, sizeof(ui->url), url);
    ui->secure = secure;
}

void psp_ui_set_network_profile(
    PspUiState *ui, unsigned profile, const char *ssid)
{
    if (ui == NULL) return;
    if (profile < 1u || profile > 100u) profile = 1u;
    ui->network_profile = profile;
    if (ssid != NULL && ssid[0] != '\0') {
        /* The value column is deliberately compact. Retain the profile
           number for support reports even when a long SSID is clipped. */
        snprintf(ui->network_profile_label,
                 sizeof(ui->network_profile_label),
                 "%.14s (P%u)", ssid, profile);
    } else {
        snprintf(ui->network_profile_label,
                 sizeof(ui->network_profile_label),
                 "Profile %u", profile);
    }
    ui->network_profile_label_valid = true;
}

void psp_ui_set_navigation_target(PspUiState *ui, const char *url)
{
    if (ui == NULL || url == NULL || url[0] == '\0') return;
    copy_string(ui->title, sizeof(ui->title), "Opening page");
    copy_string(ui->url, sizeof(ui->url), url);
    ui->secure = strncmp(url, "https://", 8u) == 0;
    /* This is a new accepted destination, not a progress notification for
       the incumbent load. Announce it even when that load was still active. */
    ui->loading = false;
    psp_ui_set_loading(ui, true, -1);
}

void psp_ui_set_history(PspUiState *ui, bool can_go_back,
                        bool can_go_forward)
{
    if (ui == NULL) return;
    ui->can_go_back = can_go_back;
    ui->can_go_forward = can_go_forward;
}

void psp_ui_set_loading(PspUiState *ui, bool loading,
                        int progress_per_mille)
{
    if (ui == NULL) return;
    bool started = loading && !ui->loading;
    if (progress_per_mille < -1) progress_per_mille = -1;
    if (progress_per_mille > 1000) progress_per_mille = 1000;
    if (loading && ui->loading
        && progress_per_mille >= 0 && ui->progress_per_mille >= 0
        && progress_per_mille < ui->progress_per_mille) {
        progress_per_mille = ui->progress_per_mille;
    }
    ui->loading = loading;
    ui->progress_per_mille = progress_per_mille;
    if (loading) {
        /* Announce a new load, but respect Triangle during its continuation. */
        if (started) ui->chrome_visible = true;
        ui->activity_frames = UI_AUTOHIDE_FRAMES;
    }
}

bool psp_ui_set_page_activation(PspUiState *ui, bool active)
{
    if (ui == NULL) return false;
    active = active && ui->screen == PSP_UI_SCREEN_PAGE
        && !ui->page_gamepad_capture;
    bool changed = ui->page_activation_busy != (unsigned) active;
    ui->page_activation_busy = active;
    return changed;
}

void psp_ui_set_scroll(PspUiState *ui, int scroll_y, int maximum_scroll_y)
{
    if (ui == NULL) return;
    if (maximum_scroll_y < 0) maximum_scroll_y = 0;
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > maximum_scroll_y) scroll_y = maximum_scroll_y;
    ui->scroll_y = scroll_y;
    ui->maximum_scroll_y = maximum_scroll_y;
}

void psp_ui_set_page_interaction(PspUiState *ui, PspUiCursorShape cursor,
                                 unsigned scrollbar_width)
{
    if (ui == NULL) return;
    ui->cursor_shape = (uint8_t) cursor;
    ui->page_scrollbar_width =
        scrollbar_width > 2u ? 0u : (uint8_t) scrollbar_width;
}

void psp_ui_set_focus(PspUiState *ui, bool visible, int x, int y,
                      int width, int height)
{
    if (ui == NULL) return;
    /*
     * A zero-sized form control is still a real sequential/script focus
     * target. draw_focus() inflates the authored box by four pixels on each
     * edge, so retaining it here produces an accessible 8x8 marker instead
     * of making focus silently disappear. Negative extents are invalid.
     */
    ui->has_focus = visible && width >= 0 && height >= 0;
    ui->focus_x = x;
    ui->focus_y = y;
    ui->focus_width = width;
    ui->focus_height = height;
}

void psp_ui_show_status(PspUiState *ui, const char *status,
                        unsigned duration_frames)
{
    if (ui == NULL) return;
    ui->tls_toast_guidance = TILEFINCH_TLS_GUIDANCE_NONE;
    ui->captive_portal_suggested = false;
    copy_string(ui->status, sizeof(ui->status), status);
    unsigned frames = duration_frames == 0
        ? UI_TOAST_DEFAULT_FRAMES : duration_frames;
    ui->toast_frames = (uint16_t) (frames > UINT16_MAX ? UINT16_MAX : frames);
    ui->toast_entry_frames = PSP_THEME_MOTION_TOAST_FRAMES;
}

void psp_ui_show_wifi_sign_in_status(
    PspUiState *ui, const char *headline, unsigned duration_frames)
{
    if (ui == NULL) return;
    psp_ui_show_status(ui, headline, duration_frames);
    ui->captive_portal_suggested = true;
}

void psp_ui_show_tls_status(
    PspUiState *ui, const char *headline,
    TilefinchTlsGuidance guidance, unsigned duration_frames)
{
    if (ui == NULL) return;
    psp_ui_show_status(ui, headline, duration_frames);
    if (guidance > TILEFINCH_TLS_GUIDANCE_NONE
        && guidance <= TILEFINCH_TLS_GUIDANCE_DETAILS) {
        ui->tls_toast_guidance = (unsigned) guidance;
    }
}

void psp_ui_set_update(
    PspUiState *ui, const char *version, const char *status,
    const char *notes, int progress_per_mille, const char *primary_label,
    bool primary_enabled, bool cancel_enabled)
{
    if (ui == NULL) return;
    ui->network_profile_label_valid = false;
    copy_string(ui->update_version, sizeof(ui->update_version), version);
    copy_string(ui->update_status, sizeof(ui->update_status), status);
    copy_string(ui->update_notes, sizeof(ui->update_notes), notes);
    copy_string(
        ui->update_primary_label, sizeof(ui->update_primary_label),
        primary_label);
    if (progress_per_mille < -1) progress_per_mille = -1;
    if (progress_per_mille > 1000) progress_per_mille = 1000;
    ui->update_progress_per_mille = progress_per_mille;
    ui->update_primary_enabled = primary_enabled;
    ui->update_cancel_enabled = cancel_enabled;
}

void psp_ui_set_update_history(
    PspUiState *ui, const TilefinchUpdateHistorySnapshot *snapshot)
{
    if (ui == NULL || snapshot == NULL) return;
    memset(ui->update_history_versions, 0,
           sizeof(ui->update_history_versions));
    size_t count = snapshot->count;
    if (count > TILEFINCH_UPDATE_HISTORY_LIMIT)
        count = TILEFINCH_UPDATE_HISTORY_LIMIT;
    for (size_t index = 0; index < count; index++) {
        copy_string(ui->update_history_versions[index],
                    sizeof(ui->update_history_versions[index]),
                    snapshot->versions[index]);
    }
    ui->update_history_count = (unsigned) count;
    ui->update_history_phase = (unsigned) snapshot->phase;
    if (ui->data_options_selection >= count)
        ui->data_options_selection = 0u;
}

bool psp_ui_update_history_tag(
    const PspUiState *ui, size_t index, char *output, size_t capacity)
{
    if (ui == NULL || index >= ui->update_history_count
        || output == NULL || capacity == 0) return false;
    int written = snprintf(
        output, capacity, "v%s", ui->update_history_versions[index]);
    return written > 0 && (size_t) written < capacity;
}

void psp_ui_set_voice_component(
    PspUiState *ui, PspUiVoiceComponentPhase phase,
    int progress_per_mille)
{
    if (ui == NULL) return;
    ui->voice_component_phase = (uint8_t) phase;
    if (progress_per_mille < -1) progress_per_mille = -1;
    if (progress_per_mille > 1000) progress_per_mille = 1000;
    ui->voice_component_progress_plus_one =
        (unsigned) (progress_per_mille + 1);
    if (phase != PSP_UI_VOICE_COMPONENT_READY)
        ui->voice_component_remove_confirmation = false;
}

void psp_ui_set_glyph_component(
    PspUiState *ui, uint16_t installed_mask, uint8_t operation_pack,
    PspUiGlyphComponentPhase phase, int progress_per_mille)
{
    if (ui == NULL) return;
    ui->glyph_installed_mask = installed_mask;
    ui->glyph_operation_pack = operation_pack;
    ui->glyph_component_phase = (uint8_t) phase;
    if (progress_per_mille < -1) progress_per_mille = -1;
    if (progress_per_mille > 1000) progress_per_mille = 1000;
    ui->glyph_component_progress_plus_one =
        (unsigned) (progress_per_mille + 1);
    if (phase != PSP_UI_GLYPH_COMPONENT_READY)
        ui->glyph_component_remove_confirmation = false;
}

void psp_ui_set_tabs(PspUiState *ui, const PspUiTabsView *tabs)
{
    if (ui == NULL) return;
    ui->tabs = tabs;
    size_t row_count = tabs == NULL ? 1u : tabs->count;
    if (row_count > PSP_UI_TAB_LIMIT) row_count = PSP_UI_TAB_LIMIT;
    if (tabs != NULL && tabs->can_create
        && row_count < PSP_UI_TAB_LIMIT) row_count++;
    if (ui->tab_selection >= row_count) {
        ui->tab_selection = row_count == 0 ? 0 : (uint8_t) (row_count - 1u);
    }
}

bool psp_ui_screen_is_native_surface(PspUiScreen screen)
{
    return screen == PSP_UI_SCREEN_HOME
        || screen == PSP_UI_SCREEN_COLLECTIONS;
}

static size_t ui_home_row_count(const PspUiState *ui)
{
    if (ui == NULL || ui->home == NULL) return 0u;
    size_t tiles = ui->home->tile_count;
    if (tiles > PSP_UI_HOME_TILE_LIMIT) tiles = PSP_UI_HOME_TILE_LIMIT;
    size_t continues = ui->home->continue_count;
    if (continues > PSP_UI_HOME_CONTINUE_LIMIT)
        continues = PSP_UI_HOME_CONTINUE_LIMIT;
    return tiles + continues;
}

static size_t ui_collections_row_count(const PspUiState *ui)
{
    if (ui == NULL || ui->collections == NULL) return 0u;
    size_t count = ui->collections->count;
    return count > PSP_UI_COLLECTIONS_ROW_LIMIT
        ? PSP_UI_COLLECTIONS_ROW_LIMIT : count;
}

void psp_ui_set_home(PspUiState *ui, const PspUiHomeView *home)
{
    if (ui == NULL) return;
    ui->home = home;
    size_t rows = ui_home_row_count(ui);
    if (rows == 0) ui->home_selection = 0;
    else if (ui->home_selection >= rows)
        ui->home_selection = (uint8_t) (rows - 1u);
}

void psp_ui_set_collections(
    PspUiState *ui, const PspUiCollectionsView *collections)
{
    if (ui == NULL) return;
    ui->collections = collections;
    ui->collections_delete_confirmation = 0;
    size_t rows = ui_collections_row_count(ui);
    if (rows == 0) {
        ui->collections_selection = 0;
        ui->collections_first_row = 0;
        return;
    }
    if (ui->collections_selection >= rows)
        ui->collections_selection = (uint8_t) (rows - 1u);
    if (ui->collections_first_row > ui->collections_selection)
        ui->collections_first_row = ui->collections_selection;
}

void psp_ui_show_collections(
    PspUiState *ui, PspUiCollectionSection section)
{
    if (ui == NULL) return;
    if (ui->screen != PSP_UI_SCREEN_COLLECTIONS) {
        ui->collections_return_home =
            ui->screen == PSP_UI_SCREEN_HOME
            || ui->base_screen == PSP_UI_SCREEN_HOME;
    }
    ui->collections_section =
        (unsigned) section % PSP_UI_COLLECTION_SECTION_COUNT;
    ui->collections_selection = 0;
    ui->collections_first_row = 0;
    ui->collections_delete_confirmation = 0;
    ui_open_overlay(ui, PSP_UI_SCREEN_COLLECTIONS);
    ui->base_screen = (uint8_t) PSP_UI_SCREEN_COLLECTIONS;
}

void psp_ui_show_offline_app_preview(
    PspUiState *ui, const PspUiOfflineAppPreview *preview)
{
    if (ui == NULL || preview == NULL) return;
    ui->base_screen = (uint8_t) PSP_UI_SCREEN_PAGE;
    ui->offline_app_preview = preview;
    ui->menu_selection = 0u;
    ui_open_overlay(ui, PSP_UI_SCREEN_OFFLINE_APP_PREVIEW);
}

void psp_ui_show_failure_recovery_actions(
    PspUiState *ui, const char *detail, uint8_t available_actions)
{
    if (ui == NULL) return;
    ui->base_screen = (uint8_t) (psp_ui_screen_is_native_surface(ui->screen)
        ? ui->screen : PSP_UI_SCREEN_PAGE);
    ui->failure_actions = available_actions;
    ui->menu_selection = 0u;
    snprintf(ui->status, sizeof(ui->status), "%s",
             detail == NULL || detail[0] == '\0'
                ? "The page could not be opened" : detail);
    ui_open_overlay(ui, PSP_UI_SCREEN_FAILURE_RECOVERY);
}

void psp_ui_show_failure_recovery(
    PspUiState *ui, const char *detail, uint8_t available_actions)
{
    psp_ui_show_failure_recovery_actions(
        ui, detail, available_actions | PSP_UI_FAILURE_READER);
}

bool psp_ui_legacy_collection_url(
    const char *url, PspUiCollectionSection *section)
{
    if (url == NULL) return false;
    /* Only these three exact internal URLs -- the pre-upgrade HTML
       generators' addresses -- map onto a COLLECTIONS section. A trailing
       slash is tolerated; anything with a further path (an offline item
       route such as /offline/video?id=, an /offline/youtube save link) is
       left for the ordinary navigation arms and is not a collection. */
    static const struct {
        const char *path;
        PspUiCollectionSection section;
    } legacy[] = {
        {"https://tilefinch.local/bookmarks", PSP_UI_COLLECTION_BOOKMARKS},
        {"https://tilefinch.local/history", PSP_UI_COLLECTION_HISTORY},
        {"https://tilefinch.local/offline", PSP_UI_COLLECTION_OFFLINE},
    };
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        size_t length = strlen(legacy[i].path);
        if (strncmp(url, legacy[i].path, length) != 0) continue;
        if (url[length] != '\0'
            && !(url[length] == '/' && url[length + 1u] == '\0'))
            continue;
        if (section != NULL) *section = legacy[i].section;
        return true;
    }
    return false;
}

bool psp_ui_native_home_url(const char *url)
{
    if (url == NULL) return false;
    /* Spelled out rather than taken from TILEFINCH_HOMEPAGE_URL so this file
       keeps its light include set, exactly as the legacy classifier above
       spells its three URLs. A trailing slash is tolerated and nothing else
       is: a further path under /home is a different address and must stay
       with the ordinary navigation arms. */
    static const char home[] = "https://tilefinch.local/home";
    size_t length = sizeof(home) - 1u;
    if (strncmp(url, home, length) != 0) return false;
    return url[length] == '\0'
        || (url[length] == '/' && url[length + 1u] == '\0');
}

bool psp_ui_internal_url(const char *url)
{
    if (url == NULL) return false;
    static const char origin[] = "https://tilefinch.local";
    size_t length = sizeof(origin) - 1u;
    if (strncmp(url, origin, length) != 0) return false;
    char boundary = url[length];
    if (boundary == '\0' || boundary == '/'
        || boundary == '?' || boundary == '#') return true;
    /* A URL parser is intentionally not pulled into the lightweight chrome
       library just for its own fixed origin.  Accept only the canonical
       default-port spelling; :444 and hostname-prefix lookalikes remain
       ordinary remote origins. */
    static const char default_port[] = ":443";
    if (strncmp(url + length, default_port, sizeof(default_port) - 1u) != 0)
        return false;
    boundary = url[length + sizeof(default_port) - 1u];
    return boundary == '\0' || boundary == '/'
        || boundary == '?' || boundary == '#';
}

void psp_ui_show_home(PspUiState *ui)
{
    if (ui == NULL) return;
    ui_open_overlay(ui, PSP_UI_SCREEN_HOME);
    ui->home_selection = 0;
    ui->base_screen = (uint8_t) PSP_UI_SCREEN_HOME;
}

void psp_ui_leave_native_surface(PspUiState *ui)
{
    if (ui == NULL) return;
    ui->base_screen = (uint8_t) PSP_UI_SCREEN_PAGE;
    if (psp_ui_screen_is_native_surface(ui->screen))
        ui->screen = PSP_UI_SCREEN_PAGE;
}

void psp_ui_set_text_entry(
    PspUiState *ui, const PspUiTextEntryView *view)
{
    if (ui == NULL || view == NULL) return;
    ui->text_entry = view;
    ui_open_overlay(ui, PSP_UI_SCREEN_TEXT_ENTRY);
}

void psp_ui_clear_text_entry(PspUiState *ui)
{
    if (ui != NULL) ui->text_entry = NULL;
}

void psp_ui_set_find(PspUiState *ui, const PspUiFindView *view)
{
    if (ui == NULL || view == NULL) return;
    ui->find_view = view;
    ui_open_overlay(ui, PSP_UI_SCREEN_FIND);
}

void psp_ui_clear_find(PspUiState *ui)
{
    if (ui == NULL) return;
    ui->find_view = NULL;
    if (ui->screen == PSP_UI_SCREEN_FIND)
        ui->screen = PSP_UI_SCREEN_PAGE;
}

void psp_ui_set_diagnostic_qr(
    PspUiState *ui, const TilefinchDiagnosticQrView *view)
{
    if (ui != NULL) ui->diagnostic_qr = view;
}

static int analog_scroll_delta(PspUiState *ui, const PspUiInput *input)
{
    int analog = (int) input->analog_y - 128;
    int magnitude = analog < 0 ? -analog : analog;
    if (magnitude <= UI_ANALOG_DEAD_ZONE) {
        ui->analog_hold_ms = 0;
        ui->analog_scroll_remainder = 0;
        ui->analog_scroll_direction = 0;
        return 0;
    }
    int8_t direction = analog < 0 ? -1 : 1;
    if (direction != ui->analog_scroll_direction) {
        ui->analog_hold_ms = 0;
        ui->analog_scroll_remainder = 0;
        ui->analog_scroll_direction = direction;
    }
    unsigned elapsed_ms = input->elapsed_ms == 0 ? 16u : input->elapsed_ms;
    if (elapsed_ms > UI_ANALOG_MAX_ELAPSED_MS)
        elapsed_ms = UI_ANALOG_MAX_ELAPSED_MS;
    if (ui->analog_hold_ms < UI_ANALOG_MAX_HOLD_MS) {
        unsigned remaining = UI_ANALOG_MAX_HOLD_MS - ui->analog_hold_ms;
        ui->analog_hold_ms = (uint16_t) (
            ui->analog_hold_ms
            + (elapsed_ms < remaining ? elapsed_ms : remaining));
    }

    /* Quadratic stick response retains fine control near the dead zone.
       After half a second, a bounded time-based boost makes long documents
       practical without making frame-rate fluctuations change the distance. */
    unsigned active = (unsigned) (magnitude - UI_ANALOG_DEAD_ZONE);
    unsigned velocity = 60u + active * active * 1540u / (104u * 104u);
    if (ui->analog_hold_ms > 500u) {
        unsigned boost_ms = ui->analog_hold_ms - 500u;
        if (boost_ms > 1000u) boost_ms = 1000u;
        velocity += boost_ms * 800u / 1000u;
    }
    unsigned distance_milli =
        velocity * elapsed_ms + ui->analog_scroll_remainder;
    unsigned distance = distance_milli / 1000u;
    ui->analog_scroll_remainder = (uint16_t) (distance_milli % 1000u);
    return (int) direction * (int) distance;
}

static int analog_cursor_delta(int analog, unsigned elapsed_ms)
{
    int offset = analog - 128;
    int magnitude = offset < 0 ? -offset : offset;
    if (magnitude <= UI_ANALOG_DEAD_ZONE) return 0;
    unsigned active = (unsigned) (magnitude - UI_ANALOG_DEAD_ZONE);
    unsigned velocity = 24u + active * active * 376u / (103u * 103u);
    unsigned distance_milli = velocity * elapsed_ms;
    /* A live stick sample must move a visible pixel in the same frame. Near
       the dead zone the time-based subpixel distance used to need two or
       three frames before integer presentation changed, which felt like
       input latency even though sampling was current. */
    if (distance_milli < 1000u) distance_milli = 1000u;
    return (offset < 0 ? -1 : 1) * (int) distance_milli;
}

void psp_ui_suspend_page_input(PspUiState *ui)
{
    if (ui == NULL) return;
    ui->cursor_pointer_down = false;
    ui->analog_hold_ms = 0;
    ui->analog_scroll_remainder = 0;
    ui->analog_scroll_direction = 0;
    ui->focus_repeat_direction = 0;
    ui->focus_repeat_elapsed_ms = 0;
}

bool psp_ui_color_mode_is_dark(BrowserColorMode mode, unsigned local_hour)
{
    if (mode == BROWSER_COLOR_MODE_DARK) return true;
    if (mode == BROWSER_COLOR_MODE_LIGHT) return false;
    /* A deliberately unsurprising local-time schedule. */
    return local_hour >= 19u || local_hour < 7u;
}

static unsigned dark_clamp6(int value)
{
    if (value < 0) return 0;
    if (value > 63) return 63;
    return (unsigned) value;
}

void psp_ui_apply_page_dark_rgb565(
    uint16_t *pixels, int width, int height, int stride)
{
    if (pixels == NULL || width <= 0 || height <= 0 || stride < width)
        return;
    for (int y = 0; y < height; y++) {
        uint16_t *row = pixels + (size_t) y * (size_t) stride;
        for (int x = 0; x < width; x++) {
            uint16_t source = row[x];
            int red = (int) tilefinch_rgb565_red_code(source) << 1;
            int green = (int) tilefinch_rgb565_green_code(source);
            int blue = (int) tilefinch_rgb565_blue_code(source) << 1;
            int luminance = (red + (green << 1) + blue) >> 2;
            int target = 56 - ((luminance * 3) >> 2);
            int delta = target - luminance;
            unsigned output_red = dark_clamp6(red + delta);
            unsigned output_green = dark_clamp6(green + delta);
            unsigned output_blue = dark_clamp6(blue + delta);
            row[x] = tilefinch_rgb565_pack_codes(
                output_red >> 1, output_green, output_blue >> 1);
        }
    }
}

static uint32_t repeat_directions(PspUiState *ui, uint32_t pressed,
                                  uint32_t held, uint32_t mask,
                                  unsigned elapsed_ms)
{
    uint32_t directional = pressed & mask;
    if ((pressed & ~mask) != 0) {
        ui->focus_repeat_direction = 0;
        ui->focus_repeat_elapsed_ms = 0;
    }
    uint32_t held_direction = held & mask;
    bool one_held_direction = held_direction != 0
        && (held_direction & (held_direction - 1u)) == 0;
    if (directional != 0) {
        ui->focus_repeat_direction =
            (uint16_t) (directional & (~directional + 1u));
        ui->focus_repeat_elapsed_ms = 0;
    } else if (one_held_direction
               && held_direction == ui->focus_repeat_direction) {
        unsigned accumulated = ui->focus_repeat_elapsed_ms + elapsed_ms;
        if (accumulated >= UI_FOCUS_REPEAT_DELAY_MS) {
            pressed |= held_direction;
            accumulated =
                UI_FOCUS_REPEAT_DELAY_MS - UI_FOCUS_REPEAT_INTERVAL_MS;
        }
        ui->focus_repeat_elapsed_ms = (uint16_t) accumulated;
    } else {
        ui->focus_repeat_direction =
            (uint16_t) (one_held_direction ? held_direction : 0u);
        ui->focus_repeat_elapsed_ms = 0;
    }
    return pressed;
}

/* Row geometry is declared with the drawing code; both need the same rects. */
static UiRect ui_home_row_rect(const PspUiState *ui, size_t row);
static size_t ui_home_tile_count(const PspUiState *ui);

/*
 * Squared distance from a point to a rectangle, zero inside it. Used only by
 * the cursor handoff, which wants the nearest row to where the cursor was.
 */
static long ui_rect_distance_squared(UiRect rect, int x, int y)
{
    long dx = x < rect.x ? rect.x - x
        : (x >= rect.x + rect.width ? x - (rect.x + rect.width - 1) : 0);
    long dy = y < rect.y ? rect.y - y
        : (y >= rect.y + rect.height ? y - (rect.y + rect.height - 1) : 0);
    return dx * dx + dy * dy;
}

static size_t ui_home_nearest_row(const PspUiState *ui, int x, int y)
{
    size_t rows = ui_home_row_count(ui);
    size_t nearest = 0;
    long best = -1;
    for (size_t at = 0; at < rows; at++) {
        long distance = ui_rect_distance_squared(
            ui_home_row_rect(ui, at), x, y);
        if (best < 0 || distance < best) {
            best = distance;
            nearest = at;
        }
    }
    return nearest;
}

/* Only rows currently on screen can be under the cursor. */
static size_t ui_collections_nearest_row(const PspUiState *ui, int y)
{
    size_t rows = ui_collections_row_count(ui);
    if (rows == 0) return 0;
    size_t first = ui->collections_first_row;
    size_t end = first + UI_COLLECTIONS_VISIBLE_ROWS;
    if (end > rows) end = rows;
    int offset = y - UI_COLLECTIONS_ROW_TOP;
    if (offset < 0) return first;
    size_t at = first + (size_t) (offset / UI_COLLECTIONS_ROW_STRIDE);
    return at >= end ? (end == 0 ? 0 : end - 1u) : at;
}

static size_t ui_home_step(const PspUiState *ui, size_t selection,
                           uint32_t pressed)
{
    size_t rows = ui_home_row_count(ui);
    if (rows == 0) return 0;
    size_t tiles = ui_home_tile_count(ui);
    size_t tile_rows = (tiles + UI_HOME_TILE_COLUMNS - 1u)
        / UI_HOME_TILE_COLUMNS;
    if (selection < tiles) {
        size_t column = selection % UI_HOME_TILE_COLUMNS;
        size_t row = selection / UI_HOME_TILE_COLUMNS;
        size_t row_first = row * UI_HOME_TILE_COLUMNS;
        size_t row_width = tiles - row_first;
        if (row_width > UI_HOME_TILE_COLUMNS)
            row_width = UI_HOME_TILE_COLUMNS;
        if (pressed & PSP_UI_BUTTON_LEFT)
            return row_first + (column + row_width - 1u) % row_width;
        if (pressed & PSP_UI_BUTTON_RIGHT)
            return row_first + (column + 1u) % row_width;
        if (pressed & PSP_UI_BUTTON_DOWN) {
            size_t below = selection + UI_HOME_TILE_COLUMNS;
            if (below < tiles) return below;
            /* Past the last tile row the list is what comes next; with no
               CONTINUE entries the grid wraps to its own top. */
            return rows > tiles ? tiles : column;
        }
        if (pressed & PSP_UI_BUTTON_UP) {
            if (row > 0) return selection - UI_HOME_TILE_COLUMNS;
            return rows > tiles ? rows - 1u
                                : row_first
                                      + (tile_rows - 1u)
                                            * UI_HOME_TILE_COLUMNS + column;
        }
        return selection;
    }
    if (pressed & PSP_UI_BUTTON_DOWN)
        return selection + 1u < rows ? selection + 1u : 0u;
    if (pressed & PSP_UI_BUTTON_UP) {
        if (selection > tiles) return selection - 1u;
        /* Back into the grid, landing on the last row's first column. */
        return tiles == 0 ? rows - 1u
                          : (tile_rows - 1u) * UI_HOME_TILE_COLUMNS;
    }
    return selection;
}

/*
 * Native-surface input. Kept in one place so both surfaces share the same
 * traversal, cursor handoff, and hint ownership rules; the page handling in
 * psp_ui_update() below never runs while a surface is showing.
 */
/* Keeps the selected row inside the fixed row window. */
static void ui_collections_reveal(PspUiState *ui)
{
    size_t rows = ui_collections_row_count(ui);
    if (rows == 0) {
        ui->collections_first_row = 0;
        return;
    }
    size_t selection = ui->collections_selection;
    size_t first = ui->collections_first_row;
    if (selection < first) first = selection;
    else if (selection >= first + UI_COLLECTIONS_VISIBLE_ROWS)
        first = selection - UI_COLLECTIONS_VISIBLE_ROWS + 1u;
    size_t maximum_first = rows > UI_COLLECTIONS_VISIBLE_ROWS
        ? rows - UI_COLLECTIONS_VISIBLE_ROWS : 0u;
    if (first > maximum_first) first = maximum_first;
    ui->collections_first_row = (uint8_t) first;
}

/*
 * The five sections switch with L/R, matching the settings overlay. The
 * surface changes section itself and reports it, because the row data behind
 * a section belongs to the frontend and has to be refreshed with it.
 */
static PspUiAction ui_collections_section_action(unsigned section)
{
    switch (section % PSP_UI_COLLECTION_SECTION_COUNT) {
        case PSP_UI_COLLECTION_BOOKMARKS:
            return PSP_UI_ACTION_SHOW_BOOKMARKS;
        case PSP_UI_COLLECTION_HISTORY:
            return PSP_UI_ACTION_SHOW_HISTORY;
        case PSP_UI_COLLECTION_DOWNLOADS:
            return PSP_UI_ACTION_SHOW_DOWNLOADS;
        case PSP_UI_COLLECTION_SCREENSHOTS:
            return PSP_UI_ACTION_SHOW_SCREENSHOTS;
        case PSP_UI_COLLECTION_SAVED:
        default:
            return PSP_UI_ACTION_SHOW_OFFLINE;
    }
}

static void ui_update_collections(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    size_t rows = ui_collections_row_count(ui);
    if (pressed & (PSP_UI_BUTTON_PAGE_UP | PSP_UI_BUTTON_PAGE_DOWN)) {
        unsigned section = ui->collections_section;
        section = (pressed & PSP_UI_BUTTON_PAGE_UP)
            ? (section + PSP_UI_COLLECTION_SECTION_COUNT - 1u)
                  % PSP_UI_COLLECTION_SECTION_COUNT
            : (section + 1u) % PSP_UI_COLLECTION_SECTION_COUNT;
        psp_ui_show_collections(ui, (PspUiCollectionSection) section);
        intent->action = ui_collections_section_action(section);
        intent->visual_changed = true;
        return;
    }
    if (pressed & PSP_UI_BUTTON_CANCEL) {
        if (ui->collections_delete_confirmation != 0) {
            ui->collections_delete_confirmation = 0;
        } else {
            PspUiScreen destination = ui->collections_return_home
                ? PSP_UI_SCREEN_HOME : PSP_UI_SCREEN_PAGE;
            ui->base_screen = (uint8_t) destination;
            ui->screen = destination;
        }
        intent->visual_changed = true;
        return;
    }
    if (rows == 0) return;
    if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN)) {
        ui->collections_delete_confirmation = 0;
        size_t selection = ui->collections_selection;
        selection = (pressed & PSP_UI_BUTTON_UP)
            ? (selection + rows - 1u) % rows
            : (selection + 1u) % rows;
        ui->collections_selection = (uint8_t) selection;
        ui_collections_reveal(ui);
        ui->focus_settle_frames = PSP_THEME_MOTION_FOCUS_FRAMES;
        intent->visual_changed = true;
        return;
    }
    if (pressed & PSP_UI_BUTTON_RELOAD) {
        /* Deletion is destructive and unattended, so it always costs two
           deliberate presses on the same row. */
        if (!ui->collections->rows[ui->collections_selection].deletable) {
            psp_ui_show_status(ui, "THIS CANNOT BE DELETED", 120);
            intent->visual_changed = true;
            return;
        }
        uint8_t confirmation = (uint8_t) (ui->collections_selection + 1u);
        if (ui->collections_delete_confirmation != confirmation) {
            ui->collections_delete_confirmation = confirmation;
        } else {
            ui->collections_delete_confirmation = 0;
            intent->action = PSP_UI_ACTION_COLLECTION_DELETE;
            intent->list_index = ui->collections_selection;
        }
        intent->visual_changed = true;
        return;
    }
    if (pressed & PSP_UI_BUTTON_CONFIRM) {
        ui->collections_delete_confirmation =
            UI_COLLECTIONS_ACTIVATION_FEEDBACK;
        intent->action = PSP_UI_ACTION_COLLECTION_ACTIVATE;
        intent->list_index = ui->collections_selection;
        intent->visual_changed = true;
    }
}

static void ui_update_native_surface(
    PspUiState *ui, uint32_t pressed, PspUiIntent *intent)
{
    if (ui->screen != PSP_UI_SCREEN_HOME) {
        ui_update_collections(ui, pressed, intent);
        return;
    }

    size_t rows = ui_home_row_count(ui);
    if (pressed & PSP_UI_BUTTON_CANCEL) {
        /* Home is the floor of the chrome, not a page with a back entry.
           Say so instead of silently doing nothing. */
        psp_ui_show_status(ui, "ALREADY HOME", 120);
        intent->visual_changed = true;
        return;
    }
    if (rows == 0) return;
    uint32_t directional = pressed
        & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
           | PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT);
    if (directional != 0) {
        size_t next = ui_home_step(ui, ui->home_selection, directional);
        if (next >= rows) next = rows - 1u;
        if (next != ui->home_selection) {
            ui->home_selection = (uint8_t) next;
            ui->focus_settle_frames = PSP_THEME_MOTION_FOCUS_FRAMES;
        }
        intent->visual_changed = true;
        return;
    }
    if (pressed & PSP_UI_BUTTON_CONFIRM) {
        intent->action = PSP_UI_ACTION_HOME_ACTIVATE;
        intent->list_index = ui->home_selection;
        intent->visual_changed = true;
    }
}

PspUiIntent psp_ui_update(PspUiState *ui, const PspUiInput *input)
{
    PspUiIntent intent = { .action = PSP_UI_ACTION_NONE };
    if (ui == NULL || input == NULL) return intent;
    ui->loading_phase = (ui->loading_phase + 1u) % 1024u;
    if (ui->overlay_animation_frames > 0) {
        ui->overlay_animation_frames--;
        intent.visual_changed = true;
    }
    if (ui->toast_entry_frames > 0) {
        ui->toast_entry_frames--;
        intent.visual_changed = true;
    }
    if (ui->focus_settle_frames > 0) {
        ui->focus_settle_frames--;
        intent.visual_changed = true;
    }
    /*
     * The cursor arrives and leaves over the same focus-settle budget the
     * rings use, instead of appearing at full strength mid-gesture and
     * vanishing mid-press. The step runs here, before every early exit this
     * function has, so no path can skip a frame of the fade; the frame that
     * first moves the cursor lights it directly so the fade never costs the
     * user a frame of feedback.
     */
    unsigned cursor_fade_target = ui->cursor_visible
        ? PSP_THEME_MOTION_FOCUS_FRAMES : 0u;
    if (ui->cursor_fade != cursor_fade_target) {
        ui->cursor_fade = ui->cursor_fade < cursor_fade_target
            ? ui->cursor_fade + 1u : ui->cursor_fade - 1u;
        intent.visual_changed = true;
    }
    if (ui->toast_frames > 0) {
        ui->toast_frames--;
        if (ui->toast_frames == 0) {
            ui->captive_portal_suggested = false;
            intent.visual_changed = true;
        }
    }

    uint32_t pressed = input->pressed;
    unsigned elapsed_ms = input->elapsed_ms == 0 ? 16u : input->elapsed_ms;
    if (elapsed_ms > UI_ANALOG_MAX_ELAPSED_MS)
        elapsed_ms = UI_ANALOG_MAX_ELAPSED_MS;
    if (ui->cursor_pointer_down
        && !(input->held & PSP_UI_BUTTON_CONFIRM)) {
        ui->cursor_pointer_down = false;
        intent.pointer_phase = PSP_UI_POINTER_UP;
        intent.pointer_x = ui->cursor_x_milli / 1000;
        intent.pointer_y = ui->cursor_y_milli / 1000;
    }
    uint32_t directional = pressed
        & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
           | PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT);
    if ((pressed & ~PSP_UI_BUTTON_CONFIRM) != 0
        && ui->cursor_pointer_down) {
        ui->cursor_pointer_down = false;
        intent.pointer_phase = PSP_UI_POINTER_CANCEL;
        intent.pointer_x = ui->cursor_x_milli / 1000;
        intent.pointer_y = ui->cursor_y_milli / 1000;
    }
    /*
     * Implicit cursor handoff. There is no mode switch between pointing and
     * pressing: analog movement fades the cursor in, and the first direction
     * pressed while it is visible hands its position to focus rather than
     * moving focus from wherever it happened to be. The press is spent on
     * the handoff; the next press moves.
     */
    bool cursor_handoff = directional != 0 && ui->cursor_visible
        && ui->analog_cursor_enabled;
    int handoff_x = ui->cursor_x_milli / 1000;
    int handoff_y = ui->cursor_y_milli / 1000;
    if (directional != 0) {
        if (ui->cursor_visible) intent.visual_changed = true;
        ui->cursor_visible = false;
    }
    bool activity = pressed != 0 || input->held != 0
        || input->analog_x < 104 || input->analog_x > 152
        || input->analog_y < 104 || input->analog_y > 152;
    if (activity) {
        ui->activity_frames = UI_AUTOHIDE_FRAMES;
    } else if (ui->activity_frames > 0) {
        ui->activity_frames--;
        if (ui->activity_frames == 0 && !ui->loading
            && !ui_has_overlay(ui)) {
            ui->chrome_visible = false;
            intent.visual_changed = true;
        }
    }
    if (ui_has_overlay(ui)) {
        /* Overlay time must not count as sustained page scrolling. */
        ui->analog_hold_ms = 0;
        ui->analog_scroll_remainder = 0;
        ui->analog_scroll_direction = 0;
        if (ui->screen != PSP_UI_SCREEN_FIND) {
            ui->focus_repeat_direction = 0;
            ui->focus_repeat_elapsed_ms = 0;
        }
        /* Native surfaces are chrome the cursor is meant to reach; only the
           panel overlays above the page take it away. */
        if (!psp_ui_screen_is_native_surface(ui->screen))
            ui->cursor_visible = false;
    }

    /* Menu-owned screens and the global Menu-button escape are routed through
       one controller. Ordinary page frames do not cross the function
       boundary, keeping this refactor out of the steady browsing hot path. */
    if ((pressed & PSP_UI_BUTTON_MENU) != 0u
        || psp_ui_menu_owns_screen(ui->screen)) {
        if (psp_ui_menu_update(ui, pressed, &intent)) return intent;
    }

    if (ui->captive_portal_suggested && ui->toast_frames != 0) {
        if (pressed & PSP_UI_BUTTON_CONFIRM) {
            ui->captive_portal_suggested = false;
            ui->toast_frames = 0;
            intent.action = PSP_UI_ACTION_CHECK_WIFI_SIGN_IN;
            intent.visual_changed = true;
            return intent;
        }
        if (pressed & PSP_UI_BUTTON_CANCEL) {
            ui->captive_portal_suggested = false;
            ui->toast_frames = 0;
            intent.visual_changed = true;
            return intent;
        }
    }

    if (ui->screen == PSP_UI_SCREEN_UPDATE) {
        if (pressed & PSP_UI_BUTTON_CONFIRM) {
            if (ui->update_primary_enabled)
                intent.update_primary_requested = true;
            intent.visual_changed = true;
        } else if ((pressed & PSP_UI_BUTTON_RELOAD)
                   && !ui->update_cancel_enabled
                   && ui->update_channel == BROWSER_UPDATE_CHANNEL_STABLE) {
            ui->data_options_selection = 0u;
            ui->update_history_count = 0u;
            ui->update_history_phase = TILEFINCH_UPDATE_HISTORY_LOADING;
            ui_open_child_overlay(ui, PSP_UI_SCREEN_UPDATE_VERSIONS);
            intent.update_versions_requested = true;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            if (ui->update_cancel_enabled) {
                intent.update_cancel_requested = true;
            } else {
                ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            }
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_UPDATE_VERSIONS) {
        size_t count = ui->update_history_count;
        bool ready = ui->update_history_phase
            == TILEFINCH_UPDATE_HISTORY_READY;
        if (ready && count != 0 && (pressed & PSP_UI_BUTTON_UP)) {
            ui->data_options_selection = (uint8_t) (
                (ui->data_options_selection + count - 1u) % count);
            intent.visual_changed = true;
        } else if (ready && count != 0 && (pressed & PSP_UI_BUTTON_DOWN)) {
            ui->data_options_selection = (uint8_t) (
                (ui->data_options_selection + 1u) % count);
            intent.visual_changed = true;
        } else if (ready && count != 0
                   && (pressed & PSP_UI_BUTTON_CONFIRM)) {
            intent.update_version_selected = true;
            intent.list_index = ui->data_options_selection;
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_UPDATE);
            intent.visual_changed = true;
        } else if (ui->update_history_phase
                       == TILEFINCH_UPDATE_HISTORY_ERROR
                   && (pressed & PSP_UI_BUTTON_CONFIRM)) {
            ui->update_history_phase = TILEFINCH_UPDATE_HISTORY_LOADING;
            intent.update_versions_requested = true;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_UPDATE);
            intent.update_versions_closed = true;
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_DIAGNOSTIC_QR) {
        if (pressed & PSP_UI_BUTTON_CANCEL) {
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_HELP);
            intent.action = PSP_UI_ACTION_CLOSE_DIAGNOSTIC_QR;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_PAGE_UP)) {
            if (ui->diagnostic_qr != NULL
                && ui->diagnostic_qr->page_count > 1u)
                intent.action = PSP_UI_ACTION_DIAGNOSTIC_QR_PREVIOUS;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_RIGHT | PSP_UI_BUTTON_PAGE_DOWN)) {
            if (ui->diagnostic_qr != NULL
                && ui->diagnostic_qr->page_count > 1u)
                intent.action = PSP_UI_ACTION_DIAGNOSTIC_QR_NEXT;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_UP) {
            if (ui->diagnostic_qr != NULL
                && ui->diagnostic_qr->part_count > 1u)
                intent.action = PSP_UI_ACTION_DIAGNOSTIC_QR_PART_PREVIOUS;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_DOWN) {
            if (ui->diagnostic_qr != NULL
                && ui->diagnostic_qr->part_count > 1u)
                intent.action = PSP_UI_ACTION_DIAGNOSTIC_QR_PART_NEXT;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_CONFIRM | PSP_UI_BUTTON_RELOAD)) {
            intent.action = PSP_UI_ACTION_BUILD_DIAGNOSTIC_QR;
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_GLYPH_OPTIONS) {
        if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
            ui->glyph_component_remove_confirmation = false;
            ui->glyph_options_selection =
                (ui->glyph_options_selection + 3u) % 4u;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            ui->glyph_component_remove_confirmation = false;
            ui->glyph_options_selection =
                (ui->glyph_options_selection + 1u) % 4u;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            if (ui->glyph_component_remove_confirmation) {
                ui->glyph_component_remove_confirmation = false;
            } else {
                ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            }
            intent.visual_changed = true;
        } else if (ui->glyph_options_selection == 0u
                   && (pressed & (PSP_UI_BUTTON_LEFT
                                  | PSP_UI_BUTTON_RIGHT
                                  | PSP_UI_BUTTON_CONFIRM))) {
            int direction = (pressed & PSP_UI_BUTTON_LEFT) ? -1 : 1;
            int language = (int) ui->glyph_language;
            language = (language + (int) BROWSER_GLYPH_LANGUAGE_COUNT
                        + direction)
                % (int) BROWSER_GLYPH_LANGUAGE_COUNT;
            ui->glyph_language = (unsigned) language;
            intent.setting.id = PSP_UI_SETTING_GLYPH_LANGUAGE;
            intent.setting.value.glyph_language =
                (BrowserGlyphLanguage) language;
            intent.visual_changed = true;
        } else if (ui->glyph_options_selection == 1u
                   && (pressed & (PSP_UI_BUTTON_LEFT
                                  | PSP_UI_BUTTON_RIGHT
                                  | PSP_UI_BUTTON_CONFIRM))) {
            ui->color_emoji = !ui->color_emoji;
            intent.setting.id = PSP_UI_SETTING_COLOR_EMOJI;
            intent.setting.value.boolean = ui->color_emoji;
            intent.visual_changed = true;
        } else if (ui->glyph_options_selection >= 2u
                   && (pressed & (PSP_UI_BUTTON_CONFIRM
                                  | PSP_UI_BUTTON_RELOAD))) {
            TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_COLOR_EMOJI;
            bool available = ui->glyph_options_selection == 3u
                || ui_glyph_language_pack(ui->glyph_language, &pack);
            if (available) {
                bool installed =
                    (ui->glyph_installed_mask & (1u << (unsigned) pack)) != 0;
                bool active = ui->glyph_operation_pack == (unsigned) pack
                    && (ui->glyph_component_phase
                            == PSP_UI_GLYPH_COMPONENT_CHECKING
                        || ui->glyph_component_phase
                            == PSP_UI_GLYPH_COMPONENT_DOWNLOADING
                        || ui->glyph_component_phase
                            == PSP_UI_GLYPH_COMPONENT_INSTALLING);
                intent.glyph_component_pack = (uint8_t) pack;
                if (pressed & PSP_UI_BUTTON_RELOAD) {
                    if (installed) {
                        if (ui->glyph_component_remove_confirmation) {
                            intent.glyph_component_remove_requested = true;
                            ui->glyph_component_remove_confirmation = false;
                        } else {
                            ui->glyph_component_remove_confirmation = true;
                        }
                    }
                } else if (active) {
                    intent.glyph_component_cancel_requested = true;
                } else {
                    intent.glyph_component_primary_requested = true;
                }
                intent.visual_changed = true;
            }
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_THEME_OPTIONS) {
        const PspUiThemeCatalog *catalog = psp_ui_theme_catalog_bound();
        size_t count = (size_t) BROWSER_CHROME_THEME_CUSTOM
            + psp_ui_theme_catalog_count(catalog);
        if (count == 0u) count = BROWSER_CHROME_THEME_CUSTOM;
        if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
            ui->data_options_selection = (uint8_t) (
                (ui->data_options_selection + count - 1u) % count);
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            ui->data_options_selection = (uint8_t) (
                (ui->data_options_selection + 1u) % count);
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
            intent.theme_selected = true;
            intent.theme_index = ui->data_options_selection;
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            intent.theme_catalog_closed = true;
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_VIDEO_LANGUAGE_OPTIONS) {
        if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
            ui->data_options_selection =
                (uint8_t) ((ui->data_options_selection + 4u) % 5u);
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            ui->data_options_selection =
                (uint8_t) ((ui->data_options_selection + 1u) % 5u);
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT
                              | PSP_UI_BUTTON_CONFIRM)) {
            int direction = (pressed & PSP_UI_BUTTON_LEFT) ? -1 : 1;
            if (ui->data_options_selection == 0u) {
                int language = (int) ui->video_language;
                language = (language + (int) BROWSER_VIDEO_LANGUAGE_COUNT
                            + direction)
                    % (int) BROWSER_VIDEO_LANGUAGE_COUNT;
                ui->video_language = (uint16_t) language;
                intent.setting.id = PSP_UI_SETTING_VIDEO_LANGUAGE;
                intent.setting.value.video_language =
                    (BrowserVideoLanguage) language;
            } else if (ui->data_options_selection == 1u) {
                int language = (int) ui->subtitle_language;
                language =
                    (language + (int) BROWSER_SUBTITLE_LANGUAGE_COUNT
                     + direction)
                    % (int) BROWSER_SUBTITLE_LANGUAGE_COUNT;
                ui->subtitle_language = (uint16_t) language;
                intent.setting.id = PSP_UI_SETTING_SUBTITLE_LANGUAGE;
                intent.setting.value.subtitle_language =
                    (BrowserSubtitleLanguage) language;
            } else if (ui->data_options_selection == 2u) {
                int language = (int) ui->alternate_language;
                if (direction > 0)
                    language = language == BROWSER_ALTERNATE_LANGUAGE_NONE
                        ? BROWSER_ALTERNATE_LANGUAGE_ENGLISH
                        : language + 1 >= BROWSER_ALTERNATE_LANGUAGE_COUNT
                            ? BROWSER_ALTERNATE_LANGUAGE_NONE
                            : language + 1;
                else
                    language = language == BROWSER_ALTERNATE_LANGUAGE_NONE
                        ? BROWSER_ALTERNATE_LANGUAGE_RUSSIAN
                        : language == BROWSER_ALTERNATE_LANGUAGE_ENGLISH
                            ? BROWSER_ALTERNATE_LANGUAGE_NONE
                            : language - 1;
                ui->alternate_language = (uint16_t) language;
                intent.setting.id = PSP_UI_SETTING_ALTERNATE_LANGUAGE;
                intent.setting.value.alternate_language =
                    (BrowserAlternateLanguage) language;
            } else if (ui->data_options_selection == 3u) {
                ui->subtitle_size ^= 1u;
                intent.setting.id = PSP_UI_SETTING_SUBTITLE_SIZE;
                intent.setting.value.subtitle_size =
                    (BrowserSubtitleSize) ui->subtitle_size;
            } else {
                ui->subtitle_background ^= 1u;
                intent.setting.id = PSP_UI_SETTING_SUBTITLE_BACKGROUND;
                intent.setting.value.subtitle_background =
                    (BrowserSubtitleBackground) ui->subtitle_background;
            }
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS) {
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        /* A saved selection is already on the card; this only asks whether to
           apply it now. Confirm re-emits the picker's one action, which the
           receiver reads as "restart" because it set this bit; cancel leaves
           the value saved for the next launch and returns to the row. */
        if (ui->experimental_decoder_restart_prompt) {
            if (pressed & PSP_UI_BUTTON_CONFIRM) {
                intent.action = PSP_UI_ACTION_SET_VIDEO_DECODER;
                intent.visual_changed = true;
            } else if (pressed & PSP_UI_BUTTON_CANCEL) {
                ui->experimental_decoder_restart_prompt = 0;
                intent.visual_changed = true;
            }
            return intent;
        }
#endif
        if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
            ui->experimental_options_selection =
                (ui->experimental_options_selection
                 + UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT - 1u)
                % UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            ui->experimental_options_selection =
                (ui->experimental_options_selection + 1u)
                % UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            if (ui->voice_component_remove_confirmation) {
                ui->voice_component_remove_confirmation = false;
                intent.visual_changed = true;
                return intent;
            }
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTIONS);
            ui->options_group_selection = 6u;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT
                              | PSP_UI_BUTTON_CONFIRM)) {
            if (ui->experimental_options_selection == 0) {
                if (ui->voice_component_phase
                        == PSP_UI_VOICE_COMPONENT_READY
                    || ui->voice_component_phase
                        == PSP_UI_VOICE_COMPONENT_LEGACY) {
                    ui->experimental_voice_input =
                        !ui->experimental_voice_input;
                    intent.setting.id = PSP_UI_SETTING_EXPERIMENTAL_VOICE;
                    intent.setting.value.boolean =
                        ui->experimental_voice_input;
                } else {
                    /* Downloading is an explicit action on the size-labelled
                       model row. Selecting Voice input without a model moves
                       there; it never starts network I/O by surprise. */
                    ui->experimental_options_selection = 1;
                }
            } else if (ui->experimental_options_selection == 1) {
                if (ui->voice_component_phase
                        == PSP_UI_VOICE_COMPONENT_READY) {
                    if (ui->voice_component_remove_confirmation) {
                        intent.voice_component_remove_requested = true;
                        ui->voice_component_remove_confirmation = false;
                    } else {
                        ui->voice_component_remove_confirmation = true;
                    }
                } else if (ui->voice_component_phase
                               == PSP_UI_VOICE_COMPONENT_CHECKING
                           || ui->voice_component_phase
                               == PSP_UI_VOICE_COMPONENT_DOWNLOADING
                           || ui->voice_component_phase
                               == PSP_UI_VOICE_COMPONENT_INSTALLING) {
                    intent.voice_component_cancel_requested = true;
                } else {
                    intent.voice_component_primary_requested = true;
                }
            } else if (ui->experimental_options_selection == 2) {
                if ((pressed & PSP_UI_BUTTON_CONFIRM)
                    && ui->experimental_voice_input) {
                    ui->screen = PSP_UI_SCREEN_PAGE;
                    intent.action = PSP_UI_ACTION_OPEN_VOICE_ADDRESS;
                }
            } else if (ui->experimental_options_selection == 3) {
                ui->adaptive_voice_memory =
                    !ui->adaptive_voice_memory;
                intent.setting.id = PSP_UI_SETTING_ADAPTIVE_VOICE_MEMORY;
                intent.setting.value.boolean =
                    ui->adaptive_voice_memory;
            } else if (ui->experimental_options_selection == 4) {
                unsigned channel = ui->update_channel;
                bool backwards = (pressed & PSP_UI_BUTTON_LEFT) != 0;
                do {
                    channel = backwards
                        ? (channel == BROWSER_UPDATE_CHANNEL_STABLE
                               ? BROWSER_UPDATE_CHANNEL_DEVELOPER
                               : channel - 1u)
                        : (channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
                               ? BROWSER_UPDATE_CHANNEL_STABLE
                               : channel + 1u);
                } while (channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
                         && !ui->developer_update_available);
                ui->update_channel = (uint8_t) channel;
                intent.setting.id = PSP_UI_SETTING_UPDATE_CHANNEL;
                intent.setting.value.update_channel =
                    (BrowserUpdateChannel) channel;
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
            } else if (ui->experimental_options_selection
                       == UI_EXPERIMENTAL_ROW_VIDEO_DECODER) {
                /* Left/Right browses the spellings without touching the card.
                   Only Confirm commits, so a walk through the list costs one
                   Memory Stick write rather than one per step. */
                if (pressed & PSP_UI_BUTTON_CONFIRM) {
                    intent.action = PSP_UI_ACTION_SET_VIDEO_DECODER;
                } else {
                    unsigned choice = ui->experimental_decoder_choice;
                    choice = (pressed & PSP_UI_BUTTON_LEFT)
                        ? (choice + PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT - 1u)
                        : (choice + 1u);
                    ui->experimental_decoder_choice = (uint8_t)
                        (choice % PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT);
                }
#endif
            } else if (ui->experimental_options_selection == 5
                       && (pressed & PSP_UI_BUTTON_CONFIRM)) {
                intent.action = PSP_UI_ACTION_EDIT_DEVELOPER_URL;
            }
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_DATA_OPTIONS) {
        if (pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_PAGE_UP)) {
            ui->data_clear_confirmation = 0;
            ui->data_options_selection =
                (ui->data_options_selection
                 + UI_DATA_OPTIONS_ITEM_COUNT - 1u)
                % UI_DATA_OPTIONS_ITEM_COUNT;
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            ui->data_clear_confirmation = 0;
            ui->data_options_selection =
                (ui->data_options_selection + 1u)
                % UI_DATA_OPTIONS_ITEM_COUNT;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            if (ui->data_clear_confirmation != 0)
                ui->data_clear_confirmation = 0;
            else
                ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTION_ITEMS);
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT
                              | PSP_UI_BUTTON_CONFIRM)) {
            int direction = (pressed & PSP_UI_BUTTON_LEFT) ? -1 : 1;
            if (ui->data_options_selection == 0) {
                static const unsigned cache_sizes[] = {
                    256, 512, 1024, 2048, 4096
                };
                int selected = 1;
                for (size_t i = 0;
                     i < sizeof(cache_sizes) / sizeof(cache_sizes[0]);
                     i++) {
                    if (cache_sizes[i] == ui->live_cache_kib)
                        selected = (int) i;
                }
                int count =
                    (int) (sizeof(cache_sizes) / sizeof(cache_sizes[0]));
                selected = (selected + count + direction) % count;
                ui->live_cache_kib = cache_sizes[selected];
                intent.setting.id = PSP_UI_SETTING_LIVE_CACHE_KIB;
                intent.setting.value.unsigned_value = ui->live_cache_kib;
            } else if (ui->data_options_selection == 1) {
                static const unsigned cache_sizes[] = {0, 1, 2, 4};
                int selected = 0;
                for (size_t i = 0;
                     i < sizeof(cache_sizes) / sizeof(cache_sizes[0]);
                     i++) {
                    if (cache_sizes[i] == ui->persistent_cache_mb)
                        selected = (int) i;
                }
                int count =
                    (int) (sizeof(cache_sizes) / sizeof(cache_sizes[0]));
                selected = (selected + count + direction) % count;
                ui->persistent_cache_mb = cache_sizes[selected];
                intent.setting.id = PSP_UI_SETTING_PERSISTENT_CACHE_MB;
                intent.setting.value.unsigned_value =
                    ui->persistent_cache_mb;
            } else if (ui->data_options_selection == 2) {
                ui->persist_local_storage =
                    !ui->persist_local_storage;
                intent.setting.id = PSP_UI_SETTING_PERSIST_LOCAL_STORAGE;
                intent.setting.value.boolean =
                    ui->persist_local_storage;
            } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
                uint8_t confirmation =
                    (uint8_t) (ui->data_options_selection + 1u);
                if (ui->data_clear_confirmation != confirmation) {
                    ui->data_clear_confirmation = confirmation;
                } else {
                    ui->data_clear_confirmation = 0;
                    if (ui->data_options_selection == 3)
                        intent.clear_cache_requested = true;
                    else if (ui->data_options_selection == 4)
                        intent.clear_cookies_requested = true;
                    else if (ui->data_options_selection == 5)
                        intent.clear_local_storage_requested = true;
                    else if (ui->data_options_selection == 6)
                        intent.clear_session_storage_requested = true;
                }
            }
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_OPTION_ITEMS) {
        if (pressed & (PSP_UI_BUTTON_PAGE_UP | PSP_UI_BUTTON_PAGE_DOWN)) {
            size_t group = ui_option_group_index(
                ui_option_id(ui->options_selection));
            group = (pressed & PSP_UI_BUTTON_PAGE_UP)
                ? (group + UI_SETTINGS_GROUP_COUNT - 1u)
                    % UI_SETTINGS_GROUP_COUNT
                : (group + 1u) % UI_SETTINGS_GROUP_COUNT;
            ui->options_group_selection = (uint8_t) group;
            if (group == 6u) {
                ui_open_child_overlay(
                    ui, PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);
                ui->experimental_options_selection = 0u;
                intent.voice_component_probe_requested = true;
            } else {
                ui->options_selection =
                    psp_ui_menu_option_first_in_group(group);
            }
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_UP) {
            ui->options_selection = ui_option_step_in_group(
                ui->options_selection, -1);
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_DOWN) {
            ui->options_selection = ui_option_step_in_group(
                ui->options_selection, 1);
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            if (ui_option_id(ui->options_selection)
                    == UI_OPTION_NETWORK_PROFILE) {
                intent.setting.id = PSP_UI_SETTING_NETWORK_PROFILE;
                /* Discard an uncommitted Left/Right preview. */
                intent.setting.value.unsigned_value = 3u;
            }
            ui_open_parent_overlay(ui, PSP_UI_SCREEN_OPTIONS);
            intent.visual_changed = true;
        } else if (pressed & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT
                              | PSP_UI_BUTTON_CONFIRM)) {
            int direction = (pressed & PSP_UI_BUTTON_LEFT) ? -1 : 1;
            switch (ui_option_id(ui->options_selection)) {
                case UI_OPTION_BROWSER_UI_SCALE:
                    ui->browser_ui_scale =
                        ui->browser_ui_scale >= 2u ? 1u : 2u;
                    intent.setting.id = PSP_UI_SETTING_BROWSER_UI_SCALE;
                    intent.setting.value.unsigned_value =
                        ui->browser_ui_scale;
                    break;
                case UI_OPTION_PAGE_FONT_PERCENT: {
                    static const unsigned sizes[] = {80, 100, 125, 150};
                    int selected = 1;
                    for (size_t i = 0;
                         i < sizeof(sizes) / sizeof(sizes[0]); i++)
                        if (sizes[i] == ui->page_font_percent)
                            selected = (int) i;
                    int count = (int) (sizeof(sizes) / sizeof(sizes[0]));
                    selected = (selected + count + direction) % count;
                    ui->page_font_percent = sizes[selected];
                    intent.setting.id = PSP_UI_SETTING_PAGE_FONT_PERCENT;
                    intent.setting.value.unsigned_value =
                        ui->page_font_percent;
                    break;
                }
                case UI_OPTION_READER_FONT:
                    ui->reader_font_serif = !ui->reader_font_serif;
                    intent.setting.id = PSP_UI_SETTING_READER_FONT;
                    intent.setting.value.reader_font =
                        ui->reader_font_serif
                            ? BROWSER_READER_FONT_SERIF
                            : BROWSER_READER_FONT_SANS;
                    break;
                case UI_OPTION_REMEMBER_READER_SCALE:
                    ui->remember_reader_site_scale =
                        !ui->remember_reader_site_scale;
                    intent.setting.id =
                        PSP_UI_SETTING_REMEMBER_READER_SITE_SCALE;
                    intent.setting.value.boolean =
                        ui->remember_reader_site_scale;
                    break;
                case UI_OPTION_READER_AUTO_MODE:
                    ui->reader_auto_mode = !ui->reader_auto_mode;
                    intent.setting.id = PSP_UI_SETTING_READER_AUTO_MODE;
                    intent.setting.value.boolean = ui->reader_auto_mode;
                    break;
                case UI_OPTION_CUSTOM_HOMEPAGE:
                    ui->custom_homepage_enabled =
                        !ui->custom_homepage_enabled;
                    intent.setting.id = PSP_UI_SETTING_CUSTOM_HOMEPAGE;
                    intent.setting.value.boolean =
                        ui->custom_homepage_enabled;
                    break;
                case UI_OPTION_HISTORY:
                    ui->history_enabled = !ui->history_enabled;
                    intent.setting.id = PSP_UI_SETTING_HISTORY;
                    intent.setting.value.boolean = ui->history_enabled;
                    break;
                case UI_OPTION_RESTORE_LAST_PAGE:
                    ui->restore_last_page = !ui->restore_last_page;
                    intent.setting.id = PSP_UI_SETTING_RESTORE_LAST_PAGE;
                    intent.setting.value.boolean = ui->restore_last_page;
                    break;
                case UI_OPTION_TAB_HIBERNATION:
                    ui->tab_hibernation_enabled =
                        !ui->tab_hibernation_enabled;
                    intent.setting.id = PSP_UI_SETTING_TAB_HIBERNATION;
                    intent.setting.value.boolean =
                        ui->tab_hibernation_enabled;
                    break;
                case UI_OPTION_EXPERIMENTAL:
                    ui_open_child_overlay(
                        ui, PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS);
                    ui->experimental_options_selection = 0;
                    intent.voice_component_probe_requested = true;
                    break;
                case UI_OPTION_ANALOG_CURSOR:
                    ui->analog_cursor_enabled =
                        !ui->analog_cursor_enabled;
                    ui->cursor_visible = false;
                    intent.setting.id = PSP_UI_SETTING_ANALOG_CURSOR;
                    intent.setting.value.boolean =
                        ui->analog_cursor_enabled;
                    break;
                case UI_OPTION_JAVASCRIPT:
                    ui->javascript_enabled = !ui->javascript_enabled;
                    intent.setting.id = PSP_UI_SETTING_JAVASCRIPT;
                    intent.setting.value.boolean = ui->javascript_enabled;
                    break;
                case UI_OPTION_SITE_JAVASCRIPT:
                    ui->site_javascript_enabled =
                        !ui->site_javascript_enabled;
                    intent.setting.id = PSP_UI_SETTING_SITE_JAVASCRIPT;
                    intent.setting.value.boolean =
                        ui->site_javascript_enabled;
                    break;
                case UI_OPTION_TEXT_ENTRY:
                    ui->danzeff_text_input = !ui->danzeff_text_input;
                    intent.setting.id = PSP_UI_SETTING_TEXT_ENTRY_MODE;
                    intent.setting.value.text_entry_mode =
                        ui->danzeff_text_input
                            ? BROWSER_TEXT_ENTRY_DANZEFF
                            : BROWSER_TEXT_ENTRY_OSK;
                    break;
                case UI_OPTION_GAMEPAD_FACE_MAPPING:
                    ui->gamepad_circle_primary =
                        !ui->gamepad_circle_primary;
                    intent.setting.id =
                        PSP_UI_SETTING_GAMEPAD_FACE_MAPPING;
                    intent.setting.value.gamepad_face_mapping =
                        ui->gamepad_circle_primary
                            ? TILEFINCH_GAMEPAD_FACE_O_PRIMARY
                            : TILEFINCH_GAMEPAD_FACE_X_PRIMARY;
                    break;
                case UI_OPTION_SEARCH_ENGINE: {
                    int selected = (int) ui->search_engine;
                    selected = (selected + 3 + direction) % 3;
                    ui->search_engine = (BrowserSearchEngine) selected;
                    intent.setting.id = PSP_UI_SETTING_SEARCH_ENGINE;
                    intent.setting.value.search_engine = ui->search_engine;
                    break;
                }
                case UI_OPTION_COLOR_MODE: {
                    int selected = (int) ui->color_mode;
                    selected = (selected + 3 + direction) % 3;
                    ui->color_mode = (BrowserColorMode) selected;
                    intent.setting.id = PSP_UI_SETTING_COLOR_MODE;
                    intent.setting.value.color_mode = ui->color_mode;
                    break;
                }
                case UI_OPTION_CHROME_THEME: {
                    if (pressed & PSP_UI_BUTTON_CONFIRM) {
                        ui_open_child_overlay(
                            ui, PSP_UI_SCREEN_THEME_OPTIONS);
                        ui->data_options_selection = ui->chrome_theme
                                < BROWSER_CHROME_THEME_CUSTOM
                            ? ui->chrome_theme : BROWSER_CHROME_THEME_CUSTOM;
                        intent.theme_catalog_probe_requested = true;
                    } else {
                        ui->chrome_theme = ui_chrome_theme_step(
                            ui->chrome_theme, direction);
                        intent.setting.id = PSP_UI_SETTING_CHROME_THEME;
                        intent.setting.value.chrome_theme =
                            (BrowserChromeTheme) ui->chrome_theme;
                    }
                    break;
                }
                case UI_OPTION_LANGUAGE_AND_EMOJI:
                    ui_open_child_overlay(
                        ui, PSP_UI_SCREEN_GLYPH_OPTIONS);
                    ui->glyph_options_selection = 0;
                    intent.glyph_component_probe_requested = true;
                    break;
                case UI_OPTION_VIDEO_SCALING:
                    ui->video_scaling_sharp = !ui->video_scaling_sharp;
                    intent.setting.id = PSP_UI_SETTING_VIDEO_SCALING;
                    intent.setting.value.video_scaling =
                        ui->video_scaling_sharp
                            ? BROWSER_VIDEO_SCALING_SHARP
                            : BROWSER_VIDEO_SCALING_SMOOTH;
                    break;
                case UI_OPTION_YOUTUBE_QUALITY:
                    ui->youtube_240p = !ui->youtube_240p;
                    intent.setting.id = PSP_UI_SETTING_YOUTUBE_QUALITY;
                    intent.setting.value.youtube_quality =
                        ui->youtube_240p
                            ? BROWSER_YOUTUBE_QUALITY_240P
                            : BROWSER_YOUTUBE_QUALITY_360P;
                    break;
                case UI_OPTION_VIDEO_LANGUAGE: {
                    ui_open_child_overlay(
                        ui, PSP_UI_SCREEN_VIDEO_LANGUAGE_OPTIONS);
                    ui->data_options_selection = 0u;
                    break;
                }
                case UI_OPTION_YOUTUBE_AUDIO_ONLY:
                    ui->youtube_audio_only = !ui->youtube_audio_only;
                    intent.setting.id = PSP_UI_SETTING_YOUTUBE_AUDIO_ONLY;
                    intent.setting.value.boolean = ui->youtube_audio_only;
                    break;
                case UI_OPTION_YOUTUBE_RESULTS:
                    ui->youtube_compact_results =
                        !ui->youtube_compact_results;
                    intent.setting.id =
                        PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS;
                    intent.setting.value.boolean =
                        ui->youtube_compact_results;
                    break;
                case UI_OPTION_VIDEO_STARTUP_BUFFERING:
                    ui->video_startup_buffering =
                        !ui->video_startup_buffering;
                    intent.setting.id =
                        PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING;
                    intent.setting.value.boolean =
                        ui->video_startup_buffering;
                    break;
                case UI_OPTION_RESUME_DOWNLOADS:
                    ui->resume_offline_downloads =
                        !ui->resume_offline_downloads;
                    intent.setting.id =
                        PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS;
                    intent.setting.value.boolean =
                        ui->resume_offline_downloads;
                    break;
                case UI_OPTION_CONTENT_BLOCKER: {
                    int selected = (int) ui->content_blocker_mode;
                    selected = (selected + 3 + direction) % 3;
                    ui->content_blocker_mode = (uint8_t) selected;
                    if (ui->content_blocker_mode == CONTENT_BLOCKER_OFF)
                        ui->content_blocker_site_allowed = false;
                    intent.setting.id = PSP_UI_SETTING_CONTENT_BLOCKER_MODE;
                    intent.setting.value.content_blocker_mode =
                        (ContentBlockerMode) ui->content_blocker_mode;
                    break;
                }
                case UI_OPTION_COSMETIC_HIDING:
                    ui->content_blocker_cosmetic_hiding =
                        !ui->content_blocker_cosmetic_hiding;
                    intent.setting.id =
                        PSP_UI_SETTING_CONTENT_BLOCKER_COSMETIC_HIDING;
                    intent.setting.value.boolean =
                        ui->content_blocker_cosmetic_hiding;
                    break;
                case UI_OPTION_COOKIE_BANNERS:
                    if (ui_content_blocker_site_available(ui)) {
                        ui->cookie_banner_hidden =
                            !ui->cookie_banner_hidden;
                        intent.setting.id =
                            PSP_UI_SETTING_COOKIE_BANNER_HIDDEN;
                        intent.setting.value.boolean =
                            ui->cookie_banner_hidden;
                    }
                    break;
                case UI_OPTION_ALLOW_SITE:
                    if (ui->content_blocker_mode != CONTENT_BLOCKER_OFF
                        && ui_content_blocker_site_available(ui)) {
                        ui->content_blocker_site_allowed =
                            !ui->content_blocker_site_allowed;
                        intent.setting.id =
                            PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED;
                        intent.setting.value.boolean =
                            ui->content_blocker_site_allowed;
                    }
                    break;
                case UI_OPTION_LOAD_ALLOWLIST:
                    if (pressed & PSP_UI_BUTTON_CONFIRM)
                        intent.load_content_blocker_allowlist_requested = true;
                    break;
                case UI_OPTION_SITE_DATA_ALLOWED:
                    ui->site_data_allowed = !ui->site_data_allowed;
                    intent.setting.id = PSP_UI_SETTING_SITE_DATA_ALLOWED;
                    intent.setting.value.boolean = ui->site_data_allowed;
                    break;
                case UI_OPTION_TLS_SESSION_PERSISTENCE:
                    ui->tls_session_persistence =
                        !ui->tls_session_persistence;
                    intent.setting.id =
                        PSP_UI_SETTING_TLS_SESSION_PERSISTENCE;
                    intent.setting.value.boolean =
                        ui->tls_session_persistence;
                    break;
                case UI_OPTION_MIXED_CONTENT_SITE:
                    ui->mixed_content_site_allowed =
                        !ui->mixed_content_site_allowed;
                    intent.setting.id = PSP_UI_SETTING_MIXED_CONTENT_SITE;
                    intent.setting.value.boolean =
                        ui->mixed_content_site_allowed;
                    break;
                case UI_OPTION_THIRD_PARTY_COOKIES_SITE:
                    ui->third_party_cookie_site_allowed =
                        !ui->third_party_cookie_site_allowed;
                    intent.setting.id =
                        PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE;
                    intent.setting.value.boolean =
                        ui->third_party_cookie_site_allowed;
                    break;
                case UI_OPTION_NETWORK_PROFILE:
                    intent.setting.id = PSP_UI_SETTING_NETWORK_PROFILE;
                    /* Left/Right preview; X commits exactly one boot-config
                       write. Circle emits operation 3 above and restores the
                       saved value. */
                    intent.setting.value.unsigned_value =
                        (pressed & PSP_UI_BUTTON_CONFIRM)
                            ? 2u : (direction < 0 ? 0u : 1u);
                    break;
                case UI_OPTION_DIAGNOSTIC_QR:
                    if (pressed & PSP_UI_BUTTON_CONFIRM) {
                        ui->status[0] = '\0';
                        ui->toast_frames = 0u;
                        ui_open_child_overlay(
                            ui, PSP_UI_SCREEN_DIAGNOSTIC_QR);
                    }
                    break;
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
                case UI_OPTION_POWER_TEST:
                    if (pressed & PSP_UI_BUTTON_CONFIRM) {
                        ui->screen = PSP_UI_SCREEN_PAGE;
                        intent.action = PSP_UI_ACTION_POWER_TEST;
                    }
                    break;
                case UI_OPTION_MEDIA_TEST:
                    if (pressed & PSP_UI_BUTTON_CONFIRM) {
                        ui->screen = PSP_UI_SCREEN_PAGE;
                        intent.action = PSP_UI_ACTION_MEDIA_TEST;
                    }
                    break;
#endif
                case UI_OPTION_UPDATE_CHECK:
                    ui->update_check_enabled = !ui->update_check_enabled;
                    intent.setting.id = PSP_UI_SETTING_UPDATE_CHECK;
                    intent.setting.value.boolean =
                        ui->update_check_enabled;
                    break;
                case UI_OPTION_UPDATE:
                    ui_open_child_overlay(ui, PSP_UI_SCREEN_UPDATE);
                    break;
                case UI_OPTION_SITE_DATA:
                    ui_open_child_overlay(ui, PSP_UI_SCREEN_DATA_OPTIONS);
                    ui->data_options_selection = 0;
                    break;
            }
            intent.visual_changed = true;
        }
        return intent;
    }

    if (ui->screen == PSP_UI_SCREEN_FIND) {
        const uint32_t find_direction_mask =
            PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
            | PSP_UI_BUTTON_PAGE_UP | PSP_UI_BUTTON_PAGE_DOWN;
        pressed = repeat_directions(
            ui, pressed, input->held, find_direction_mask, elapsed_ms);
        int analog_delta = analog_scroll_delta(ui, input);
        if (analog_delta != 0) {
            intent.scroll_delta = analog_delta;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_PAGE_UP) {
            intent.action = PSP_UI_ACTION_PAGE_UP;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_PAGE_DOWN) {
            intent.action = PSP_UI_ACTION_PAGE_DOWN;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_UP) {
            intent.action = PSP_UI_ACTION_FIND_PREVIOUS;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_DOWN) {
            intent.action = PSP_UI_ACTION_FIND_NEXT;
            intent.visual_changed = true;
        } else if (pressed
                   & (PSP_UI_BUTTON_CONFIRM | PSP_UI_BUTTON_ADDRESS)) {
            psp_ui_clear_find(ui);
            intent.action = PSP_UI_ACTION_FIND_EDIT;
            intent.visual_changed = true;
        } else if (pressed & PSP_UI_BUTTON_CANCEL) {
            psp_ui_clear_find(ui);
            intent.action = PSP_UI_ACTION_FIND_CLOSE;
            intent.visual_changed = true;
        }
        return intent;
    }

    /* A native surface has no page chrome to retract, so Triangle is inert
       there rather than silently dropping the surface. */
    if ((pressed & PSP_UI_BUTTON_TOOLBAR)
        && !psp_ui_screen_is_native_surface(ui->screen)) {
        ui->cursor_visible = false;
        ui->chrome_visible = !ui->chrome_visible;
        ui->screen = PSP_UI_SCREEN_PAGE;
        intent.visual_changed = true;
        return intent;
    }
    if (pressed & PSP_UI_BUTTON_ADDRESS) {
        bool submit_focused = ui->screen == PSP_UI_SCREEN_PAGE
            && ui->focus_editable;
        ui->cursor_visible = false;
        ui_close_overlay(ui);
        ui->chrome_visible = true;
        intent.action = submit_focused
            ? PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT
            : PSP_UI_ACTION_OPEN_ADDRESS;
        intent.visual_changed = true;
        return intent;
    }

    const uint32_t direction_mask =
        PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
        | PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT;
    uint32_t unrepeated_pressed = pressed;
    pressed = repeat_directions(
        ui, pressed, input->held, direction_mask, elapsed_ms);
    if (pressed != unrepeated_pressed) {
        directional = pressed & direction_mask;
        if (ui->cursor_visible) intent.visual_changed = true;
        ui->cursor_visible = false;
    }

    bool cursor_moved = false;
    if (ui->analog_cursor_enabled && directional == 0) {
        int dx = analog_cursor_delta(input->analog_x, elapsed_ms);
        int dy = analog_cursor_delta(input->analog_y, elapsed_ms);
        if (dx != 0 || dy != 0) {
            int old_x = ui->cursor_x_milli / 1000;
            int old_y = ui->cursor_y_milli / 1000;
            ui->cursor_x_milli += dx;
            ui->cursor_y_milli += dy;
            if (ui->cursor_x_milli < 0) ui->cursor_x_milli = 0;
            if (ui->cursor_x_milli > UI_CURSOR_MAX_X)
                ui->cursor_x_milli = UI_CURSOR_MAX_X;
            if (ui->cursor_y_milli < 0) ui->cursor_y_milli = 0;
            if (ui->cursor_y_milli > UI_CURSOR_MAX_Y)
                ui->cursor_y_milli = UI_CURSOR_MAX_Y;
            int x = ui->cursor_x_milli / 1000;
            int y = ui->cursor_y_milli / 1000;
            cursor_moved = x != old_x || y != old_y;
            ui->cursor_visible = true;
            /* Movement feedback is immediate. Fade-out remains useful, but
               fading in made the first acknowledged sample nearly invisible
               on the physical LCD. */
            ui->cursor_fade = PSP_THEME_MOTION_FOCUS_FRAMES;
            ui->cursor_idle_ms = 0;
            intent.visual_changed = true;
            if (cursor_moved && intent.pointer_phase == PSP_UI_POINTER_NONE) {
                intent.pointer_phase = PSP_UI_POINTER_MOVE;
                intent.pointer_x = x;
                intent.pointer_y = y;
            }
            bool edge_scroll =
                (dy < 0 && ui->cursor_y_milli == 0)
                || (dy > 0 && ui->cursor_y_milli == UI_CURSOR_MAX_Y);
            if (edge_scroll) {
                intent.scroll_delta = analog_scroll_delta(ui, input);
            } else {
                ui->analog_hold_ms = 0;
                ui->analog_scroll_remainder = 0;
                ui->analog_scroll_direction = 0;
            }
        } else if (!ui->cursor_pointer_down && ui->cursor_visible) {
            unsigned remaining = ui->cursor_idle_ms < UI_CURSOR_HIDE_MS
                ? UI_CURSOR_HIDE_MS - ui->cursor_idle_ms : 0;
            ui->cursor_idle_ms = (uint16_t) (
                ui->cursor_idle_ms
                + (elapsed_ms < remaining ? elapsed_ms : remaining));
            if (ui->cursor_idle_ms >= UI_CURSOR_HIDE_MS) {
                ui->cursor_visible = false;
                intent.visual_changed = true;
            }
        }
    }

    /*
     * Native surfaces share everything above this point -- cursor movement,
     * direction repeat, chrome activity -- and nothing below it: page
     * scrolling, pointer events, and history verbs belong to a document.
     */
    if (psp_ui_screen_is_native_surface(ui->screen)) {
        intent.scroll_delta = 0;
        bool cursor_activate = ui->analog_cursor_enabled
            && ui->cursor_visible
            && intent.pointer_phase != PSP_UI_POINTER_CANCEL
            && (pressed & PSP_UI_BUTTON_CONFIRM) != 0;
        if (cursor_handoff || cursor_activate) {
            int at_x = cursor_activate
                ? ui->cursor_x_milli / 1000 : handoff_x;
            int at_y = cursor_activate
                ? ui->cursor_y_milli / 1000 : handoff_y;
            if (ui->screen == PSP_UI_SCREEN_HOME) {
                size_t nearest = ui_home_nearest_row(ui, at_x, at_y);
                if (nearest != ui->home_selection) {
                    ui->home_selection = (uint8_t) nearest;
                    ui->focus_settle_frames = PSP_THEME_MOTION_FOCUS_FRAMES;
                }
            } else {
                size_t nearest = ui_collections_nearest_row(ui, at_y);
                if (nearest != ui->collections_selection) {
                    ui->collections_selection = (uint8_t) nearest;
                    ui->collections_delete_confirmation = 0;
                    ui->focus_settle_frames = PSP_THEME_MOTION_FOCUS_FRAMES;
                }
            }
            intent.visual_changed = true;
            if (!cursor_activate) return intent;
            /* X with the cursor visible activates what is under it. */
            pressed = PSP_UI_BUTTON_CONFIRM;
        }
        ui_update_native_surface(ui, pressed, &intent);
        return intent;
    }

    bool pointer_confirm = ui->analog_cursor_enabled
        && ui->cursor_visible
        && intent.pointer_phase != PSP_UI_POINTER_CANCEL
        && (pressed & PSP_UI_BUTTON_CONFIRM) != 0;
    if (pointer_confirm) {
        ui->cursor_pointer_down = true;
        intent.pointer_phase = PSP_UI_POINTER_DOWN;
        intent.pointer_x = ui->cursor_x_milli / 1000;
        intent.pointer_y = ui->cursor_y_milli / 1000;
    }

    if (pressed & PSP_UI_BUTTON_CANCEL) {
        intent.action = PSP_UI_ACTION_BACK;
    } else if (pressed & PSP_UI_BUTTON_RELOAD) {
        intent.action = PSP_UI_ACTION_RELOAD;
    } else if ((pressed & PSP_UI_BUTTON_CONFIRM) && !pointer_confirm) {
        intent.action = PSP_UI_ACTION_ACTIVATE;
    } else if (cursor_handoff) {
        intent.action = PSP_UI_ACTION_FOCUS_AT;
        intent.pointer_x = handoff_x;
        intent.pointer_y = handoff_y;
    } else if (pressed & PSP_UI_BUTTON_UP) {
        intent.action = PSP_UI_ACTION_FOCUS_UP;
    } else if (pressed & PSP_UI_BUTTON_DOWN) {
        intent.action = PSP_UI_ACTION_FOCUS_DOWN;
    } else if (pressed & PSP_UI_BUTTON_LEFT) {
        intent.action = PSP_UI_ACTION_FOCUS_LEFT;
    } else if (pressed & PSP_UI_BUTTON_RIGHT) {
        intent.action = PSP_UI_ACTION_FOCUS_RIGHT;
    } else if (pressed & PSP_UI_BUTTON_PAGE_UP) {
        intent.action = PSP_UI_ACTION_PAGE_UP;
    } else if (pressed & PSP_UI_BUTTON_PAGE_DOWN) {
        intent.action = PSP_UI_ACTION_PAGE_DOWN;
    }

    if (!ui->analog_cursor_enabled) {
        bool was_scrolling = ui->analog_scroll_direction != 0;
        intent.scroll_delta = analog_scroll_delta(ui, input);
        intent.scroll_settle =
            was_scrolling && ui->analog_scroll_direction == 0;
    }
    if (intent.action != PSP_UI_ACTION_NONE || intent.scroll_delta != 0
        || intent.pointer_phase != PSP_UI_POINTER_NONE) {
        intent.visual_changed = true;
    }
    return intent;
}

bool psp_ui_motion_pending(const PspUiState *ui)
{
    return ui != NULL
        && (ui->overlay_animation_frames != 0
            || ui->toast_entry_frames != 0
            || ui->focus_settle_frames != 0);
}

bool psp_ui_intent_has_predispatch_visual(const PspUiIntent *intent)
{
    if (intent == NULL || !intent->visual_changed) return false;
    switch (intent->action) {
        case PSP_UI_ACTION_FOCUS_PREVIOUS:
        case PSP_UI_ACTION_FOCUS_NEXT:
        case PSP_UI_ACTION_FOCUS_UP:
        case PSP_UI_ACTION_FOCUS_DOWN:
        case PSP_UI_ACTION_FOCUS_LEFT:
        case PSP_UI_ACTION_FOCUS_RIGHT:
        case PSP_UI_ACTION_FOCUS_AT:
        case PSP_UI_ACTION_PAGE_UP:
        case PSP_UI_ACTION_PAGE_DOWN:
        case PSP_UI_ACTION_SCROLL_TOP:
        case PSP_UI_ACTION_SCROLL_BOTTOM:
        case PSP_UI_ACTION_FIND_PREVIOUS:
        case PSP_UI_ACTION_FIND_NEXT:
        case PSP_UI_ACTION_FIND_CLOSE:
            return false;
        case PSP_UI_ACTION_NONE:
        case PSP_UI_ACTION_ACTIVATE:
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
        case PSP_UI_ACTION_BACK:
        case PSP_UI_ACTION_FORWARD:
        case PSP_UI_ACTION_RELOAD:
        case PSP_UI_ACTION_TOGGLE_READER:
        case PSP_UI_ACTION_TOGGLE_BASIC:
        case PSP_UI_ACTION_TOGGLE_READER_SITE:
        case PSP_UI_ACTION_OPEN_ADDRESS:
        case PSP_UI_ACTION_OPEN_VOICE_ADDRESS:
        case PSP_UI_ACTION_OPEN_FIND:
        case PSP_UI_ACTION_FIND_EDIT:
        case PSP_UI_ACTION_VOICE_FOCUSED_TEXT:
        case PSP_UI_ACTION_HOME:
        case PSP_UI_ACTION_SAVE_FOR_LATER:
        case PSP_UI_ACTION_SHOW_OFFLINE:
        case PSP_UI_ACTION_SHOW_DOWNLOADS:
        case PSP_UI_ACTION_SHOW_SCREENSHOTS:
        case PSP_UI_ACTION_TOGGLE_BOOKMARK:
        case PSP_UI_ACTION_SWITCH_TAB:
        case PSP_UI_ACTION_NEW_TAB:
        case PSP_UI_ACTION_CLOSE_TAB:
        case PSP_UI_ACTION_SHOW_BOOKMARKS:
        case PSP_UI_ACTION_SHOW_HOMEPAGE:
        case PSP_UI_ACTION_SHOW_HISTORY:
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL:
        case PSP_UI_ACTION_SET_VIDEO_DECODER:
        case PSP_UI_ACTION_EXIT:
        default:
            return true;
    }
}

bool psp_ui_intent_predispatch_is_complete(const PspUiIntent *intent)
{
    return psp_ui_intent_has_predispatch_visual(intent)
        && intent->action == PSP_UI_ACTION_NONE
        && intent->scroll_delta == 0
        && intent->pointer_phase == PSP_UI_POINTER_NONE
        && intent->setting.id == PSP_UI_SETTING_NONE
        && !intent->clear_cache_requested
        && !intent->clear_cookies_requested
        && !intent->clear_local_storage_requested
        && !intent->clear_session_storage_requested
        && !intent->update_primary_requested
        && !intent->update_cancel_requested;
}

/*
 * One legend entry on the bottom hint bar. The accent is spent on the button
 * glyph alone; the word it names is muted, exactly as on the native
 * surfaces' hint line. The filled per-button chips (each in its own colour)
 * went with the cold era -- four saturated pills on a 21px bar were the
 * loudest thing on screen and none of the colours meant anything.
 */
static void draw_button_legend(uint16_t *pixels, int width, int height,
                               int stride, int x, int y, const char *button,
                               const char *label, uint16_t accent,
                               uint16_t text)
{
    (void) text;
    int button_width = (int) strlen(button) * 6 + 6;
    draw_text(pixels, width, height, stride, x + 3, y + 3, button, 8,
              accent, 1);
    /* Without the chip the glyph and its word need the gap the chip's
       padding used to provide, or "START Search" reads as one word. */
    draw_text(pixels, width, height, stride,
              x + button_width + PSP_THEME_SPACE_M, y + 3,
              label, 12, PSP_THEME_TEXT_MUTED, 1);
}

static void draw_focus(const PspUiState *ui, uint16_t *pixels, int width,
                       int height, int stride, uint16_t accent)
{
    if (!ui->has_focus) return;
    UiRect focus = {
        ui->focus_x - 4, ui->focus_y - 4,
        ui->focus_width + 8, ui->focus_height + 8
    };
    if (focus.x + focus.width < 0 || focus.y + focus.height < 0
        || focus.x >= width || focus.y >= height) return;
    outline_rect(pixels, width, height, stride, focus,
                 PSP_THEME_GROUND, 4);
    outline_rect(pixels, width, height, stride,
                 (UiRect) {focus.x + 1, focus.y + 1,
                           focus.width - 2, focus.height - 2},
                 accent, 2);
}

static void draw_cursor(const PspUiState *ui, uint16_t *pixels, int width,
                        int height, int stride, uint16_t accent)
{
    if (!ui->analog_cursor_enabled || ui->cursor_fade == 0u
        || ui->cursor_shape == PSP_UI_CURSOR_HIDDEN) return;
    int x = ui->cursor_x_milli / 1000;
    int y = ui->cursor_y_milli / 1000;
    uint16_t shadow = PSP_THEME_GROUND;
    /* The fade is the fill's own opacity ladder: three steps between gone
       and present, no second code path and no per-pixel arithmetic beyond
       the blend the primitive already does. */
    unsigned ink = ui->cursor_fade + 1u;
    if (ui->cursor_shape == PSP_UI_CURSOR_POINTER) {
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x, y, 2, 11}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x + 2, y + 2, 2, 8}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x + 4, y + 4, 2, 6}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x + 1, y + 1, 1, 9}, accent, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x + 3, y + 3, 1, 7}, accent, ink);
        return;
    }
    if (ui->cursor_shape == PSP_UI_CURSOR_TEXT) {
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x - 3, y - 6, 7, 2}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x - 1, y - 5, 3, 11}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x - 3, y + 5, 7, 2}, shadow, ink);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {x, y - 5, 1, 11}, accent, ink);
        return;
    }
    if (ui->cursor_shape == PSP_UI_CURSOR_RESIZE_HORIZONTAL
        || ui->cursor_shape == PSP_UI_CURSOR_RESIZE_VERTICAL) {
        bool horizontal =
            ui->cursor_shape == PSP_UI_CURSOR_RESIZE_HORIZONTAL;
        fill_rect(pixels, width, height, stride,
                  horizontal ? (UiRect) {x - 6, y - 1, 13, 3}
                             : (UiRect) {x - 1, y - 6, 3, 13},
                  shadow, ink);
        fill_rect(pixels, width, height, stride,
                  horizontal ? (UiRect) {x - 5, y, 11, 1}
                             : (UiRect) {x, y - 5, 1, 11},
                  accent, ink);
        /* Arrowheads keep the resize cursor from reading as two parallel
           bars when the dark outline and accent centre line separate on a
           low-response LCD. */
        if (horizontal) {
            fill_rect(pixels, width, height, stride,
                      (UiRect) {x - 5, y - 2, 1, 5}, accent, ink);
            fill_rect(pixels, width, height, stride,
                      (UiRect) {x + 5, y - 2, 1, 5}, accent, ink);
        } else {
            fill_rect(pixels, width, height, stride,
                      (UiRect) {x - 2, y - 5, 5, 1}, accent, ink);
            fill_rect(pixels, width, height, stride,
                      (UiRect) {x - 2, y + 5, 5, 1}, accent, ink);
        }
        return;
    }
    fill_rect(pixels, width, height, stride,
              (UiRect) {x - 6, y - 1, 13, 3}, shadow, ink);
    fill_rect(pixels, width, height, stride,
              (UiRect) {x - 1, y - 6, 3, 13}, shadow, ink);
    fill_rect(pixels, width, height, stride,
              (UiRect) {x - 5, y, 11, 1}, accent, ink);
    fill_rect(pixels, width, height, stride,
              (UiRect) {x, y - 5, 1, 11}, accent, ink);
}

static void draw_scrollbar(const PspUiState *ui, uint16_t *pixels, int width,
                           int height, int stride, uint16_t accent)
{
    if (ui->maximum_scroll_y <= 0
        || ui->page_scrollbar_width == 2u) return;
    int track_top = ui->chrome_visible ? UI_TOP_HEIGHT + 4 : 4;
    int track_bottom = ui->chrome_visible
        ? height - browser_bottom_height(ui) - 4 : height - 4;
    int track_height = track_bottom - track_top;
    if (track_height < 16) return;
    int viewport_estimate = ui->maximum_scroll_y + height;
    int thumb_height = height * track_height / viewport_estimate;
    if (thumb_height < 14) thumb_height = 14;
    if (thumb_height > track_height) thumb_height = track_height;
    int travel = track_height - thumb_height;
    int thumb_y = track_top;
    if (travel > 0) {
        thumb_y += ui->scroll_y * travel / ui->maximum_scroll_y;
    }
    int track_width = ui->page_scrollbar_width == 1u ? 2 : 4;
    int thumb_width = ui->page_scrollbar_width == 1u ? 3 : 5;
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) {
                        width - track_width - 2, track_top,
                        track_width, track_height
                    },
                    1, PSP_THEME_SURFACE, 2);
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) {
                        width - thumb_width - 2, thumb_y,
                        thumb_width, thumb_height
                    },
                    2, accent, 4);
}

static void draw_top_bar(const PspUiState *ui, uint16_t *pixels, int width,
                         int height, int stride, uint16_t panel,
                         uint16_t accent, uint16_t text, uint16_t muted)
{
    int scale = browser_ui_scale(ui);
    (void) panel;
    (void) text;
    fill_rect(pixels, width, height, stride,
              (UiRect) { 0, 0, width, UI_TOP_HEIGHT },
              PSP_THEME_CHROME_BAR, 4);
    /*
     * The common case is this dark bar over a light page, so the bar has to
     * hold its own edge: a LINE hairline at the content edge, not an accent
     * rule that competes with the progress band right below it.
     */
    fill_rect(pixels, width, height, stride,
              (UiRect) { 0, UI_TOP_HEIGHT - 1, width, 1 },
              PSP_THEME_LINE, 4);

    uint16_t back_color =
        ui->can_go_back ? PSP_THEME_TEXT : PSP_THEME_TEXT_FAINT;
    uint16_t forward_color =
        ui->can_go_forward ? PSP_THEME_TEXT : PSP_THEME_TEXT_FAINT;
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) { 7, 7, 25, 25 }, PSP_THEME_RADIUS_CHIP,
                    PSP_THEME_SURFACE, 4);
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) { 37, 7, 25, 25 }, PSP_THEME_RADIUS_CHIP,
                    PSP_THEME_SURFACE, 4);
    draw_chevron(pixels, width, height, stride, 20, 19, -1, back_color);
    draw_chevron(pixels, width, height, stride, 49, 19, 1, forward_color);

    /* Same clock preference as the native surfaces; the address field
       yields whatever the formatted string actually needs. */
    /* Reload is a Square shortcut rather than a pointer target. Keep the
       device status at the trailing edge and spend the reclaimed chip width
       on the address, where it helps on every page. */
    int clock_right = width - 33;
    int clock_left = clock_right;
    if (device_status.valid) {
        clock_left = clock_right
            - chrome_text_width_bytes(
                  device_status.clock, strlen(device_status.clock), 1, false);
    }
    int address_right = device_status.valid
        ? clock_left - PSP_THEME_SPACE_S : width - 8;
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) { 68, 5, address_right - 68, 29 },
                    PSP_THEME_RADIUS_CARD, PSP_THEME_SURFACE, 4);
    /* Semantic, never the accent: secure is OK, a request in flight borrows
       the accent because it is the same "working" signal the progress band
       carries, and anything else is simply muted. */
    draw_lock(pixels, width, height, stride, 77, 12, ui->secure,
              ui->loading ? accent
                          : (ui->secure ? PSP_THEME_OK
                                        : PSP_THEME_TEXT_MUTED));
    char domain[128], detail[256];
    ui_url_presentation(
        ui, domain, sizeof(domain), detail, sizeof(detail));
    draw_text_with_font(
        pixels, width, height, stride, 91, scale == 2 ? 8 : 7,
        domain, scale == 2 ? 32 : 64, address_right - 5,
        PSP_THEME_TEXT, scale, NULL, true);
    if (scale == 1 && detail[0] != '\0')
        draw_text_with_font(
            pixels, width, height, stride, 91, 20, detail, 80,
            address_right - 5, PSP_THEME_TEXT_MUTED, 1, NULL, false);

    if (device_status.valid) {
        draw_text_right_aligned(pixels, width, height, stride,
                                clock_right, 14, device_status.clock,
                                sizeof(device_status.clock) - 1u,
                                muted, 1, false);
        UiRect battery = {width - 27, 13, 17, 9};
        outline_rect(pixels, width, height, stride, battery, muted, 1);
        fill_rect(pixels, width, height, stride,
                  (UiRect) {battery.x + battery.width, battery.y + 3, 2, 3},
                  muted, 4);
        int fill = 13 * (int) device_status.battery_percent / 100;
        uint16_t battery_color = device_status.battery_percent <= 15u
            ? PSP_THEME_WARN : accent;
        if (fill > 0)
            fill_rect(pixels, width, height, stride,
                      (UiRect) {battery.x + 2, battery.y + 2, fill, 5},
                      battery_color, 4);
        if (device_status.charging)
            draw_text_bold(pixels, width, height, stride,
                           battery.x + 5, battery.y - 1, "+", 1,
                           PSP_THEME_TEXT, 1);
    }

}

static void draw_bottom_bar(const PspUiState *ui, uint16_t *pixels, int width,
                            int height, int stride, uint16_t panel,
                            uint16_t accent, uint16_t text)
{
    int scale = browser_ui_scale(ui);
    int bottom_height = browser_bottom_height(ui);
    int top = height - bottom_height;
    (void) panel;
    (void) text;
    fill_rect(pixels, width, height, stride,
              (UiRect) { 0, top, width, bottom_height },
              PSP_THEME_HINT_BAR, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) { 0, top, width, 1 }, PSP_THEME_LINE, 4);
    if (ui->page_gamepad_capture) {
        draw_text_with_font(
            pixels, width, height, stride, 7,
            top + (scale == 2 ? 7 : 6),
            scale == 2 ? "PAGE CONTROLS  START+SELECT EXIT"
                       : "PAGE CONTROLS  HOLD START+SELECT TO EXIT",
            40,
            width - 7, accent, scale == 2 ? 2 : 1, NULL, true);
        return;
    }
    if (ui->captive_portal_active) {
        char domain[128], detail[256];
        ui_url_presentation(
            ui, domain, sizeof(domain), detail, sizeof(detail));
        char label[180];
        snprintf(label, sizeof(label), "WI-FI SIGN-IN: %s", domain);
        draw_text_with_font(
            pixels, width, height, stride, 7, top + (scale == 2 ? 7 : 6),
            label, 48, width - 92, accent, scale == 2 ? 2 : 1,
            NULL, true);
        draw_button_legend(
            pixels, width, height, stride, width - 83, top + 4,
            "O", "Cancel", accent, text);
        return;
    }
    if (scale == 2) {
        draw_text(pixels, width, height, stride, 7, top + 7,
                  ui->focus_editable
                      ? "X Edit O Back SQ Reload Start Enter"
                      : "X Open O Back SQ Reload Start Search",
                  38,
                  PSP_THEME_TEXT_MUTED, 2);
        return;
    }
    draw_button_legend(pixels, width, height, stride, 7, top + 4,
                       "X", "Open", accent, text);
    draw_button_legend(pixels, width, height, stride, 58, top + 4,
                       "O", "Back", accent, text);
    draw_button_legend(pixels, width, height, stride, 109, top + 4,
                       "TRI", "Bar", accent, text);
    draw_button_legend(pixels, width, height, stride, 166, top + 4,
                       "START", ui->focus_editable ? "Enter" : "Search",
                       accent, text);
    draw_button_legend(pixels, width, height, stride, 253, top + 4,
                       "Square", "Reload", accent, text);
    int percent = ui->maximum_scroll_y <= 0 ? 100
        : ui->scroll_y * 100 / ui->maximum_scroll_y;
    char progress[16];
    snprintf(progress, sizeof(progress), "%d%%", percent);
    draw_text(pixels, width, height, stride, width - 34, top + 7,
              progress, 5, PSP_THEME_TEXT_MUTED, 1);
    if (ui->page_requests_blocked != 0) {
        char blocked[12];
        if (ui->page_requests_blocked > 999u)
            snprintf(blocked, sizeof(blocked), "B999+");
        else
            snprintf(blocked, sizeof(blocked), "B%u",
                     (unsigned) ui->page_requests_blocked);
        draw_text(pixels, width, height, stride, width - 78, top + 7,
                  blocked, 6, accent, 1);
    }
}

/*
 * Page chrome is opaque and changes far less often than an animated canvas.
 * Rasterizing its proportional text and rounded controls for every frame
 * costs several milliseconds on Allegrex, even though the resulting rows
 * are byte-identical. Retain exactly one bounded top/bottom pair and copy it
 * into each rotating scanout buffer. The key stores the complete inputs used
 * by the painters rather than relying on a hash, so a collision can never
 * expose stale URL, security, or device state.
 *
 * This is application chrome, not page-owned storage. It is fixed at 65,280
 * bytes and does not grow with the document or number of canvases.
 */
typedef struct {
    PspUiThemePalette palette;
    UiDeviceStatus device;
    const FontFace *regular_font;
    const FontFace *bold_font;
    uint8_t scale;
    uint8_t can_go_back;
    uint8_t can_go_forward;
    uint8_t secure;
    uint8_t loading;
    char url[PSP_UI_URL_CAPACITY];
    char title[PSP_UI_TITLE_CAPACITY];
} UiTopBarCacheKey;

typedef struct {
    PspUiThemePalette palette;
    const FontFace *regular_font;
    const FontFace *bold_font;
    int32_t scroll_y;
    int32_t maximum_scroll_y;
    uint32_t page_requests_blocked;
    uint8_t scale;
    uint8_t page_gamepad_capture;
    uint8_t captive_portal_active;
    uint8_t focus_editable;
    char url[PSP_UI_URL_CAPACITY];
} UiBottomBarCacheKey;

typedef struct {
    bool top_valid;
    bool bottom_valid;
    UiTopBarCacheKey top_key;
    UiBottomBarCacheKey bottom_key;
    uint16_t top[UI_BROWSER_CHROME_CACHE_WIDTH * UI_TOP_HEIGHT];
    uint16_t bottom[UI_BROWSER_CHROME_CACHE_WIDTH
                    * UI_BROWSER_CHROME_CACHE_BOTTOM_HEIGHT];
} UiBrowserChromeCache;

static UiBrowserChromeCache browser_chrome_cache;

static void ui_copy_cached_rows(
    uint16_t *destination, int destination_stride,
    const uint16_t *source, int rows)
{
    for (int row = 0; row < rows; row++) {
        memcpy(destination + (size_t) row * (size_t) destination_stride,
               source + (size_t) row * UI_BROWSER_CHROME_CACHE_WIDTH,
               UI_BROWSER_CHROME_CACHE_WIDTH * sizeof(uint16_t));
    }
}

static void draw_cached_top_bar(
    const PspUiState *ui, uint16_t *pixels, int width, int height, int stride,
    uint16_t panel, uint16_t accent, uint16_t text, uint16_t muted)
{
    if (width != UI_BROWSER_CHROME_CACHE_WIDTH || height < UI_TOP_HEIGHT) {
        draw_top_bar(ui, pixels, width, height, stride,
                     panel, accent, text, muted);
        return;
    }
    UiTopBarCacheKey key = {0};
    key.palette = *psp_ui_theme_active_palette;
    key.device = device_status;
    key.regular_font = chrome_font_cache.faces[0];
    key.bold_font = chrome_font_cache.faces[1];
    key.scale = (uint8_t) browser_ui_scale(ui);
    key.can_go_back = ui->can_go_back;
    key.can_go_forward = ui->can_go_forward;
    key.secure = ui->secure;
    key.loading = ui->loading;
    copy_string(key.url, sizeof(key.url), ui->url);
    copy_string(key.title, sizeof(key.title), ui->title);
    if (!browser_chrome_cache.top_valid
        || memcmp(&key, &browser_chrome_cache.top_key, sizeof(key)) != 0) {
        draw_top_bar(
            ui, browser_chrome_cache.top,
            UI_BROWSER_CHROME_CACHE_WIDTH, UI_TOP_HEIGHT,
            UI_BROWSER_CHROME_CACHE_WIDTH, panel, accent, text, muted);
        browser_chrome_cache.top_key = key;
        browser_chrome_cache.top_valid = true;
    }
    ui_copy_cached_rows(
        pixels, stride, browser_chrome_cache.top, UI_TOP_HEIGHT);
}

static void draw_cached_bottom_bar(
    const PspUiState *ui, uint16_t *pixels, int width, int height, int stride,
    uint16_t panel, uint16_t accent, uint16_t text)
{
    int bottom_height = browser_bottom_height(ui);
    if (width != UI_BROWSER_CHROME_CACHE_WIDTH
        || bottom_height > UI_BROWSER_CHROME_CACHE_BOTTOM_HEIGHT
        || height < bottom_height) {
        draw_bottom_bar(ui, pixels, width, height, stride,
                        panel, accent, text);
        return;
    }
    UiBottomBarCacheKey key = {0};
    key.palette = *psp_ui_theme_active_palette;
    key.regular_font = chrome_font_cache.faces[0];
    key.bold_font = chrome_font_cache.faces[1];
    key.scroll_y = ui->scroll_y;
    key.maximum_scroll_y = ui->maximum_scroll_y;
    key.page_requests_blocked = ui->page_requests_blocked;
    key.scale = (uint8_t) browser_ui_scale(ui);
    key.page_gamepad_capture = ui->page_gamepad_capture;
    key.captive_portal_active = ui->captive_portal_active;
    key.focus_editable = ui->focus_editable;
    copy_string(key.url, sizeof(key.url), ui->url);
    if (!browser_chrome_cache.bottom_valid
        || memcmp(&key, &browser_chrome_cache.bottom_key, sizeof(key)) != 0) {
        draw_bottom_bar(
            ui, browser_chrome_cache.bottom,
            UI_BROWSER_CHROME_CACHE_WIDTH, bottom_height,
            UI_BROWSER_CHROME_CACHE_WIDTH, panel, accent, text);
        browser_chrome_cache.bottom_key = key;
        browser_chrome_cache.bottom_valid = true;
    }
    ui_copy_cached_rows(
        pixels + (size_t) (height - bottom_height) * (size_t) stride,
        stride, browser_chrome_cache.bottom, bottom_height);
}

static TILEFINCH_OUT_OF_LINE void draw_menu(
                      const PspUiState *ui, uint16_t *pixels, int width,
                      int height, int stride, uint16_t panel,
                      uint16_t accent, uint16_t text, uint16_t muted)
{
    static const char *items[PSP_UI_MENU_ITEM_COUNT] = {
        "Home", "Tabs", "Page tools", "Library", "Settings",
        "Help & diagnostics", "Exit to XMB"
    };
    /* Chrome menu text is always 15px (scale 2): it names browser controls,
       not page content, so it must not shrink with the page-zoom setting.
       The panel then matches the options/tabs overlays' top and height for a
       steady frame when switching between them; only its width stays
       narrower and centred, which is deliberate. */
    const int scale = 2;
    const int row_spacing = 23;
    const int row_height = 21;
    UiRect shadow = { 58, 10, width - 116, 252 };
    ui_apply_overlay_motion(ui, &shadow);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, shadow);
    draw_panel_rule(pixels, width, height, stride, shadow, shadow.y + 40);
    draw_panel_hint_bar(pixels, width, height, stride, shadow,
                        shadow.y + shadow.height - 18);
    draw_text_bold(pixels, width, height, stride,
                   shadow.x + 16, shadow.y + 14,
                   "Tilefinch", 24, text, scale);
    size_t first = 0;
    size_t end = first + UI_MENU_VISIBLE_ROWS;
    if (end > PSP_UI_MENU_ITEM_COUNT) end = PSP_UI_MENU_ITEM_COUNT;
    for (size_t at = first; at < end; at++) {
        int row_y = shadow.y + 52 + (int) (at - first) * row_spacing;
        if (at == ui->menu_selection) {
            fill_round_rect(pixels, width, height, stride,
                            (UiRect) { shadow.x + 8, row_y - 4,
                                       shadow.width - 16, row_height },
                            PSP_THEME_RADIUS_ROW, accent, 4);
        }
        draw_text_with_font(
                  pixels, width, height, stride, shadow.x + 18, row_y,
                  items[at], 24,
                  shadow.x + shadow.width - 18,
                  at == ui->menu_selection
                      ? PSP_THEME_ON_ACCENT
                      : (at == PSP_UI_MENU_ITEM_COUNT - 1
                             ? PSP_THEME_TEXT_MUTED : PSP_THEME_TEXT_BODY),
                  scale, NULL, false);
        if (at >= UI_MENU_ROW_TABS && at <= UI_MENU_ROW_HELP)
            draw_chevron(
                pixels, width, height, stride,
                shadow.x + shadow.width - 20, row_y + 4, 1,
                at == ui->menu_selection ? PSP_THEME_ON_ACCENT : accent);
    }
    draw_text_with_font(pixels, width, height, stride, shadow.x + 16,
              shadow.y + shadow.height - 18,
              "X Open   O Back   Start Address",
              36, shadow.x + shadow.width - 12,
              muted, scale, NULL, false);
}

static void draw_routed_list(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, const char *title, const char *context,
    const char *const *labels, const char *const *values,
    size_t count, size_t selection, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    UiRect box = {58, 10, width - 116, 252};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 45);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14, title, 25, text, 2);
    if (context != NULL && context[0] != '\0')
        draw_text_right_aligned(
            pixels, width, height, stride, box.x + box.width - 16,
            box.y + 15, context, 25, accent, 1, true);
    /* Seven 24px rows fit above the fixed hint bar. Longer routed lists are
       selection-windowed; drawing an eighth physical row would overlap the
       bottom hint and make its action label unreadable. */
    size_t visible = count < UI_ROUTED_LIST_VISIBLE_ROWS
        ? count : UI_ROUTED_LIST_VISIBLE_ROWS;
    size_t first = 0u;
    if (count > visible) {
        if (selection >= count) first = count - visible;
        else if (selection >= visible) first = selection - visible + 1u;
    }
    size_t end = first + visible;
    for (size_t at = first; at < end; at++) {
        int row_y = box.y + 57 + (int) (at - first) * 24;
        bool selected = at == selection;
        if (selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 22},
                PSP_THEME_RADIUS_ROW, accent, 4);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            labels[at], 31, box.x + box.width - 105,
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
        if (values != NULL && values[at] != NULL)
            draw_text_right_aligned(
                pixels, width, height, stride, box.x + box.width - 18,
                row_y, values[at], 18,
                selected ? PSP_THEME_ON_ACCENT : accent, 2, true);
    }
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 20, "X Open or toggle   O Back", 28,
        box.x + box.width - 14, muted, 2, NULL, false);
}

static TILEFINCH_OUT_OF_LINE void draw_page_tools(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    static const char *const labels[UI_PAGE_TOOLS_ITEM_COUNT] = {
        "Find in page", "Reader mode", "Basic view",
        "Add/remove bookmark", "Save article", "Save screenshot",
        "Site information", "Install offline app"
    };
    const char *values[UI_PAGE_TOOLS_ITEM_COUNT] = {
        ">", ui->reader_mode ? "On" : "Off",
        ui->basic_mode ? "On" : "Off", NULL, NULL, NULL, ">", NULL
    };
    draw_routed_list(
        ui, pixels, width, height, stride, "Page tools", NULL,
        labels, values, UI_PAGE_TOOLS_ITEM_COUNT, ui->menu_selection,
        accent, text, muted);
}

static TILEFINCH_OUT_OF_LINE void draw_site_controls(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    static const char *const labels[UI_SITE_CONTROLS_ITEM_COUNT] = {
        "JavaScript", "Content blocking", "Cookie notices",
        "Always Reader", "Third-party cookies", "Mixed HTTP (session)"
    };
    char domain[64], detail[64];
    ui_url_presentation(ui, domain, sizeof(domain), detail, sizeof(detail));
    bool blocker_available = ui->content_blocker_mode != CONTENT_BLOCKER_OFF
        && ui_content_blocker_site_available(ui);
    const char *values[UI_SITE_CONTROLS_ITEM_COUNT] = {
        !ui->javascript_enabled ? "Global off"
            : (ui->site_javascript_enabled ? "On" : "Off"),
        !blocker_available ? "N/A"
            : (ui->content_blocker_site_allowed ? "Off" : "On"),
        !ui_content_blocker_site_available(ui) ? "N/A"
            : (ui->cookie_banner_hidden ? "On" : "Off"),
        ui->reader_site_always ? "On" : "Off",
        ui->third_party_cookie_site_allowed ? "On" : "Off",
        ui->mixed_content_site_allowed ? "On" : "Off"
    };
    draw_routed_list(
        ui, pixels, width, height, stride, "Site controls", domain,
        labels, values, UI_SITE_CONTROLS_ITEM_COUNT,
        ui->menu_selection, accent, text, muted);
}

static TILEFINCH_OUT_OF_LINE void draw_help(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    static const char *const labels[UI_HELP_ITEM_COUNT] = {
        "Check Wi-Fi sign-in", "Diagnostic QR", "Last error summary",
        "Controls guide", "Version & system", "Licences"
    };
    static const char *const values[UI_HELP_ITEM_COUNT] = {
        ">", ">", ">", ">", ">"
    };
    draw_routed_list(
        ui, pixels, width, height, stride, "Help & diagnostics", NULL,
        labels, values, UI_HELP_ITEM_COUNT, ui->menu_selection,
        accent, text, muted);
}

static TILEFINCH_OUT_OF_LINE void draw_page_information(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {42, 12, width - 84, 248};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 40);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 14,
                   "Page tools  >  Site information", 40, text, 2);
    char domain[64], detail[128], connection[48], data[64];
    ui_url_presentation(ui, domain, sizeof(domain), detail, sizeof(detail));
    bool tls_verified = ui->secure
        && ui->site_tls_verify_result_available
        && ui->site_tls_verify_result == 0;
    snprintf(connection, sizeof(connection), "%s  |  %s",
             tls_verified ? "Secure"
                : (ui->secure && !ui->site_tls_verify_result_available
                       ? "TLS status unavailable"
                       : (ui->secure ? "Certificate warning" : "Not secure")),
             ui->site_tls_version);
    snprintf(data, sizeof(data), "%u cookies  |  %u storage  |  %u KB",
             (unsigned) ui->site_cookie_count,
             (unsigned) ui->site_storage_count,
             (unsigned) ((ui->site_data_bytes + 1023u) / 1024u));
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 49, domain, 38, accent, 2);
    draw_text(pixels, width, height, stride,
        box.x + 16, box.y + 72, connection, 44,
        tls_verified
            ? PSP_THEME_TEXT_BODY : PSP_THEME_WARN, 1);
    draw_text_with_font(pixels, width, height, stride,
        box.x + 16, box.y + 89, ui->site_tls_issuer, 54,
        box.x + box.width - 16, muted, 1, NULL, false);
    draw_text(pixels, width, height, stride,
        box.x + 16, box.y + 107, data, 48, muted, 1);
    static const char *const actions[3] = {
        "Permissions & controls", "Clear data for this site",
        "Reset permissions"
    };
    for (size_t at = 0; at < 3u; at++) {
        UiRect row = {box.x + 12, box.y + 126 + (int) at * 28,
                      box.width - 24, 24};
        bool selected = ui->menu_selection == at;
        bool confirm = selected
            && ui->data_clear_confirmation == (uint8_t) (at + 1u);
        if (selected)
            fill_round_rect(pixels, width, height, stride, row,
                            PSP_THEME_RADIUS_ROW,
                            PSP_THEME_SURFACE_FOCUS, 4);
        draw_text(pixels, width, height, stride, row.x + 8, row.y + 4,
                  confirm ? "Press X again to confirm" : actions[at], 42,
                  confirm ? PSP_THEME_WARN
                          : (selected ? text : PSP_THEME_TEXT_BODY), 1);
        if (at == 0u && !confirm)
            draw_text_right_aligned(
                pixels, width, height, stride,
                row.x + row.width - 8, row.y + 4, ">", 2,
                selected ? accent : muted, 1, true);
    }
    draw_text(pixels, width, height, stride,
        box.x + 16, box.y + box.height - 20,
        "X Open / confirm   O Back", 28, muted, 1);
}

static TILEFINCH_OUT_OF_LINE void draw_offline_app_preview(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    const PspUiOfflineAppPreview *preview = ui->offline_app_preview;
    UiRect box = {42, 12, width - 84, 248};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 40);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    static const char *const operations[] = {
        "Install offline app", "Update offline app", "Reinstall offline app"
    };
    static const char *const modes[] = {
        "Browser", "Minimal UI", "Standalone", "Fullscreen"
    };
    unsigned operation = preview == NULL || preview->operation >= 3u
        ? 0u : preview->operation;
    unsigned mode = preview == NULL || preview->display_mode >= 4u
        ? 0u : preview->display_mode;
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 14,
                   operations[operation], 36, text, 2);
    if (preview == NULL) return;
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 54, preview->name, 48, accent, 2);
    char size_line[64], resource_line[72], mode_line[48];
    uint64_t kib = (preview->estimated_bytes + 1023u) / 1024u;
    if (kib < 1024u)
        snprintf(size_line, sizeof(size_line),
                 "Estimated size: %llu KB", (unsigned long long) kib);
    else
        snprintf(size_line, sizeof(size_line),
                 "Estimated size: %llu.%01llu MB",
                 (unsigned long long) (kib / 1024u),
                 (unsigned long long) ((kib % 1024u) * 10u / 1024u));
    snprintf(resource_line, sizeof(resource_line),
             "%u captured  |  %u unavailable",
             (unsigned) preview->captured_resources,
             (unsigned) preview->unavailable_resources);
    snprintf(mode_line, sizeof(mode_line), "Display: %s", modes[mode]);
    draw_text(pixels, width, height, stride,
              box.x + 16, box.y + 84, size_line, 54,
              PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride,
              box.x + 16, box.y + 106, resource_line, 60,
              preview->unavailable_resources == 0 ? muted : PSP_THEME_WARN, 1);
    draw_text(pixels, width, height, stride,
              box.x + 16, box.y + 128, mode_line, 40, muted, 1);
    if (preview->theme_color_valid) {
        uint16_t swatch = rgb565(
            (preview->theme_color >> 16) & 0xffu,
            (preview->theme_color >> 8) & 0xffu,
            preview->theme_color & 0xffu);
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {box.x + 16, box.y + 151, 24, 14},
            PSP_THEME_RADIUS_CHIP, swatch, 4);
        draw_text(pixels, width, height, stride,
                  box.x + 48, box.y + 152, "App theme color", 30, muted, 1);
    }
    draw_text_with_font(
        pixels, width, height, stride,
        box.x + 16, box.y + 177,
        preview->unavailable_resources == 0
            ? "Only already-loaded same-origin resources are included."
            : "Unavailable resources may still need a network connection.",
        64, box.x + box.width - 16, muted, 1, NULL, false);
    draw_text(pixels, width, height, stride,
              box.x + 16, box.y + box.height - 20,
              "X Confirm   O Back", 24, muted, 1);
}

static size_t ui_failure_labels(
    const PspUiState *ui, const char **labels, size_t capacity)
{
    size_t count = 0;
#define ADD_FAILURE_LABEL(text_) \
    do { if (count < capacity) labels[count++] = (text_); } while (0)
    ADD_FAILURE_LABEL("Retry");
    if (ui->failure_actions & PSP_UI_FAILURE_READER)
        ADD_FAILURE_LABEL("Try Reader mode");
    if (ui->failure_actions & PSP_UI_FAILURE_WIFI)
        ADD_FAILURE_LABEL("Open Wi-Fi sign-in");
    if (ui->failure_actions & PSP_UI_FAILURE_DISABLE_JAVASCRIPT)
        ADD_FAILURE_LABEL("Disable JavaScript for this site");
    if (ui->failure_actions & PSP_UI_FAILURE_AUDIO_ONLY)
        ADD_FAILURE_LABEL("Use audio-only");
    if (ui->failure_actions & PSP_UI_FAILURE_LOWER_QUALITY)
        ADD_FAILURE_LABEL("Lower video quality");
    ADD_FAILURE_LABEL("Return to the last usable page");
#undef ADD_FAILURE_LABEL
    return count;
}

static TILEFINCH_OUT_OF_LINE void draw_failure_recovery(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {38, 8, width - 76, 256};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 40);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 15, box.y + 13, "Page did not open", 34,
                   PSP_THEME_WARN, 2);
    draw_text_with_font(
        pixels, width, height, stride, box.x + 15, box.y + 48,
        ui->status, 64, box.x + box.width - 15, muted, 1, NULL, false);
    const char *labels[7] = {0};
    size_t count = ui_failure_labels(ui, labels, 7u);
    int top = box.y + 70;
    int row_height = count > 6u ? 23 : 25;
    for (size_t at = 0; at < count; at++) {
        UiRect row = {box.x + 11, top + (int) at * row_height,
                      box.width - 22, row_height - 2};
        bool selected = ui->menu_selection == at;
        if (selected)
            fill_round_rect(pixels, width, height, stride, row,
                            PSP_THEME_RADIUS_ROW,
                            PSP_THEME_SURFACE_FOCUS, 4);
        draw_text(pixels, width, height, stride, row.x + 8, row.y + 3,
                  labels[at], 48, selected ? text : PSP_THEME_TEXT_BODY, 1);
    }
    draw_text(pixels, width, height, stride,
              box.x + 15, box.y + box.height - 20,
              "X Choose   O Return", 24, muted, 1);
    (void) accent;
}

static TILEFINCH_OUT_OF_LINE void draw_help_detail(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {52, 24, width - 104, 224};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 43);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    const char *title = "Help";
    const char *lines[5] = {NULL, NULL, NULL, NULL, NULL};
    if (ui->menu_selection == 2u) {
        title = "Last error summary";
        lines[0] = "The last saved error is included first";
        lines[1] = "in Diagnostic QR. Photograph every";
        lines[2] = "part to preserve the complete report.";
    } else if (ui->menu_selection == 3u) {
        title = "Controls guide";
        lines[0] = "X opens or toggles.  O goes back.";
        lines[1] = "Start opens Address.  Square reloads.";
        lines[2] = "Select: menu.  L/R: page/category.";
        lines[3] = "Analog moves the pointer or scrolls.";
        lines[4] = "Hold Start+Select for page controls.";
    } else if (ui->menu_selection == 4u) {
        title = "Version & system";
        lines[0] = "Tilefinch " TILEFINCH_VERSION_STRING;
        lines[1] = "Sony PSP release build";
        lines[2] = "480 x 272  |  333 MHz target";
    } else {
        title = "Licences";
        lines[0] = "Tilefinch is MIT licensed.";
        lines[1] = "Third-party notices and font licences";
        lines[2] = "are included in the distribution.";
    }
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14, title, 32, text, 2);
    for (size_t at = 0; at < 5u; at++) {
        if (lines[at] == NULL) continue;
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18,
            box.y + 61 + (int) at * 24, lines[at], 50,
            box.x + box.width - 18,
            at == 0u ? accent : PSP_THEME_TEXT_BODY,
            at == 0u ? 2 : 1, NULL, false);
    }
    draw_text(pixels, width, height, stride,
        box.x + 16, box.y + box.height - 20, "O Back", 12, muted, 2);
}

static TILEFINCH_OUT_OF_LINE void draw_tabs(
                      const PspUiState *ui, uint16_t *pixels, int width,
                      int height, int stride, uint16_t panel,
                      uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {52, 8, width - 104, 256};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 40);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 19);
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 13,
                   "Tabs", 8, text, 2);

    size_t count = ui->tabs == NULL ? 1u : ui->tabs->count;
    if (count == 0 || count > PSP_UI_TAB_LIMIT) count = 1u;
    size_t rows = count
        + (ui->tabs != NULL && ui->tabs->can_create
               && count < PSP_UI_TAB_LIMIT ? 1u : 0u);
    for (size_t at = 0; at < rows; at++) {
        int y = box.y + 46 + (int) at * 34;
        bool selected = at == ui->tab_selection;
        if (selected) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 8, y - 3, box.width - 16, 33},
                PSP_THEME_RADIUS_ROW, accent, 4);
        }
        if (at < count) {
            bool active = ui->tabs != NULL
                && at == ui->tabs->active_index;
            char label[PSP_UI_TAB_TITLE_CAPACITY + 8];
            const char *title = ui->tabs == NULL
                ? "CURRENT PAGE" : ui->tabs->titles[at];
            bool hibernated = ui->tabs != NULL
                && (ui->tabs->hibernated_mask & (uint8_t) (1u << at)) != 0;
            snprintf(
                label, sizeof(label), "%s%s",
                title == NULL || title[0] == '\0' ? "Untitled" : title,
                hibernated ? " [Z]" : "");
            bool has_thumbnail = ui->tabs != NULL
                && (ui->tabs->thumbnail_valid_mask
                    & (uint8_t) (1u << at)) != 0;
            UiRect thumbnail_box = {
                box.x + 13, y - 2,
                PSP_UI_TAB_THUMBNAIL_WIDTH,
                PSP_UI_TAB_THUMBNAIL_HEIGHT
            };
            if (has_thumbnail) {
                const uint16_t *thumbnail = ui->tabs->thumbnails[at];
                int left = thumbnail_box.x < 0 ? 0 : thumbnail_box.x;
                int top = thumbnail_box.y < 0 ? 0 : thumbnail_box.y;
                int right = thumbnail_box.x + thumbnail_box.width;
                int bottom = thumbnail_box.y + thumbnail_box.height;
                if (right > width) right = width;
                if (bottom > height) bottom = height;
                if (left < right && top < bottom) {
                    size_t source_x = (size_t) (left - thumbnail_box.x);
                    size_t row_bytes = (size_t) (right - left)
                        * sizeof(uint16_t);
                    for (int destination_y = top;
                         destination_y < bottom; destination_y++) {
                        size_t source_y =
                            (size_t) (destination_y - thumbnail_box.y);
                        memcpy(
                            pixels + (size_t) destination_y * (size_t) stride
                                + (size_t) left,
                            thumbnail
                                + source_y * PSP_UI_TAB_THUMBNAIL_WIDTH
                                + source_x,
                            row_bytes);
                    }
                }
            } else {
                /*
                 * The placeholder is one letter, so it must be copied out
                 * rather than character-capped: capping a longer title
                 * trips the shared helper's ellipsis path, and with no
                 * maximum_x to stop them the three dots marched out of the
                 * box and landed on the domain line beside it.
                 */
                char initial[8] = "?";
                const char *source = title == NULL || title[0] == '\0'
                    ? "?" : title;
                unsigned codepoint = 0;
                size_t used = font_utf8_next(
                    source, strlen(source), &codepoint);
                if (used != 0 && used < sizeof(initial)) {
                    memcpy(initial, source, used);
                    initial[used] = '\0';
                }
                fill_round_rect(
                    pixels, width, height, stride, thumbnail_box,
                    PSP_THEME_RADIUS_ROW, PSP_THEME_SURFACE, 4);
                int initial_width = chrome_text_width_bytes(
                    initial, strlen(initial), 2, true);
                draw_text_bold(
                    pixels, width, height, stride,
                    thumbnail_box.x
                        + (PSP_UI_TAB_THUMBNAIL_WIDTH - initial_width) / 2,
                    thumbnail_box.y + 10, initial, 1,
                    selected ? PSP_THEME_TEXT : accent, 2);
            }
            if (active) {
                fill_rect(
                    pixels, width, height, stride,
                    (UiRect) {box.x + 9, y - 2, 3, 30},
                    selected ? PSP_THEME_ON_ACCENT : accent, 4);
            }
            draw_text_with_font(
                pixels, width, height, stride, box.x + 82, y + 3,
                label, 38, box.x + box.width - 14,
                selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT, 1, NULL,
                true);
            const char *domain = ui->tabs == NULL
                ? "" : ui->tabs->domains[at];
            draw_text_with_font(
                pixels, width, height, stride, box.x + 82, y + 16,
                domain, 38, box.x + box.width - 14,
                /* Was a dark teal companion to the pre-ember accent; on an
                   ember selection fill it hue-clashed. The selected row now
                   uses the on-accent ink its title uses. */
                selected ? PSP_THEME_ON_ACCENT : muted, 1, NULL, false);
        } else {
            draw_text(
                pixels, width, height, stride, box.x + 22, y,
                "+  New tab", 12,
                selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY, 1);
        }
    }
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 19,
        count > 1u ? "X Open   Square Close   O Back"
                   : "X Open   O Back",
        40, box.x + box.width - 12, muted, 1, NULL, false);
}

static TILEFINCH_OUT_OF_LINE void draw_options(
                      const PspUiState *ui, uint16_t *pixels, int width,
                         int height, int stride, uint16_t panel,
                         uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {42, 17, width - 84, 238};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 43);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 20);
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 14,
                   "Settings", 20, text, 2);
    for (size_t at = 0; at < UI_SETTINGS_GROUP_COUNT; at++) {
        int row_y = box.y + 55 + (int) at * 24;
        bool selected = at == ui->options_group_selection;
        if (selected) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 23},
                PSP_THEME_RADIUS_ROW, accent, 4);
        }
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, row_y,
            ui_option_group_at(at), 28, box.x + box.width - 42,
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
        draw_text_right_aligned(
            pixels, width, height, stride, box.x + box.width - 20,
            row_y, ">", 1,
            selected ? PSP_THEME_ON_ACCENT : accent, 2, true);
    }
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 20, "X Open   O Back", 18,
        box.x + box.width - 14, muted, 2, NULL, false);
}

static const char *ui_video_language_name(unsigned language)
{
    switch ((BrowserVideoLanguage) language) {
        case BROWSER_VIDEO_LANGUAGE_SYSTEM: return "PSP system";
        case BROWSER_VIDEO_LANGUAGE_ORIGINAL: return "Original";
        case BROWSER_VIDEO_LANGUAGE_ENGLISH: return "English";
        case BROWSER_VIDEO_LANGUAGE_SPANISH: return "Spanish";
        case BROWSER_VIDEO_LANGUAGE_FRENCH: return "French";
        case BROWSER_VIDEO_LANGUAGE_GERMAN: return "German";
        case BROWSER_VIDEO_LANGUAGE_ITALIAN: return "Italian";
        case BROWSER_VIDEO_LANGUAGE_PORTUGUESE: return "Portuguese";
        case BROWSER_VIDEO_LANGUAGE_JAPANESE: return "Japanese";
        case BROWSER_VIDEO_LANGUAGE_KOREAN: return "Korean";
        case BROWSER_VIDEO_LANGUAGE_CHINESE_SIMPLIFIED:
            return "Chinese (S)";
        case BROWSER_VIDEO_LANGUAGE_CHINESE_TRADITIONAL:
            return "Chinese (T)";
        case BROWSER_VIDEO_LANGUAGE_RUSSIAN: return "Russian";
        case BROWSER_VIDEO_LANGUAGE_COUNT:
        default: return "PSP system";
    }
}

static const char *ui_subtitle_language_name(unsigned language)
{
    if (language == BROWSER_SUBTITLE_LANGUAGE_SYSTEM) return "PSP system";
    if (language == BROWSER_SUBTITLE_LANGUAGE_SAME_AS_AUDIO)
        return "Same as audio";
    return ui_video_language_name(language);
}

static const char *ui_alternate_language_name(unsigned language)
{
    return language == BROWSER_ALTERNATE_LANGUAGE_NONE
        ? "None" : ui_video_language_name(language);
}

static const char *ui_subtitle_size_name(unsigned size)
{
    return size == BROWSER_SUBTITLE_SIZE_SMALL ? "Small" : "Standard";
}

static const char *ui_subtitle_background_name(unsigned background)
{
    return background == BROWSER_SUBTITLE_BACKGROUND_SHADOW
        ? "Shadow" : "Box";
}

static TILEFINCH_OUT_OF_LINE void ui_option_row_presentation(
    const PspUiState *ui, size_t selection, const char **label,
    const char **value, char *formatted, size_t formatted_size)
{
    *label = "";
    *value = "";
    if (formatted_size != 0u) formatted[0] = '\0';
    switch (ui_option_id(selection)) {
        case UI_OPTION_BROWSER_UI_SCALE:
            *label = "Browser UI";
            *value = ui->browser_ui_scale >= 2u ? "Large" : "Compact";
            break;
        case UI_OPTION_PAGE_FONT_PERCENT:
            *label = (ui->reader_mode || ui->basic_mode)
                ? "Extracted text" : "Web pages";
            snprintf(formatted, formatted_size, "%u%%", ui->page_font_percent);
            *value = formatted;
            break;
        case UI_OPTION_READER_FONT:
            *label = "Reader font";
            *value = ui->reader_font_serif ? "Serif" : "Sans";
            break;
        case UI_OPTION_REMEMBER_READER_SCALE:
            *label = "Remember size";
            *value = ui->remember_reader_site_scale ? "Per site" : "Off";
            break;
        case UI_OPTION_READER_AUTO_MODE:
            *label = "Auto Reader";
            *value = ui->reader_auto_mode ? "On" : "Off";
            break;
        case UI_OPTION_CUSTOM_HOMEPAGE:
            *label = "Home page";
            *value = ui->custom_homepage_enabled ? "My links" : "Built-in";
            break;
        case UI_OPTION_HISTORY:
            *label = "URL history";
            *value = ui->history_enabled ? "On" : "Off";
            break;
        case UI_OPTION_RESTORE_LAST_PAGE:
            *label = "Restore page";
            *value = ui->restore_last_page ? "On" : "Off";
            break;
        case UI_OPTION_TAB_HIBERNATION:
            *label = "Hibernate tab";
            *value = ui->tab_hibernation_enabled ? "On" : "Off";
            break;
        case UI_OPTION_EXPERIMENTAL:
            *label = "Experimental";
            *value = ">";
            break;
        case UI_OPTION_ANALOG_CURSOR:
            *label = "Analog cursor";
            *value = ui->analog_cursor_enabled ? "On" : "Off";
            break;
        case UI_OPTION_JAVASCRIPT:
            *label = "JavaScript";
            *value = ui->javascript_enabled ? "On" : "Off";
            break;
        case UI_OPTION_SITE_JAVASCRIPT:
            *label = "This site JS";
            *value = !ui->javascript_enabled
                ? "Global off" : (ui->site_javascript_enabled ? "On" : "Off");
            break;
        case UI_OPTION_TEXT_ENTRY:
            *label = "Keyboard";
            *value = ui->danzeff_text_input ? "Danzeff" : "PSP OSK";
            break;
        case UI_OPTION_GAMEPAD_FACE_MAPPING:
            *label = "Game buttons";
            *value = ui->gamepad_circle_primary ? "O primary" : "X primary";
            break;
        case UI_OPTION_SEARCH_ENGINE:
            *label = "Search";
            *value = ui_search_engine_name(ui->search_engine);
            break;
        case UI_OPTION_COLOR_MODE:
            *label = "Night mode";
            *value = ui->color_mode == BROWSER_COLOR_MODE_DARK
                ? "Dark" : (ui->color_mode == BROWSER_COLOR_MODE_LIGHT
                                ? "Light" : "Auto");
            break;
        case UI_OPTION_CHROME_THEME:
            *label = "Theme";
            *value = ui_chrome_theme_name((BrowserChromeTheme) ui->chrome_theme);
            break;
        case UI_OPTION_LANGUAGE_AND_EMOJI:
            *label = "Language & emoji";
            *value = ">";
            break;
        case UI_OPTION_VIDEO_SCALING:
            *label = "Video scaling";
            *value = ui->video_scaling_sharp ? "Sharp" : "Smooth";
            break;
        case UI_OPTION_YOUTUBE_QUALITY:
            *label = "YouTube";
            snprintf(formatted, formatted_size, "%up",
                     ui->youtube_240p ? 240u : 360u);
            *value = formatted;
            break;
        case UI_OPTION_VIDEO_LANGUAGE:
            *label = "Audio & subtitles";
            *value = ">";
            break;
        case UI_OPTION_YOUTUBE_AUDIO_ONLY:
            *label = "Audio-only";
            *value = ui->youtube_audio_only ? "On" : "Off";
            break;
        case UI_OPTION_YOUTUBE_RESULTS:
            *label = "YouTube results";
            *value = ui->youtube_compact_results ? "Compact" : "Detailed";
            break;
        case UI_OPTION_VIDEO_STARTUP_BUFFERING:
            *label = "Video start";
            *value = ui->video_startup_buffering ? "Buffered" : "Immediate";
            break;
        case UI_OPTION_RESUME_DOWNLOADS:
            *label = "Resume saves";
            *value = ui->resume_offline_downloads ? "On" : "Off";
            break;
        case UI_OPTION_CONTENT_BLOCKER: {
            *label = "Content blocker";
            const char *mode = ui->content_blocker_mode == CONTENT_BLOCKER_BASIC
                ? "Basic" : (ui->content_blocker_mode == CONTENT_BLOCKER_CUSTOM
                                ? "Custom" : "Off");
            char count[12];
            if (ui->total_requests_blocked >= UINT64_C(1000000))
                snprintf(count, sizeof(count), "%lluM", (unsigned long long)
                         (ui->total_requests_blocked / UINT64_C(1000000)));
            else if (ui->total_requests_blocked >= UINT64_C(1000))
                snprintf(count, sizeof(count), "%lluK", (unsigned long long)
                         (ui->total_requests_blocked / UINT64_C(1000)));
            else
                snprintf(count, sizeof(count), "%llu",
                         (unsigned long long) ui->total_requests_blocked);
            snprintf(formatted, formatted_size, "%s / %s", mode, count);
            *value = formatted;
            break;
        }
        case UI_OPTION_COSMETIC_HIDING:
            *label = "Hide page ads";
            *value = ui->content_blocker_cosmetic_hiding ? "On" : "Off";
            break;
        case UI_OPTION_COOKIE_BANNERS:
            *label = "Cookie notices";
            *value = !ui_content_blocker_site_available(ui)
                ? "N/A" : (ui->cookie_banner_hidden ? "Hide" : "Show");
            break;
        case UI_OPTION_ALLOW_SITE:
            *label = "Allow site";
            *value = ui->content_blocker_mode == CONTENT_BLOCKER_OFF
                    || !ui_content_blocker_site_available(ui)
                ? "N/A" : (ui->content_blocker_site_allowed ? "Yes" : "No");
            break;
        case UI_OPTION_LOAD_ALLOWLIST:
            *label = "Load allowlist";
            *value = ">";
            break;
        case UI_OPTION_SITE_DATA_ALLOWED:
            *label = "Site data";
            *value = ui->site_data_allowed ? "Allow" : "Block";
            break;
        case UI_OPTION_TLS_SESSION_PERSISTENCE:
            *label = "TLS ticket saving";
            *value = ui->tls_session_persistence ? "On" : "Off";
            break;
        case UI_OPTION_MIXED_CONTENT_SITE:
            *label = "HTTP (session)";
            *value = ui->mixed_content_site_allowed ? "Allow" : "Block";
            break;
        case UI_OPTION_THIRD_PARTY_COOKIES_SITE:
            *label = "3rd-party cookies";
            *value = ui->third_party_cookie_site_allowed ? "Allow" : "Block";
            break;
        case UI_OPTION_NETWORK_PROFILE:
            *label = "Wi-Fi profile";
            if (ui->network_profile_label_valid) {
                *value = ui->network_profile_label;
            } else {
                snprintf(formatted, formatted_size, "Profile %u",
                         ui->network_profile == 0u ? 1u
                                                   : (unsigned) ui->network_profile);
                *value = formatted;
            }
            break;
        case UI_OPTION_DIAGNOSTIC_QR:
            *label = "Diagnostic QR";
            *value = ">";
            break;
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        case UI_OPTION_POWER_TEST:
            *label = "Power test";
            *value = ui->validation_power_test_phase != 0
                ? "Running" : "2 min auto";
            break;
        case UI_OPTION_MEDIA_TEST:
            *label = "Video test";
            *value = ui->validation_media_test_phase != 0 ? "Running" : "Auto";
            break;
#endif
        case UI_OPTION_UPDATE_CHECK:
            *label = "Update check";
            *value = ui->update_check_enabled ? "On" : "Off";
            break;
        case UI_OPTION_UPDATE:
            *label = "Version and update";
            *value = ui->update_release_available ? "New" : ">";
            break;
        case UI_OPTION_SITE_DATA:
            *label = "Manage site data";
            *value = ">";
            break;
    }
}

static TILEFINCH_OUT_OF_LINE void draw_option_items(
                              const PspUiState *ui, uint16_t *pixels,
                              int width, int height, int stride,
                              uint16_t panel, uint16_t accent,
                              uint16_t text, uint16_t muted)
{
    UiRect box = {58, 10, width - 116, 252};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 48);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 18);
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 14,
                   "Settings", 20, text, 2);
    draw_text_with_font(
        pixels, width, height, stride, box.x + 96, box.y + 14,
        ">", 1, box.x + 112, accent, 2, NULL, true);
    draw_text_with_font(
        pixels, width, height, stride, box.x + 116, box.y + 14,
        ui_option_group_at(ui_option_group_index(
            ui_option_id(ui->options_selection))), 24,
        box.x + box.width - 16, accent, 2, NULL, true);
    draw_text_right_aligned(
        pixels, width, height, stride, box.x + box.width - 16,
        box.y + 31, "L/R category   O Back", 22, muted, 1, false);
    size_t group = ui_option_group_index(
        ui_option_id(ui->options_selection));
    size_t group_items[UI_OPTIONS_ITEM_COUNT];
    size_t group_count = 0;
    size_t selected_row = 0;
    for (size_t at = 0; at < UI_OPTIONS_ITEM_COUNT; at++) {
        if (ui_option_group_index(ui_option_id(at)) != group) continue;
        if (at == ui->options_selection) selected_row = group_count;
        group_items[group_count++] = at;
    }
    size_t first = 0;
    if (selected_row >= UI_OPTIONS_VISIBLE_ROWS)
        first = selected_row - UI_OPTIONS_VISIBLE_ROWS + 1u;
    size_t maximum_first = group_count > UI_OPTIONS_VISIBLE_ROWS
        ? group_count - UI_OPTIONS_VISIBLE_ROWS : 0u;
    if (first > maximum_first) first = maximum_first;
    size_t end = first + UI_OPTIONS_VISIBLE_ROWS;
    if (end > group_count) end = group_count;
    const int row_spacing = 23;
    const int row_height = 21;
    /* A group that fits on one screen reserves no right gutter, so its values
       and selection pill run to a normal margin. Only a scrolling group gives
       up a narrow strip on the right for its (smaller) chevrons; values and
       the pill stop short of it so neither can merge with an arrow. */
    bool scrolls = first != 0 || end != group_count;
    const int value_right = box.x + box.width - (scrolls ? 46 : 16);
    const int pill_width = box.width - (scrolls ? 48 : 20);
    const int label_right = box.x + box.width - (scrolls ? 145 : 115);
    const int scroll_gutter_x = box.x + box.width - 17;
    for (size_t row = first; row < end; row++) {
        size_t at = group_items[row];
        const char *label = "";
        const char *value = "";
        char formatted[32];
        ui_option_row_presentation(
            ui, at, &label, &value, formatted, sizeof(formatted));
        int row_y = box.y + 60 + (int) (row - first) * row_spacing;
        bool selected = at == ui->options_selection;
        if (selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {
                    box.x + 10, row_y - 4, pill_width, row_height
                },
                PSP_THEME_RADIUS_ROW, accent, 4);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            label, 28, label_right,
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
        draw_text_right_aligned(
            pixels, width, height, stride, value_right,
            row_y, value, 22,
            selected ? PSP_THEME_ON_ACCENT : accent, 2, true);
    }
    if (first != 0)
        draw_vertical_chevron(
            pixels, width, height, stride,
            scroll_gutter_x, box.y + 63, -1, muted, 3);
    if (end < group_count)
        draw_vertical_chevron(
            pixels, width, height, stride,
            scroll_gutter_x,
            box.y + 63
                + ((int) UI_OPTIONS_VISIBLE_ROWS - 1) * row_spacing,
            1, muted, 3);
    draw_text(pixels, width, height, stride, box.x + 16,
              box.y + box.height - 18,
              ui_option_description(ui_option_id(ui->options_selection)),
              46, muted, 1);
}

static TILEFINCH_OUT_OF_LINE void draw_experimental_options(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    /* The optional component row made this a full-height menu. Keep every
       highlight above the fixed hint band in both shipping and validation
       builds; a shorter panel caused the last rows to overlap the footer. */
    UiRect box = {42, 14, width - 84, 244};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 22);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14,
                   "Experimental", 24, text, 2);
    char voice[40], model[48], search[40], memory[40], updates[48],
        developer[48];
    snprintf(voice, sizeof(voice), "Voice input  %s",
             ui->experimental_voice_input ? "On" : "Off");
    snprintf(search, sizeof(search), "Voice search%s",
             ui->experimental_voice_input ? "..." : "  (Off)");
    snprintf(memory, sizeof(memory), "Voice memory %s",
             ui->adaptive_voice_memory ? "Adaptive" : "Full");
    static const char *const model_states[] = {
        "Checking...", "Not installed", "Checking...", "Downloading...",
        "Installing...", "Installed", "Try again", "Legacy slot model"
    };
    unsigned model_phase = ui->voice_component_phase;
    if (model_phase >= sizeof(model_states) / sizeof(model_states[0]))
        model_phase = 0;
    if (ui->voice_component_remove_confirmation) {
        snprintf(model, sizeof(model), "Voice model  Remove?");
    } else if ((model_phase == PSP_UI_VOICE_COMPONENT_DOWNLOADING
                || model_phase == PSP_UI_VOICE_COMPONENT_INSTALLING)
               && ui->voice_component_progress_plus_one != 0) {
        snprintf(model, sizeof(model), "Voice model  %d%%",
                 ((int) ui->voice_component_progress_plus_one - 1) / 10);
    } else {
        if (model_phase == PSP_UI_VOICE_COMPONENT_NOT_INSTALLED) {
            snprintf(model, sizeof(model),
                     "Voice model  Download (9.1 MB)");
        } else {
            snprintf(model, sizeof(model), "Voice model  %s",
                     model_states[model_phase]);
        }
    }
    if (ui->update_channel == BROWSER_UPDATE_CHANNEL_DEVELOPER)
        snprintf(updates, sizeof(updates),
                 "Updates  Dev URL (untrusted)");
    else
        snprintf(updates, sizeof(updates), "Update channel  %s",
                 ui->update_channel == BROWSER_UPDATE_CHANNEL_BETA
                     ? "Beta" : "Stable");
    snprintf(developer, sizeof(developer), "Developer URL  %s...",
             ui->developer_update_available ? "Edit" : "Set");
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
    char decoder[48];
    snprintf(decoder, sizeof(decoder), "%s  %s",
             ui->experimental_decoder_restart_prompt
                 ? "Restart to apply?" : "Video decoder",
             ui->experimental_decoder_restart_prompt
                 ? "X Yes  O Later"
                 : psp_media_wide_program_choice(
                       ui->experimental_decoder_choice
                       % PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT));
#endif
    const char *rows[UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT] = {
        voice, model, search, memory, updates, developer
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        , decoder
#endif
    };
    for (size_t at = 0; at < UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT; at++) {
        int row_y = box.y + 55 + (int) at * 24;
        if (at == ui->experimental_options_selection) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 22},
                PSP_THEME_RADIUS_ROW, accent, 4);
        }
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            rows[at], 32, box.x + box.width - 14,
            at == ui->experimental_options_selection
                ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
    }
    const char *hint = ui->experimental_options_selection == 5
        ? "X Edit URL   O Back" : "X Select   O Back";
    if (ui->experimental_options_selection == 1) {
        hint = ui->voice_component_remove_confirmation
            ? "X Remove model   O Cancel"
                   : (ui->voice_component_phase == PSP_UI_VOICE_COMPONENT_READY
                   ? "X Remove model   O Back"
                   : ui->voice_component_phase
                              == PSP_UI_VOICE_COMPONENT_LEGACY
                          ? "X Download shared copy   O Back"
                   : (ui->voice_component_phase
                              == PSP_UI_VOICE_COMPONENT_CHECKING
                          || ui->voice_component_phase
                              == PSP_UI_VOICE_COMPONENT_DOWNLOADING
                          || ui->voice_component_phase
                              == PSP_UI_VOICE_COMPONENT_INSTALLING
                          ? "X Cancel   O Back"
                          : "X Download 9.1 MB   O Back"));
    }
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
    if (ui->experimental_decoder_restart_prompt)
        hint = "X Restart now   O Apply next launch";
    else if (ui->experimental_options_selection
             == UI_EXPERIMENTAL_ROW_VIDEO_DECODER)
        hint = "Left/Right choose   X Save   O Back";
#endif
    draw_text(pixels, width, height, stride, box.x + 16,
              box.y + box.height - 22, hint, 24, muted, 2);
}

static const char *ui_glyph_language_name(unsigned language)
{
    switch ((BrowserGlyphLanguage) language) {
        case BROWSER_GLYPH_LANGUAGE_JAPANESE: return "Japanese";
        case BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED:
            return "Chinese (Simplified)";
        case BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL:
            return "Chinese (Traditional)";
        case BROWSER_GLYPH_LANGUAGE_KOREAN: return "Korean";
        case BROWSER_GLYPH_LANGUAGE_CYRILLIC: return "Cyrillic";
        case BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED:
            return "Extended Latin";
        case BROWSER_GLYPH_LANGUAGE_ARABIC: return "Arabic";
        case BROWSER_GLYPH_LANGUAGE_HEBREW: return "Hebrew";
        case BROWSER_GLYPH_LANGUAGE_COUNT:
        case BROWSER_GLYPH_LANGUAGE_EMBEDDED:
        default: return "Embedded";
    }
}

static bool ui_glyph_language_pack(
    unsigned language, TilefinchGlyphPack *pack)
{
    if (pack == NULL) return false;
    switch ((BrowserGlyphLanguage) language) {
        case BROWSER_GLYPH_LANGUAGE_JAPANESE:
            *pack = TILEFINCH_GLYPH_PACK_JAPANESE;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED:
            *pack = TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL:
            *pack = TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL;
            return true;
        case BROWSER_GLYPH_LANGUAGE_KOREAN:
            *pack = TILEFINCH_GLYPH_PACK_KOREAN;
            return true;
        case BROWSER_GLYPH_LANGUAGE_CYRILLIC:
            *pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
            return true;
        case BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED:
            *pack = TILEFINCH_GLYPH_PACK_LATIN_EXTENDED;
            return true;
        case BROWSER_GLYPH_LANGUAGE_ARABIC:
            *pack = TILEFINCH_GLYPH_PACK_ARABIC;
            return true;
        case BROWSER_GLYPH_LANGUAGE_HEBREW:
            *pack = TILEFINCH_GLYPH_PACK_HEBREW;
            return true;
        case BROWSER_GLYPH_LANGUAGE_COUNT:
        case BROWSER_GLYPH_LANGUAGE_EMBEDDED:
        default:
            return false;
    }
}

static const char *ui_glyph_pack_state(
    const PspUiState *ui, TilefinchGlyphPack pack, char output[32])
{
    unsigned phase = ui->glyph_operation_pack == (unsigned) pack
        ? ui->glyph_component_phase : PSP_UI_GLYPH_COMPONENT_UNKNOWN;
    if ((phase == PSP_UI_GLYPH_COMPONENT_DOWNLOADING
         || phase == PSP_UI_GLYPH_COMPONENT_INSTALLING)
        && ui->glyph_component_progress_plus_one != 0) {
        snprintf(output, 32, "%s %d%%",
                 phase == PSP_UI_GLYPH_COMPONENT_DOWNLOADING
                     ? "Downloading" : "Installing",
                 ((int) ui->glyph_component_progress_plus_one - 1) / 10);
        return output;
    }
    switch ((PspUiGlyphComponentPhase) phase) {
        case PSP_UI_GLYPH_COMPONENT_CHECKING: return "Checking...";
        case PSP_UI_GLYPH_COMPONENT_DOWNLOADING: return "Downloading...";
        case PSP_UI_GLYPH_COMPONENT_INSTALLING: return "Installing...";
        case PSP_UI_GLYPH_COMPONENT_ERROR: return "Try again";
        default:
            return (ui->glyph_installed_mask & (1u << (unsigned) pack)) != 0
                ? "Installed" : "Not installed";
    }
}

static TILEFINCH_OUT_OF_LINE void draw_theme_options(
    const PspUiState *ui, uint16_t *pixels, int width, int height, int stride,
    uint16_t accent, uint16_t text, uint16_t muted)
{
    const PspUiThemeCatalog *catalog = psp_ui_theme_catalog_bound();
    const size_t builtins = BROWSER_CHROME_THEME_CUSTOM;
    size_t custom_count = psp_ui_theme_catalog_count(catalog);
    size_t count = builtins + custom_count;
    UiRect box = {58, 10, width - 116, 252};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 48);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 18);
    draw_text_bold(pixels, width, height, stride, box.x + 16, box.y + 14,
                   "Settings", 20, text, 2);
    draw_text_with_font(pixels, width, height, stride, box.x + 96,
                        box.y + 14, ">", 1, box.x + 112, accent, 2,
                        NULL, true);
    draw_text_with_font(pixels, width, height, stride, box.x + 116,
                        box.y + 14, "Themes", 16,
                        box.x + box.width - 16, accent, 2, NULL, true);
    draw_text_right_aligned(pixels, width, height, stride,
                            box.x + box.width - 16, box.y + 31,
                            "X Apply   O Back", 18, muted, 1, false);

    size_t selected = ui->data_options_selection;
    if (selected >= count) selected = 0u;
    size_t first = selected >= UI_OPTIONS_VISIBLE_ROWS
        ? selected - UI_OPTIONS_VISIBLE_ROWS + 1u : 0u;
    size_t maximum_first = count > UI_OPTIONS_VISIBLE_ROWS
        ? count - UI_OPTIONS_VISIBLE_ROWS : 0u;
    if (first > maximum_first) first = maximum_first;
    size_t end = first + UI_OPTIONS_VISIBLE_ROWS;
    if (end > count) end = count;
    size_t active_custom = psp_ui_theme_catalog_selected(catalog);
    for (size_t row = first; row < end; row++) {
        bool row_selected = row == selected;
        bool custom = row >= builtins;
        size_t custom_index = custom ? row - builtins : 0u;
        const char *label = custom
            ? psp_ui_theme_catalog_label(catalog, custom_index)
            : ui_chrome_theme_name((BrowserChromeTheme) row);
        const PspUiThemePalette *palette = custom ? NULL
            : psp_ui_theme_palette((BrowserChromeTheme) row);
        uint16_t row_accent = custom
            ? psp_ui_theme_catalog_accent(catalog, custom_index)
            : palette->accent;
        bool active = custom
            ? ui->chrome_theme == BROWSER_CHROME_THEME_CUSTOM
                && active_custom == custom_index
            : ui->chrome_theme == row;
        int row_y = box.y + 60 + (int) (row - first) * 23;
        if (row_selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 4, box.width - 20, 21},
                PSP_THEME_RADIUS_ROW, accent, 4);
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {box.x + box.width - 48, row_y - 1, 18, 10},
            PSP_THEME_RADIUS_CHIP, row_accent, 4);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            label == NULL ? "Unnamed" : label, 25, box.x + box.width - 82,
            row_selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
        if (active)
            draw_text_right_aligned(
                pixels, width, height, stride, box.x + box.width - 58,
                row_y, "Active", 6,
                row_selected ? PSP_THEME_ON_ACCENT : accent, 1, true);
    }
    const char *hint = custom_count == 0u
        ? "Copy .tfth files into data/themes"
        : (psp_ui_theme_catalog_truncated(catalog)
               ? "Showing first 12 valid theme files"
               : "Built-ins and downloaded theme files");
    draw_text(pixels, width, height, stride, box.x + 16,
              box.y + box.height - 18, hint, 46, muted, 1);
}

static TILEFINCH_OUT_OF_LINE void draw_glyph_options(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    UiRect box = {42, 14, width - 84, 244};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 22);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14,
                   "Language & emoji", 30, text, 2);
    TilefinchGlyphPack language_pack = TILEFINCH_GLYPH_PACK_JAPANESE;
    bool downloadable = ui_glyph_language_pack(
        ui->glyph_language, &language_pack);
    char language_state[32], emoji_state[32];
    char rows[4][64];
    snprintf(rows[0], sizeof(rows[0]), "Language  %s",
             ui_glyph_language_name(ui->glyph_language));
    snprintf(rows[1], sizeof(rows[1]), "Emoji  %s",
             ui->color_emoji ? "Color pack" : "Embedded");
    snprintf(rows[2], sizeof(rows[2]), "Language pack  %s",
             downloadable
                 ? ui_glyph_pack_state(ui, language_pack, language_state)
                 : "Built in");
    snprintf(rows[3], sizeof(rows[3]), "Color emoji pack  %s",
             ui_glyph_pack_state(
                 ui, TILEFINCH_GLYPH_PACK_COLOR_EMOJI, emoji_state));
    for (size_t at = 0; at < 4u; at++) {
        int row_y = box.y + 58 + (int) at * 34;
        bool selected = at == ui->glyph_options_selection;
        if (selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 24},
                PSP_THEME_RADIUS_ROW, accent, 4);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            rows[at], 42, box.x + box.width - 14,
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            2, NULL, false);
    }
    const char *hint = "Left/Right choose   O Back";
    if (ui->glyph_options_selection >= 2u) {
        TilefinchGlyphPack pack = ui->glyph_options_selection == 2u
            ? language_pack : TILEFINCH_GLYPH_PACK_COLOR_EMOJI;
        bool available = ui->glyph_options_selection != 2u || downloadable;
        bool active = available
            && ui->glyph_operation_pack == (unsigned) pack
            && (ui->glyph_component_phase
                    == PSP_UI_GLYPH_COMPONENT_CHECKING
                || ui->glyph_component_phase
                    == PSP_UI_GLYPH_COMPONENT_DOWNLOADING
                || ui->glyph_component_phase
                    == PSP_UI_GLYPH_COMPONENT_INSTALLING);
        bool installed = available
            && (ui->glyph_installed_mask & (1u << (unsigned) pack)) != 0;
        if (!available) hint = "Choose a language above   O Back";
        else if (active) hint = "X Cancel   O Back";
        else if (ui->glyph_component_remove_confirmation)
            hint = "Square remove   O Cancel";
        else if (installed) hint = "X Check update   Square remove";
        else hint = "X Download pack   O Back";
    }
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 22, hint, 34,
        box.x + box.width - 14, muted, 1, NULL, false);
}

static TILEFINCH_OUT_OF_LINE void draw_video_language_options(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    UiRect box = {42, 30, width - 84, 210};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 22);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14,
                   "Settings > Video > Audio & subtitles", 38, text, 2);
    static const char *const labels[5] = {
        "Audio language", "Subtitle language", "Alternate language",
        "Subtitle size", "Subtitle background"
    };
    const char *values[5] = {
        ui_video_language_name(ui->video_language),
        ui_subtitle_language_name(ui->subtitle_language),
        ui_alternate_language_name(ui->alternate_language),
        ui_subtitle_size_name(ui->subtitle_size),
        ui_subtitle_background_name(ui->subtitle_background)
    };
    for (size_t at = 0; at < 5u; at++) {
        int row_y = box.y + 51 + (int) at * 27;
        bool selected = at == ui->data_options_selection;
        if (selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 22},
                PSP_THEME_RADIUS_ROW, accent, 4);
        uint16_t color = selected ? PSP_THEME_ON_ACCENT
                                  : PSP_THEME_TEXT_BODY;
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            labels[at], 24, box.x + 220, color, 1, NULL, false);
        draw_text_right_aligned(
            pixels, width, height, stride, box.x + box.width - 18,
            row_y, values[at], 18, color, 1, true);
    }
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 22, "Left/Right choose   O Back", 30,
        box.x + box.width - 14, muted, 1, NULL, false);
}

static TILEFINCH_OUT_OF_LINE void draw_update(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    UiRect box = {42, 14, width - 84, 244};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 18);
    draw_text(
        pixels, width, height, stride, box.x + 16, box.y + 14,
        "Tilefinch update", 30, text, 2);
    char current[64];
    snprintf(
        current, sizeof(current), "Current version  %s",
        ui->update_version[0] == '\0' ? "Unknown" : ui->update_version);
    draw_text(
        pixels, width, height, stride, box.x + 16, box.y + 56,
        current, 38, PSP_THEME_TEXT_BODY, 2);
    draw_text(
        pixels, width, height, stride, box.x + 16, box.y + 80,
        ui->update_status, 42, accent, 2);
    if (ui->update_notes[0] != '\0') {
        draw_text(
            pixels, width, height, stride, box.x + 16, box.y + 101,
            ui->update_notes, 30, PSP_THEME_TEXT_BODY, 2);
    }
    if (ui->update_progress_per_mille >= 0) {
        UiRect track = {box.x + 16, box.y + 119, box.width - 32, 8};
        fill_round_rect(
            pixels, width, height, stride, track, PSP_THEME_RADIUS_CHIP,
            PSP_THEME_SURFACE, 4);
        int filled = track.width * ui->update_progress_per_mille / 1000;
        if (filled > 0) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {track.x, track.y, filled, track.height},
                PSP_THEME_RADIUS_CHIP, accent, 4);
        }
    }
    if (ui->update_primary_label[0] != '\0') {
        /* Disabled is a surface with faint ink, never a filled button in a
           colour that still invites the press. */
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {box.x + 16, box.y + box.height - 48,
                      box.width - 32, 25},
            PSP_THEME_RADIUS_CHIP,
            ui->update_primary_enabled ? accent : PSP_THEME_SURFACE, 4);
        draw_text(
            pixels, width, height, stride, box.x + 28,
            box.y + box.height - 42, ui->update_primary_label, 42,
            ui->update_primary_enabled
                ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_FAINT, 2);
    }
    if (!ui->update_cancel_enabled
        && ui->update_channel == BROWSER_UPDATE_CHANNEL_STABLE) {
        draw_text_with_font(
            pixels, width, height, stride, box.x + 16,
            box.y + box.height - 15,
            "Square Versions   O Back", 28,
            box.x + box.width - 14, muted, 1, NULL, false);
    } else {
        draw_text(
            pixels, width, height, stride, box.x + 16,
            box.y + box.height - 18,
            ui->update_cancel_enabled ? "O Stop" : "O Back",
            20, muted, 2);
    }
}

static TILEFINCH_OUT_OF_LINE void draw_update_versions(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, uint16_t text, uint16_t muted)
{
    UiRect box = {70, 14, width - 140, 244};
    ui_apply_overlay_motion(ui, &box);
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 22);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14,
                   "Previous versions", 24, text, 2);
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16, box.y + 49,
        "Signed and installed as an A/B trial", 42,
        box.x + box.width - 14, muted, 1, NULL, false);
    size_t count = ui->update_history_count;
    if (ui->update_history_phase == TILEFINCH_UPDATE_HISTORY_LOADING) {
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, box.y + 92,
            "Loading releases...", 24, box.x + box.width - 14,
            text, 1, NULL, false);
    } else if (ui->update_history_phase == TILEFINCH_UPDATE_HISTORY_ERROR) {
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, box.y + 92,
            "Could not load releases", 28, box.x + box.width - 14,
            text, 1, NULL, false);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, box.y + 115,
            "Check Wi-Fi, then press X to retry", 38,
            box.x + box.width - 14, muted, 1, NULL, false);
    } else if (count == 0) {
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, box.y + 92,
            "No earlier releases found", 30,
            box.x + box.width - 14, text, 1, NULL, false);
    } else {
        const size_t visible_capacity = 6u;
        size_t visible = count < visible_capacity
            ? count : visible_capacity;
        size_t start = 0;
        if (ui->data_options_selection >= visible)
            start = ui->data_options_selection - visible + 1u;
        if (start + visible > count) start = count - visible;
        if (count > visible) {
            char range[16];
            snprintf(range, sizeof(range), "%u-%u of %u",
                     (unsigned) start + 1u,
                     (unsigned) (start + visible), (unsigned) count);
            draw_text_with_font(
                pixels, width, height, stride,
                box.x + box.width - 82, box.y + 16,
                range, 14, box.x + box.width - 14,
                muted, 1, NULL, false);
        }
        for (size_t row = 0; row < visible; row++) {
            size_t index = start + row;
            int y = box.y + 68 + (int) row * 27;
            bool selected = index == ui->data_options_selection;
            if (selected) {
                fill_round_rect(
                    pixels, width, height, stride,
                    (UiRect) {box.x + 10, y - 3, box.width - 20, 24},
                    PSP_THEME_RADIUS_CHIP, PSP_THEME_SURFACE, 4);
                fill_rect(
                    pixels, width, height, stride,
                    (UiRect) {box.x + 10, y - 3, 3, 24}, accent, 4);
            }
            char label[24];
            snprintf(label, sizeof(label), "Tilefinch %s",
                     ui->update_history_versions[index]);
            draw_text_with_font(
                pixels, width, height, stride, box.x + 20, y,
                label, 22, box.x + box.width - 14,
                selected ? text : PSP_THEME_TEXT_BODY,
                1, NULL, false);
        }
    }
    const char *hint = ui->update_history_phase
            == TILEFINCH_UPDATE_HISTORY_ERROR
        ? "X Retry   O Back"
        : count != 0 ? "X Select   O Back" : "O Back";
    draw_text_with_font(
        pixels, width, height, stride, box.x + 16,
        box.y + box.height - 18, hint, 22,
        box.x + box.width - 14, muted, 1, NULL, false);
}

static TILEFINCH_OUT_OF_LINE void draw_data_options(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    UiRect box = {54, 15, width - 108, 242};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 41);
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 22);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 14,
                   "Site data", 20, text, 2);
    char live[32], cache[32], local[32];
    if (ui->live_cache_kib < 1024u) {
        snprintf(live, sizeof(live), "Memory cache  %u KB",
                 ui->live_cache_kib);
    } else {
        snprintf(live, sizeof(live), "Memory cache  %u MB",
                 ui->live_cache_kib / 1024u);
    }
    if (ui->persistent_cache_mb == 0) {
        snprintf(cache, sizeof(cache), "Disk cache   Off");
    } else {
        snprintf(cache, sizeof(cache), "Disk cache   %u MB",
                 ui->persistent_cache_mb);
    }
    snprintf(local, sizeof(local), "Save local data  %s",
             ui->persist_local_storage ? "On" : "Off");
    const char *rows[UI_DATA_OPTIONS_ITEM_COUNT] = {
        live, cache, local, "Clear HTTP caches", "Clear cookies",
        "Clear local storage", "Clear session storage"
    };
    for (size_t at = 0; at < UI_DATA_OPTIONS_ITEM_COUNT; at++) {
        int row_y = box.y + 50 + (int) at * 24;
        if (at == ui->data_options_selection) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, row_y - 5, box.width - 20, 21},
                PSP_THEME_RADIUS_ROW, accent, 4);
        }
        bool destructive = at >= 3u;
        draw_text_with_font(
            pixels, width, height, stride, box.x + 18, row_y,
            rows[at], 32, box.x + box.width - 14,
            at == ui->data_options_selection
                ? PSP_THEME_ON_ACCENT
                /* Destructive rows keep their own red: it is a semantic,
                   not an accent, and it never lands on an accent fill. */
                : (destructive ? rgb565(239, 107, 117)
                               : PSP_THEME_TEXT_BODY),
            2, NULL, false);
    }
    draw_text(pixels, width, height, stride, box.x + 16,
              box.y + box.height - 22,
              ui->data_clear_confirmation != 0
                  ? "X Confirm clear   O Cancel" : "X Select   O Back",
              28,
              muted, 2);
}

/* Full-screen diagnostic transport. Version 27 at two pixels per module plus
   the four-module quiet zone is exactly 266 pixels square, so the PSP's
   272-pixel height preserves three untouched rows above and below it. Every
   dark run is an integer rectangle; no filtered texture or fractional scale
   can soften module edges. */
static TILEFINCH_OUT_OF_LINE void draw_diagnostic_qr(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t accent)
{
    const uint16_t black = rgb565(0, 0, 0);
    const uint16_t white = rgb565(255, 255, 255);
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, height}, PSP_THEME_GROUND, 4);
    const TilefinchDiagnosticQrView *view = ui->diagnostic_qr;
    if (view == NULL || view->modules == NULL
        || view->module_count != TILEFINCH_DIAGNOSTIC_QR_MODULES) {
        UiRect box = {54, 31, width - 108, 210};
        draw_panel_shell(pixels, width, height, stride, box);
        fill_round_edge_bar(pixels, width, height, stride, box,
                            PSP_THEME_RADIUS_PANEL, 4, accent);
        draw_text_bold(pixels, width, height, stride,
                       box.x + 18, box.y + 18,
                       "Diagnostic QR", 24, PSP_THEME_TEXT, 2);
        draw_text(pixels, width, height, stride,
                  box.x + 18, box.y + 54,
                  "Build a QR report from Tilefinch's", 42,
                  PSP_THEME_TEXT_BODY, 2);
        draw_text(pixels, width, height, stride,
                  box.x + 18, box.y + 76,
                  "existing diagnostic logs.", 42,
                  PSP_THEME_TEXT_BODY, 2);
        draw_text_with_font(
            pixels, width, height, stride,
            box.x + 18, box.y + 112,
            ui->status[0] == '\0'
                ? "The log files stay unchanged." : ui->status,
            46, box.x + box.width - 18,
            ui->status[0] == '\0' ? PSP_THEME_TEXT_MUTED : PSP_THEME_WARN,
            2, NULL, false);
        draw_text(pixels, width, height, stride,
                  box.x + 18, box.y + box.height - 34,
                  "X Build report     O Back", 38,
                  accent, 2);
        return;
    }

    const int qr_x = 3;
    const int qr_y = 3;
    const int module_pixels = (int) TILEFINCH_DIAGNOSTIC_QR_MODULE_PIXELS;
    const int quiet_pixels =
        (int) (TILEFINCH_DIAGNOSTIC_QR_QUIET_MODULES
               * TILEFINCH_DIAGNOSTIC_QR_MODULE_PIXELS);
    fill_rect(
        pixels, width, height, stride,
        (UiRect) {qr_x, qr_y, (int) TILEFINCH_DIAGNOSTIC_QR_RENDER_PIXELS,
                  (int) TILEFINCH_DIAGNOSTIC_QR_RENDER_PIXELS}, white, 4);
    int modules = (int) view->module_count;
    for (int y = 0; y < modules; y++) {
        int x = 0;
        while (x < modules) {
            while (x < modules && !tilefinch_diagnostic_qr_module(
                       view, (unsigned) x, (unsigned) y)) x++;
            int first = x;
            while (x < modules && tilefinch_diagnostic_qr_module(
                       view, (unsigned) x, (unsigned) y)) x++;
            if (first < x) {
                fill_rect(
                    pixels, width, height, stride,
                    (UiRect) {
                        qr_x + quiet_pixels + first * module_pixels,
                        qr_y + quiet_pixels + y * module_pixels,
                        (x - first) * module_pixels, module_pixels
                    }, black, 4);
            }
        }
    }

    const int text_x = qr_x + (int) TILEFINCH_DIAGNOSTIC_QR_RENDER_PIXELS + 10;
    char page[24];
    char part[24];
    char version[32];
    char firmware[32];
    snprintf(page, sizeof(page), "Page %u of %u",
             view->page_index + 1u, view->page_count);
    snprintf(part, sizeof(part), "Part %u of %u",
             view->part_index + 1u, view->part_count);
    snprintf(version, sizeof(version), "Tilefinch %s", view->app_version);
    snprintf(firmware, sizeof(firmware), "Firmware %s", view->firmware);
    draw_text_bold(pixels, width, height, stride, text_x, 10,
                   "Diagnostic", 22, PSP_THEME_TEXT, 2);
    draw_text_bold(pixels, width, height, stride, text_x, 29,
                   "report", 22, PSP_THEME_TEXT, 2);
    draw_text(pixels, width, height, stride, text_x, 58,
              version, 30, PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride, text_x, 71,
              view->device, 24, PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride, text_x, 84,
              firmware, 26, PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride, text_x, 103,
              part, 22, accent, 2);
    draw_text(pixels, width, height, stride, text_x, 120,
              page, 22, accent, 1);
    draw_text(pixels, width, height, stride, text_x, 136,
              "Error", 12, PSP_THEME_TEXT_MUTED, 1);
    draw_text_with_font(
        pixels, width, height, stride, text_x, 149,
        view->error_summary, 34, width - 6,
        PSP_THEME_TEXT_BODY, 1, NULL, false);
    draw_text(pixels, width, height, stride, text_x, 174,
              "Take a clear photo", 30, PSP_THEME_TEXT, 1);
    draw_text(pixels, width, height, stride, text_x, 187,
              "of every QR page.", 30, PSP_THEME_TEXT, 1);
    draw_text(pixels, width, height, stride, text_x, 207,
              "Report ID", 20, PSP_THEME_TEXT_MUTED, 1);
    draw_text(pixels, width, height, stride, text_x, 220,
              view->report_id, 12, PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride, text_x, 243,
              "L/R Page", 20, accent, 1);
    draw_text(pixels, width, height, stride, text_x, 256,
              "Up/Down Part  O Back", 28, PSP_THEME_TEXT_BODY, 1);
}

/*
 * Native-surface furniture. Both surfaces own the whole panel, so they draw
 * their own ground, a compact device status line, and the single bottom hint
 * line the one-legend rule allows.
 */
#define UI_SURFACE_STATUS_HEIGHT 18
#define UI_SURFACE_HINT_HEIGHT 15

static uint16_t ui_fade_step(uint16_t token, unsigned step);

/* `fade` is the boot entrance's settle step; every ordinary caller passes
   2, the settled value, and pays nothing for it. */
static void draw_surface_status(
    uint16_t *pixels, int width, int height,
    int stride, uint16_t accent, const char *heading, unsigned fade)
{
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, UI_SURFACE_STATUS_HEIGHT},
              PSP_THEME_CHROME_BAR, 4);
    /*
     * One vertical rule for the whole bar: cap-height text drawn at y=3
     * puts the 11px face's caps on rows 5..12, which shares an optical
     * centre with the battery outline's rows 4..12. Heading, clock and
     * battery then read as a single line.
     */
    draw_text(pixels, width, height, stride, PSP_THEME_SPACE_L, 3,
              heading, 20, ui_fade_step(PSP_THEME_TEXT_BODY, fade), 1);
    if (!device_status.valid) return;
    UiRect battery = {width - 34, 4, 17, 9};
    /* The clock right-aligns against whatever is next to it: the battery
       when there is no signal to show, or the wifi cluster when there is. */
    int clock_anchor = battery.x - PSP_THEME_SPACE_S;
    if (device_status.wifi_valid) {
        /* Four ascending bars sharing the battery's 4..13 row band, filled
           on the active accent and empty on TEXT_FAINT -- absent, not zero,
           whenever the link is not READY (handled by `wifi_valid`). */
        const int bar_width = 2, bar_gap = 1;
        int cluster_width = (int) UI_WIFI_BARS_MAX * bar_width
            + ((int) UI_WIFI_BARS_MAX - 1) * bar_gap;
        int cluster_left = battery.x - PSP_THEME_SPACE_S - cluster_width;
        int base_y = battery.y + battery.height;
        for (unsigned i = 0; i < UI_WIFI_BARS_MAX; i++) {
            int bar_height = 3 + 2 * (int) i;
            int bar_x = cluster_left + (int) i * (bar_width + bar_gap);
            uint16_t color = i < device_status.wifi_bars
                ? ui_fade_step(accent, fade)
                : ui_fade_step(PSP_THEME_TEXT_FAINT, fade);
            fill_rect(pixels, width, height, stride,
                      (UiRect) {bar_x, base_y - bar_height,
                                bar_width, bar_height},
                      color, 4);
        }
        clock_anchor = cluster_left - PSP_THEME_SPACE_S;
    }
    /* Right-align against the neighbour rather than a fixed column:
       the 12-hour string is wider than the 24-hour one. */
    draw_text_right_aligned(
        pixels, width, height, stride, clock_anchor, 3,
        device_status.clock, sizeof(device_status.clock) - 1u,
        ui_fade_step(PSP_THEME_TEXT_BODY, fade), 1, false);
    outline_rect(pixels, width, height, stride, battery,
                 ui_fade_step(PSP_THEME_TEXT_MUTED, fade), 1);
    fill_rect(pixels, width, height, stride,
              (UiRect) {battery.x + battery.width, battery.y + 3, 2, 3},
              ui_fade_step(PSP_THEME_TEXT_MUTED, fade), 4);
    int fill = 13 * (int) device_status.battery_percent / 100;
    if (fill > 0)
        fill_rect(pixels, width, height, stride,
                  (UiRect) {battery.x + 2, battery.y + 2, fill, 5},
                  device_status.battery_percent <= 15u
                      ? PSP_THEME_WARN : accent, 4);
    if (device_status.charging)
        draw_text_bold(pixels, width, height, stride,
                       battery.x + 5, battery.y - 2, "+", 1,
                       PSP_THEME_TEXT, 1);
}

static void draw_surface_hint(
    uint16_t *pixels, int width, int height, int stride, const char *hint,
    unsigned fade)
{
    int top = height - UI_SURFACE_HINT_HEIGHT;
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, top, width, UI_SURFACE_HINT_HEIGHT},
              PSP_THEME_HINT_BAR, 4);
    /* Caps of the label face occupy y+2..y+9, so +2 centres the 8px caps
       in the 15px bar. */
    draw_text(pixels, width, height, stride, PSP_THEME_SPACE_L, top + 2,
              hint, 60, ui_fade_step(PSP_THEME_TEXT_MUTED, fade), 1);
}

static size_t ui_home_tile_count(const PspUiState *ui)
{
    if (ui == NULL || ui->home == NULL) return 0u;
    size_t tiles = ui->home->tile_count;
    return tiles > PSP_UI_HOME_TILE_LIMIT ? PSP_UI_HOME_TILE_LIMIT : tiles;
}

static UiRect ui_home_row_rect(const PspUiState *ui, size_t row)
{
    size_t tiles = ui_home_tile_count(ui);
    if (row < tiles) {
        return (UiRect) {
            UI_HOME_MARGIN
                + (int) (row % UI_HOME_TILE_COLUMNS) * UI_HOME_TILE_STRIDE_X,
            UI_HOME_TILE_TOP
                + (int) (row / UI_HOME_TILE_COLUMNS) * UI_HOME_TILE_STRIDE_Y,
            UI_HOME_TILE_WIDTH, UI_HOME_TILE_HEIGHT
        };
    }
    size_t entry = row - tiles;
    return (UiRect) {
        UI_HOME_MARGIN,
        UI_HOME_CONTINUE_TOP + (int) entry * UI_HOME_CONTINUE_STRIDE,
        480 - 2 * UI_HOME_MARGIN, UI_HOME_CONTINUE_HEIGHT
    };
}

/* Shared ordered ramp matrix; unlike the retired wave it remains useful for
   static accent fills and costs nothing between presentations. */
static const uint8_t ui_ordered_bayer[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/*
 * How much of HOME the boot entrance has assembled. All-zero -- the settled
 * surface -- is what every ordinary composite passes.
 */
typedef struct {
    int tile_rise;      /* pixels the tiles still have to climb */
    unsigned furniture; /* 0 hidden, 1 fading, 2 settled */
    bool entering;
} UiHomeEntrance;

/*
 * One step down the text ramp. The entrance's fade-in is spent in the ramp's
 * own steps rather than per-pixel alpha: the same flat fills, drawn in a
 * dimmer token, instead of a blend over every glyph.
 */
static uint16_t ui_fade_step(uint16_t token, unsigned step)
{
    const uint16_t ramp[4] = {
        PSP_THEME_TEXT, PSP_THEME_TEXT_BODY,
        PSP_THEME_TEXT_MUTED, PSP_THEME_TEXT_FAINT
    };
    if (step >= 2u) return token;
    for (unsigned at = 0; at < 4u; at++) {
        if (ramp[at] != token) continue;
        unsigned down = at + (2u - step);
        return ramp[down > 3u ? 3u : down];
    }
    return token;
}

static void draw_home_entering(const PspUiState *ui, uint16_t *pixels,
                               int width, int height, int stride,
                               UiSurfaceAccent accent,
                               UiHomeEntrance entrance)
{
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, height}, PSP_THEME_GROUND, 4);
    if (entrance.furniture != 0u)
        draw_surface_status(
            pixels, width, height, stride, accent.accent, "TILEFINCH",
            entrance.furniture);

    size_t tiles = ui_home_tile_count(ui);
    size_t rows = ui_home_row_count(ui);
    /* The focus-settle budget is spent brightening the ring the move landed
       on, so a move reads as arriving somewhere rather than teleporting. */
    bool settling = ui->focus_settle_frames != 0;
    for (size_t at = 0; at < tiles; at++) {
        UiRect tile = ui_home_row_rect(ui, at);
        /* The entrance's rise is one offset added to the layout, so the
           tiles that climb are the same tiles hit-testing already knows
           about; nothing about the settled geometry moves. */
        tile.y += entrance.tile_rise;
        bool selected = at == ui->home_selection && !entrance.entering;
        /* The ring is the card's silhouette grown by the stroke, painted
           first and then covered by the card itself. A square outline_rect
           around a radius-5 card left its corner pixels poking out. */
        if (selected)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {tile.x - PSP_THEME_FOCUS_STROKE,
                          tile.y - PSP_THEME_FOCUS_STROKE,
                          tile.width + 2 * PSP_THEME_FOCUS_STROKE,
                          tile.height + 2 * PSP_THEME_FOCUS_STROKE},
                PSP_THEME_RADIUS_CARD + PSP_THEME_FOCUS_STROKE,
                settling ? accent.accent_high : accent.accent, 4);
        fill_round_rect(
            pixels, width, height, stride, tile, PSP_THEME_RADIUS_CARD,
            selected ? PSP_THEME_SURFACE_FOCUS : PSP_THEME_SURFACE, 4);
        /* No per-tile adornment: the title is the tile. */
        draw_text_with_font(
            pixels, width, height, stride,
            tile.x + PSP_THEME_SPACE_M, tile.y + PSP_THEME_SPACE_M + 2,
            ui->home->tiles[at].label, PSP_UI_HOME_LABEL_CAPACITY,
            tile.x + tile.width - PSP_THEME_SPACE_S,
            PSP_THEME_TEXT, 2, NULL, true);
        draw_text_with_font(
            pixels, width, height, stride,
            tile.x + PSP_THEME_SPACE_M, tile.y + 38,
            ui->home->tiles[at].detail, PSP_UI_HOME_DETAIL_CAPACITY,
            tile.x + tile.width - PSP_THEME_SPACE_S,
            PSP_THEME_TEXT_MUTED, 1, NULL, false);
    }

    /* CONTINUE, the status line and the hint are the last things to arrive:
       the tiles are what the entrance is delivering, and the furniture
       around them settles once they have landed. */
    if (entrance.furniture == 0u) return;

    /* The label names the list, not the cards, so it sits on the rows'
       TEXT rail rather than their card edge. */
    draw_text(pixels, width, height, stride,
              UI_HOME_MARGIN + PSP_THEME_SPACE_M,
              UI_HOME_CONTINUE_LABEL_Y, "CONTINUE", 8,
              ui_fade_step(PSP_THEME_TEXT_MUTED, entrance.furniture), 1);
    if (rows == tiles) {
        draw_text(
            pixels, width, height, stride,
            UI_HOME_MARGIN + PSP_THEME_SPACE_M,
            UI_HOME_CONTINUE_TOP + 4, "NOTHING OPEN YET", 20,
            ui_fade_step(PSP_THEME_TEXT_BODY, entrance.furniture), 1);
    }
    for (size_t at = tiles; at < rows; at++) {
        UiRect row = ui_home_row_rect(ui, at);
        bool selected = at == ui->home_selection && !entrance.entering;
        const PspUiHomeEntry *entry = &ui->home->continues[at - tiles];
        if (selected) {
            fill_round_rect(
                pixels, width, height, stride, row, PSP_THEME_RADIUS_ROW,
                PSP_THEME_SURFACE_FOCUS, 4);
            fill_round_edge_bar(
                pixels, width, height, stride, row, PSP_THEME_RADIUS_ROW,
                PSP_THEME_FOCUS_STROKE,
                settling ? accent.accent_high : accent.accent);
        }
        draw_text_with_font(
            pixels, width, height, stride,
            row.x + PSP_THEME_SPACE_M, row.y + 4, entry->label,
            PSP_UI_HOME_LABEL_CAPACITY, row.x + row.width - 150,
            ui_fade_step(selected ? PSP_THEME_TEXT : PSP_THEME_TEXT_BODY,
                         entrance.furniture), 1, NULL, selected);
        draw_text_right_aligned(
            pixels, width, height, stride,
            row.x + row.width - PSP_THEME_SPACE_M, row.y + 4,
            entry->detail, PSP_UI_HOME_DETAIL_CAPACITY,
            ui_fade_step(PSP_THEME_TEXT_MUTED, entrance.furniture),
            1, false);
    }

    draw_surface_hint(
        pixels, width, height, stride,
        ui->home != NULL && !ui->home->engine_ready
            ? "X OPEN WHEN READY   SELECT MENU"
            : "X OPEN   START SEARCH   SELECT MENU",
        entrance.furniture);
}

static TILEFINCH_OUT_OF_LINE void draw_home(
                      const PspUiState *ui, uint16_t *pixels, int width,
                      int height, int stride, UiSurfaceAccent accent)
{
    draw_home_entering(
        ui, pixels, width, height, stride, accent,
        (UiHomeEntrance) {0, 2u, false});
}

static const char *ui_collection_section_name(unsigned section)
{
    switch (section % PSP_UI_COLLECTION_SECTION_COUNT) {
        case PSP_UI_COLLECTION_SAVED: return "SAVED";
        case PSP_UI_COLLECTION_BOOKMARKS: return "BOOKMARKS";
        case PSP_UI_COLLECTION_HISTORY: return "HISTORY";
        case PSP_UI_COLLECTION_DOWNLOADS: return "DOWNLOADS";
        case PSP_UI_COLLECTION_SCREENSHOTS: return "SHOTS";
        default: return "SAVED";
    }
}

static UiRect ui_collections_row_rect(const PspUiState *ui, size_t row)
{
    size_t first = ui->collections_first_row;
    return (UiRect) {
        UI_HOME_MARGIN,
        UI_COLLECTIONS_ROW_TOP
            + (int) (row - first) * UI_COLLECTIONS_ROW_STRIDE,
        480 - 2 * UI_HOME_MARGIN, UI_COLLECTIONS_ROW_HEIGHT
    };
}

static void draw_collection_icon(uint16_t *pixels, int width, int height,
                                 int stride, int x, int y,
                                 const unsigned char *rgba)
{
    if (rgba == NULL) return;
    for (int row = 0; row < (int) OFFLINE_LIBRARY_APP_ICON_EDGE; row++) {
        for (int column = 0; column < (int) OFFLINE_LIBRARY_APP_ICON_EDGE;
             column++) {
            const unsigned char *source = rgba
                + ((size_t) row * OFFLINE_LIBRARY_APP_ICON_EDGE + column) * 4u;
            unsigned parts = (source[3] * UI_BLEND_PARTS + 127u) / 255u;
            if (parts == 0 || x + column < 0 || x + column >= width
                || y + row < 0 || y + row >= height) continue;
            uint16_t color = rgb565(source[0], source[1], source[2]);
            uint16_t *target = pixels + (size_t) (y + row) * stride
                + x + column;
            *target = parts == UI_BLEND_PARTS
                ? color : blend565(color, *target, parts);
        }
    }
}

static TILEFINCH_OUT_OF_LINE void draw_collections(
                             const PspUiState *ui, uint16_t *pixels,
                             int width, int height, int stride,
                             UiSurfaceAccent accent)
{
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, height}, PSP_THEME_GROUND, 4);
    draw_surface_status(
        pixels, width, height, stride, accent.accent, "COLLECTIONS", 2u);

    /* Section strip: the same L/R convention the options overlay uses. */
    int section_x = UI_HOME_MARGIN;
    for (unsigned at = 0; at < PSP_UI_COLLECTION_SECTION_COUNT; at++) {
        const char *name = ui_collection_section_name(at);
        int text_width = chrome_text_width_bytes(name, strlen(name), 1, true);
        UiRect chip = {
            section_x, UI_COLLECTIONS_SECTION_TOP,
            text_width + 2 * PSP_THEME_SPACE_M, UI_COLLECTIONS_SECTION_HEIGHT
        };
        bool active = at == ui->collections_section;
        if (active)
            fill_round_rect(
                pixels, width, height, stride, chip,
                PSP_THEME_RADIUS_CHIP, accent.accent, 4);
        draw_text_with_font(
            pixels, width, height, stride,
            chip.x + PSP_THEME_SPACE_M, chip.y + 3, name, 12,
            /* The chip is sized to its own label, so the ellipsis reserve
               the shared text helper applies would clip a name that fits. */
            chip.x + chip.width + 3 * ui_ellipsis_advance(1, active, NULL),
            active ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_MUTED,
            1, NULL, active);
        section_x += chip.width + PSP_THEME_SPACE_XS;
    }
    size_t rows = ui_collections_row_count(ui);
    char count[16];
    snprintf(count, sizeof(count), "%u", (unsigned) rows);
    /* Same right rail the rows' trailing details use, so the column has
       one edge instead of two 8px apart. */
    draw_text_right_aligned(
        pixels, width, height, stride,
        width - UI_HOME_MARGIN - PSP_THEME_SPACE_M,
        UI_COLLECTIONS_SECTION_TOP + 3, count, 8,
        PSP_THEME_TEXT_FAINT, 1, false);
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, UI_COLLECTIONS_ROW_TOP - 6, width, 1},
              PSP_THEME_LINE, 4);

    if (rows == 0) {
        const char *empty = ui->collections == NULL
                || ui->collections->empty_message == NULL
            ? "NOTHING HERE YET" : ui->collections->empty_message;
        draw_text(pixels, width, height, stride, UI_HOME_MARGIN,
                  UI_COLLECTIONS_ROW_TOP + 8, empty, 40,
                  PSP_THEME_TEXT_MUTED, 1);
        draw_surface_hint(
            pixels, width, height, stride, "L/R SECTION   O BACK", 2u);
        return;
    }

    size_t first = ui->collections_first_row;
    size_t end = first + UI_COLLECTIONS_VISIBLE_ROWS;
    if (end > rows) end = rows;
    bool confirming = false;
    for (size_t at = first; at < end; at++) {
        const PspUiCollectionsRow *row = &ui->collections->rows[at];
        UiRect box = ui_collections_row_rect(ui, at);
        bool selected = at == ui->collections_selection;
        bool activating = selected
            && ui->collections_delete_confirmation
                   == UI_COLLECTIONS_ACTIVATION_FEEDBACK;
        bool confirm = selected
            && ui->collections_delete_confirmation == (uint8_t) (at + 1u);
        if (confirm) confirming = true;
        if (selected) {
            fill_round_rect(
                pixels, width, height, stride, box, PSP_THEME_RADIUS_ROW,
                activating
                    ? blend565(accent.accent, PSP_THEME_SURFACE_FOCUS, 2)
                    : PSP_THEME_SURFACE_FOCUS,
                4);
            fill_round_edge_bar(
                pixels, width, height, stride, box, PSP_THEME_RADIUS_ROW,
                PSP_THEME_FOCUS_STROKE,
                confirm ? PSP_THEME_WARN
                        : (ui->focus_settle_frames != 0
                               ? accent.accent_high : accent.accent));
        }
        int trailing_width = row->trailing[0] == '\0' ? 0
            : chrome_text_width_bytes(
                  row->trailing, strlen(row->trailing), 2, false)
              + PSP_THEME_SPACE_L;
        int content_x = box.x + PSP_THEME_SPACE_M;
        if (row->app_theme_valid) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {content_x, box.y + 7, 5, 22}, 2,
                row->app_theme_rgb565, 4);
            content_x += 9;
        }
        if (row->icon_rgba != NULL) {
            draw_collection_icon(
                pixels, width, height, stride, content_x, box.y + 5,
                row->icon_rgba);
            content_x += (int) OFFLINE_LIBRARY_APP_ICON_EDGE
                + PSP_THEME_SPACE_S;
        }
        draw_text_with_font(
            pixels, width, height, stride,
            content_x, box.y + 1, row->title, 64,
            box.x + box.width - PSP_THEME_SPACE_M - trailing_width,
            selected ? PSP_THEME_TEXT : PSP_THEME_TEXT_BODY, 2, NULL,
            selected);
        draw_text_with_font(
            pixels, width, height, stride,
            content_x, box.y + UI_COLLECTIONS_ROW_DETAIL_DY,
            confirm
                ? (row->is_web_app ? "PRESS SQUARE AGAIN TO UNINSTALL"
                                   : "PRESS SQUARE AGAIN TO DELETE")
                : row->detail, 64,
            box.x + box.width - PSP_THEME_SPACE_M - trailing_width,
            confirm ? PSP_THEME_WARN : PSP_THEME_TEXT_MUTED, 2, NULL,
            false);
        if (row->trailing[0] != '\0')
            draw_text_right_aligned(
                pixels, width, height, stride,
                /* Shares the title's baseline rather than floating
                   between the row's two lines: on the selected row the
                   white title and its trailing figure then read as one
                   line. */
                box.x + box.width - PSP_THEME_SPACE_M, box.y + 1,
                row->trailing, PSP_UI_COLLECTIONS_TRAILING_CAPACITY,
                selected ? PSP_THEME_TEXT_BODY : PSP_THEME_TEXT_MUTED,
                2, false);
    }
    if (first != 0)
        draw_vertical_chevron(
            pixels, width, height, stride, width - 10,
            UI_COLLECTIONS_ROW_TOP + 4, -1, PSP_THEME_TEXT_FAINT, 4);
    if (end < rows)
        draw_vertical_chevron(
            pixels, width, height, stride, width - 10,
            UI_COLLECTIONS_ROW_TOP
                + (int) (UI_COLLECTIONS_VISIBLE_ROWS - 1u)
                      * UI_COLLECTIONS_ROW_STRIDE + 4,
            1, PSP_THEME_TEXT_FAINT, 4);

    bool deletable = ui->collections_selection < rows
        && ui->collections->rows[ui->collections_selection].deletable;
    const PspUiCollectionsRow *selected =
        ui->collections_selection < rows
            ? &ui->collections->rows[ui->collections_selection] : NULL;
    const char *primary = "X OPEN";
    if (ui->collections_section == PSP_UI_COLLECTION_DOWNLOADS
        && selected != NULL) {
        if (selected->offline_state == OFFLINE_ITEM_DOWNLOADING)
            primary = "X PAUSE";
        else if (selected->offline_state == OFFLINE_ITEM_READY)
            primary = "X PLAY";
        else
            primary = "X RESUME";
    }
    char hint[64];
    snprintf(hint, sizeof(hint), "%s%s   L/R SECTION",
             primary, deletable
                 ? (selected != NULL && selected->is_web_app
                        ? "   SQUARE UNINSTALL" : "   SQUARE DELETE") : "");
    draw_surface_hint(
        pixels, width, height, stride,
        confirming
            ? (selected != NULL && selected->is_web_app
                   ? "SQUARE UNINSTALL   O CANCEL"
                   : "SQUARE DELETE   O CANCEL")
            : hint, 2u);
}

/*
 * A dithered accent -> accent-hi ramp across `rect`, using the same 4x4
 * ordered matrix the wave band uses. Ordered, not error-diffused: a pixel is
 * a pure function of (x, y), so a band that slides does not shimmer.
 */
static void fill_accent_ramp(uint16_t *pixels, int width, int height,
                             int stride, UiRect rect,
                             uint16_t from, uint16_t to)
{
    if (rect.width <= 0 || rect.height <= 0) return;
    unsigned fr = tilefinch_rgb565_red_code(from);
    unsigned fg = tilefinch_rgb565_green_code(from);
    unsigned fb = tilefinch_rgb565_blue_code(from);
    unsigned tr = tilefinch_rgb565_red_code(to);
    unsigned tg = tilefinch_rgb565_green_code(to);
    unsigned tb = tilefinch_rgb565_blue_code(to);
    for (int y = 0; y < rect.height; y++) {
        for (int x = 0; x < rect.width; x++) {
            unsigned weight = (unsigned) (x * 16) / (unsigned) rect.width;
            unsigned threshold =
                ui_ordered_bayer[((unsigned) y & 3u) * 4u
                                 + ((unsigned) x & 3u)];
            unsigned red =
                (tr * weight + fr * (16u - weight) + threshold) / 16u;
            unsigned green =
                (tg * weight + fg * (16u - weight) + threshold) / 16u;
            unsigned blue =
                (tb * weight + fb * (16u - weight) + threshold) / 16u;
            if (red > 31u) red = 31u;
            if (green > 63u) green = 63u;
            if (blue > 31u) blue = 31u;
            put_pixel(pixels, width, height, stride,
                      rect.x + x, rect.y + y,
                      tilefinch_rgb565_pack_codes(red, green, blue));
        }
    }
}

static void draw_loading(const PspUiState *ui, uint16_t *pixels, int width,
                         int height, int stride, uint16_t accent)
{
    if (!ui->loading && !ui->page_activation_busy) return;
    /* The upper activity line always moves, so a stalled/unknown-length
       request never looks frozen. The lower line is monotonic overall
       completion; both occupy the original three-pixel loading strip. */
    UiSurfaceAccent ramp = ui_surface_accent(
        (BrowserChromeTheme) ui->chrome_theme);
    (void) accent;
    int segment = width / 4;
    int range = width + segment;
    int position = (int) (ui->loading_phase * 4u % (unsigned) range);
    int y = ui->chrome_visible ? UI_TOP_HEIGHT : 0;
    fill_accent_ramp(pixels, width, height, stride,
                     (UiRect) { position - segment, y, segment, 2 },
                     ramp.accent, ramp.accent_high);
    if (ui->loading && ui->progress_per_mille >= 0) {
        int filled = width * ui->progress_per_mille / 1000;
        fill_rect(pixels, width, height, stride,
                  (UiRect) { 0, y + 2, width, 1 }, PSP_THEME_LINE, 4);
        fill_accent_ramp(pixels, width, height, stride,
                         (UiRect) { 0, y + 2, filled, 1 },
                         ramp.accent, ramp.accent_high);
    }
}

static void draw_danzeff_character(
    uint16_t *pixels, int width, int height, int stride,
    int x, int y, char character, uint16_t color, int scale)
{
    char label[2] = {character, '\0'};
    if (character == '\0') return;
    if (character == '\b') label[0] = '<';
    else if (character == ' ') label[0] = '_';
    draw_text(pixels, width, height, stride, x, y, label, 1, color, scale);
}

static TILEFINCH_OUT_OF_LINE void draw_text_entry(
    const PspUiState *ui, uint16_t *pixels, int width, int height,
    int stride, uint16_t panel, uint16_t accent, uint16_t text,
    uint16_t muted)
{
    const PspUiTextEntryView *view = ui->text_entry;
    if (view == NULL || view->text == NULL) return;
    /* Keep the panel and both legend baselines above the physical LCD edge.
       The former 7..265 extent left the second bitmap-font descent in the
       last scanlines, where overscan and the panel clip made it look cut. */
    UiRect box = {8, 5, width - 16, height - 18};
    /* Text entry owns a synchronous controller loop and only presents when
       its contents change. Applying the ordinary overlay entrance transform
       here would therefore freeze the modal at its first, downward-offset
       animation frame until the user moved a control. Draw it settled. */
    uint16_t field = PSP_THEME_SURFACE;
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    /* Keep the control legends out of the LCD's least reliable bottom
       scanlines. The keyboard is lifted below to leave this full-height
       hint region without overlapping its selected-cell expansion. */
    draw_panel_hint_bar(pixels, width, height, stride, box,
                        box.y + box.height - 40);
    draw_text(
        pixels, width, height, stride, box.x + 14, box.y + 11,
        view->description == NULL ? "Enter text" : view->description,
        30, text, 2);
    draw_text(
        pixels, width, height, stride, box.x + box.width - 114,
        box.y + 14,
        view->numbers
            ? (view->shifted ? "Symbols" : "Numbers")
            : (view->shifted ? "Uppercase" : "Lowercase"),
        12, accent, 1);

    UiRect input = {box.x + 12, box.y + 39, box.width - 24, 30};
    if (view->replace_all) {
        fill_round_rect(pixels, width, height, stride, input,
                        PSP_THEME_RADIUS_ROW + PSP_THEME_FOCUS_STROKE,
                        accent, 4);
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {input.x + PSP_THEME_FOCUS_STROKE,
                      input.y + PSP_THEME_FOCUS_STROKE,
                      input.width - 2 * PSP_THEME_FOCUS_STROKE,
                      input.height - 2 * PSP_THEME_FOCUS_STROKE},
            PSP_THEME_RADIUS_ROW, field, 4);
    } else {
        fill_round_rect(pixels, width, height, stride, input,
                        PSP_THEME_RADIUS_ROW, field, 4);
    }
    size_t length = strlen(view->text);
    size_t cursor = view->cursor;
    if (cursor > length) cursor = length;
    const size_t visible_characters = 69;
    size_t start = cursor > visible_characters / 2u
        ? cursor - visible_characters / 2u : 0u;
    if (length - start > visible_characters
        && cursor + visible_characters / 2u >= length)
        start = length - visible_characters;
    while (start < length
           && ((unsigned char) view->text[start] & 0xc0u)
                  == 0x80u) start++;
    draw_text(
        pixels, width, height, stride, input.x + 9, input.y + 10,
        view->text + start, visible_characters, text, 1);
    if (!view->replace_all) {
        size_t cursor_offset = cursor >= start ? cursor - start : 0;
        if (cursor_offset > visible_characters)
            cursor_offset = visible_characters;
        int cursor_x = input.x + 9 + chrome_text_width_bytes(
            view->text + start, cursor_offset, 1, false);
        if (cursor_x > input.x + input.width - 4)
            cursor_x = input.x + input.width - 4;
        fill_rect(
            pixels, width, height, stride,
            (UiRect) {cursor_x, input.y + 8, 1, 16}, accent, 4);
    }

    draw_text(
        pixels, width, height, stride, box.x + 15, box.y + 80,
        "BOOKMARKS AND HISTORY", 24, muted, 1);
    if (view->suggestion_count == 0) {
        draw_text(
            pixels, width, height, stride, box.x + 15, box.y + 102,
            view->replace_all
                ? "TYPE TO FIND A SAVED PAGE"
                : "NO SAVED PAGE MATCHES YET",
            34, muted, 1);
    }
    size_t suggestion_count = view->suggestion_count;
    if (suggestion_count > PSP_UI_TEXT_SUGGESTION_LIMIT)
        suggestion_count = PSP_UI_TEXT_SUGGESTION_LIMIT;
    for (size_t i = 0; i < suggestion_count; i++) {
        /* The selected row carries a second URL line. Leave real descent
           space below it instead of ending the highlight on the glyphs. */
        int row_y = box.y + 98 + (int) i * 32;
        bool selected = view->suggestion_selection == (int) i;
        if (selected) {
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 11, row_y - 5, 282, 30},
                PSP_THEME_RADIUS_ROW, accent, 4);
        }
        const BrowserProfileSuggestion *suggestion =
            view->suggestions == NULL ? NULL : &view->suggestions[i];
        const char *suggestion_text = suggestion == NULL
            ? "" : (suggestion->title == NULL
                         || suggestion->title[0] == '\0'
                     ? suggestion->url : suggestion->title);
        bool bookmark = suggestion != NULL && suggestion->bookmark;
        /* Bookmark carries the accent, history a plain raised surface; on
           the accent-filled selected row both invert to the dark ink so the
           chip never fights the fill it sits on. */
        UiRect source_badge = {box.x + 17, row_y - 2, 14, 12};
        fill_round_rect(
            pixels, width, height, stride, source_badge,
            PSP_THEME_RADIUS_CHIP,
            selected ? PSP_THEME_ON_ACCENT
                     : (bookmark ? accent : PSP_THEME_SURFACE_FOCUS), 4);
        const char *source_label = bookmark ? "B" : "H";
        int source_label_width = chrome_text_width_bytes(
            source_label, 1u, 1, true);
        draw_text_bold(
            pixels, width, height, stride,
            source_badge.x + (source_badge.width - source_label_width) / 2,
            source_badge.y + 2, source_label, 1,
            selected ? PSP_THEME_TEXT
                     : (bookmark ? PSP_THEME_ON_ACCENT
                                 : PSP_THEME_TEXT_BODY), 1);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 37, row_y,
            suggestion_text == NULL ? "" : suggestion_text, 39,
            box.x + 293,
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY,
            1, NULL, true);
        if (selected && suggestion != NULL && suggestion->url != NULL)
            draw_text_with_font(
                pixels, width, height, stride, box.x + 37, row_y + 11,
                suggestion->url, 39, box.x + 293,
                PSP_THEME_ON_ACCENT, 1, NULL, false);
    }

    const int keyboard_x = box.x + box.width - 143;
    const int keyboard_y = box.y + 76;
    /* The keyboard ends above the two-line control legend even when the
       selected cell expands by three pixels. */
    const int cell_size = 44;
    const int gap = 2;
    for (unsigned pass = 0; pass < 2u; pass++) {
      for (unsigned cell = 0; cell < 9u; cell++) {
        int x = keyboard_x + (int) (cell % 3u) * (cell_size + gap);
        int y = keyboard_y + (int) (cell / 3u) * (cell_size + gap);
        bool selected = cell == view->cell;
        if (selected != (pass == 1u)) continue;
        int expansion = selected ? 3 : 0;
        int character_scale = selected ? 2 : 1;
        uint16_t cell_color = selected ? accent : field;
        uint16_t character_color =
            selected ? PSP_THEME_ON_ACCENT : PSP_THEME_TEXT_BODY;
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {x - expansion, y - expansion,
                      cell_size + expansion * 2,
                      cell_size + expansion * 2},
            PSP_THEME_RADIUS_ROW, cell_color, 4);
        DanzeffInputMode mode = view->numbers
            ? DANZEFF_INPUT_NUMBERS : DANZEFF_INPUT_LETTERS;
        draw_danzeff_character(
            pixels, width, height, stride,
            x + (selected ? 18 : 20), y + (selected ? 0 : 4),
            danzeff_input_character(
                mode, view->shifted, cell,
                DANZEFF_INPUT_TRIANGLE), character_color, character_scale);
        draw_danzeff_character(
            pixels, width, height, stride,
            x + (selected ? 2 : 6), y + (selected ? 16 : 18),
            danzeff_input_character(
                mode, view->shifted, cell,
                DANZEFF_INPUT_SQUARE), character_color, character_scale);
        draw_danzeff_character(
            pixels, width, height, stride,
            x + (selected ? 18 : 20), y + (selected ? 32 : 31),
            danzeff_input_character(
                mode, view->shifted, cell,
                DANZEFF_INPUT_CROSS), character_color, character_scale);
        draw_danzeff_character(
            pixels, width, height, stride,
            x + (selected ? 34 : 34), y + (selected ? 16 : 18),
            danzeff_input_character(
                mode, view->shifted, cell,
                DANZEFF_INPUT_CIRCLE), character_color, character_scale);
      }
    }
    draw_text(
        pixels, width, height, stride, box.x + 14,
        box.y + box.height - 35,
        "Stick center + TRI Delete   L Mode   R Shift", 48, muted, 1);
    draw_text(
        pixels, width, height, stride, box.x + 14,
        box.y + box.height - 21,
        view->allow_submit
            ? "Start Done   R+Start Enter   Select Cancel"
            : (view->navigation
                   ? "X Pick   Start Go   Select Cancel"
                   : "X Pick   Start Done   Select Cancel"),
        52,
        PSP_THEME_TEXT_BODY, 1);
}

static TILEFINCH_OUT_OF_LINE void draw_find(
                      const PspUiState *ui, uint16_t *pixels, int width,
                      int height, int stride, uint16_t panel,
                      uint16_t accent, uint16_t text, uint16_t muted)
{
    const PspUiFindView *view = ui->find_view;
    if (view == NULL) return;
    UiRect box = {14, height - UI_BOTTOM_HEIGHT - 48, width - 28, 42};
    ui_apply_overlay_motion(ui, &box);
    (void) panel;
    draw_panel_shell(pixels, width, height, stride, box);
    draw_panel_hint_bar(pixels, width, height, stride, box, box.y + 26);
    fill_round_edge_bar(pixels, width, height, stride, box,
                        PSP_THEME_RADIUS_PANEL, 4, accent);
    char count[24];
    if (view->match_count == 0) {
        snprintf(count, sizeof(count), "NO MATCHES");
    } else {
        snprintf(count, sizeof(count), "%u / %u%s",
                 (unsigned) view->selected + 1u,
                 (unsigned) view->match_count,
                 view->truncated ? "+" : "");
    }
    draw_text(pixels, width, height, stride, box.x + 13, box.y + 8,
              "FIND", 6, accent, 1);
    draw_text(pixels, width, height, stride, box.x + 54, box.y + 8,
              view->query, 48, text, 1);
    draw_text(pixels, width, height, stride, box.x + box.width - 82,
              box.y + 8, count, 13, PSP_THEME_TEXT_BODY, 1);
    draw_text(pixels, width, height, stride, box.x + 13, box.y + 26,
              view->wrapped
                  ? "WRAPPED  UP/DOWN MATCH  L/R SCROLL"
                  : "UP/DOWN MATCH  L/R SCROLL  X EDIT",
              46, muted, 1);
}

static void draw_toast(const PspUiState *ui, uint16_t *pixels, int width,
                       int height, int stride, uint16_t panel,
                       uint16_t accent, uint16_t text)
{
    if (ui->toast_frames == 0 || ui->status[0] == '\0') return;
    char presented[PSP_UI_STATUS_CAPACITY];
    copy_string(presented, sizeof(presented), ui->status);
    bool has_lowercase = false;
    for (size_t at = 0; presented[at] != '\0'; at++) {
        if (presented[at] >= 'a' && presented[at] <= 'z') {
            has_lowercase = true;
            break;
        }
    }
    if (!has_lowercase) {
        bool first_letter = true;
        for (size_t at = 0; presented[at] != '\0'; at++) {
            unsigned char value = (unsigned char) presented[at];
            if (value >= 'A' && value <= 'Z') {
                if (!first_letter)
                    presented[at] = (char) tolower(value);
                first_letter = false;
            }
        }
    }
    char *embedded_second_line = strchr(presented, '\n');
    const char *second_line = embedded_second_line;
    if (embedded_second_line != NULL) {
        *embedded_second_line++ = '\0';
        second_line = embedded_second_line;
        /* Toasts intentionally support one explanatory continuation line.
           Flatten any later line break rather than letting an error grow an
           unbounded modal over the page. */
        char *extra_line = strchr(embedded_second_line, '\n');
        if (extra_line != NULL) *extra_line = ' ';
    }
    static const char *const tls_guidance[] = {
        "",
        "Try correcting PSP date/time, then retry",
        "Try another Wi-Fi or update Tilefinch",
        "Secure connection was redirected. Try another Wi-Fi network",
        "Secure connection unsupported. Update Tilefinch",
        "Open Help & diagnostics for details"
    };
    if (ui->tls_toast_guidance > TILEFINCH_TLS_GUIDANCE_NONE
        && ui->tls_toast_guidance <= TILEFINCH_TLS_GUIDANCE_DETAILS) {
        second_line = tls_guidance[ui->tls_toast_guidance];
    }
    if (ui->captive_portal_suggested)
        second_line = "X Open Wi-Fi sign-in  O Cancel";
    bool multiline = second_line != NULL && second_line[0] != '\0';
    int scale = browser_ui_scale(ui);
    if (multiline && scale > 1) {
        size_t first_length = strlen(presented);
        size_t second_length = strlen(second_line);
        int first_scaled = chrome_text_width_bytes(
            presented, first_length, scale, false);
        int second_scaled = chrome_text_width_bytes(
            second_line, second_length, scale, false);
        if (first_scaled > width - 48 || second_scaled > width - 48)
            scale = 1;
    }
    /* Derive the final text capacity from the actual toast interior. Large
       single-line status remains conservatively cell-bounded. A multiline
       diagnostic gets a slightly longer bound when the proportional face's
       measured pixels fit, and falls back to the cell bound on a narrow
       surface. */
    size_t preferred_maximum = multiline
        ? PSP_UI_MULTILINE_TOAST_CHARACTER_LIMIT
        : (scale == 2 ? PSP_UI_LARGE_TOAST_CHARACTER_LIMIT : 56u);
    size_t viewport_maximum = width > 48
        ? (size_t) (width - 48) / (size_t) (6 * scale)
        : 1u;
    size_t maximum = preferred_maximum;
    if (!multiline && maximum > viewport_maximum)
        maximum = viewport_maximum;
    size_t first_characters = strlen(presented);
    size_t second_characters = second_line == NULL ? 0u : strlen(second_line);
    size_t characters = first_characters > second_characters
        ? first_characters : second_characters;
    if (characters > maximum) characters = maximum;
    int first_width = chrome_text_width_bytes(
        presented, first_characters < maximum ? first_characters : maximum,
        scale, false);
    int second_width = second_line == NULL ? 0 : chrome_text_width_bytes(
        second_line,
        second_characters < maximum ? second_characters : maximum,
        scale, false);
    int text_width = first_width > second_width ? first_width : second_width;
    if (multiline && text_width > width - 48) {
        maximum = preferred_maximum < viewport_maximum
            ? preferred_maximum : viewport_maximum;
        first_width = chrome_text_width_bytes(
            presented,
            first_characters < maximum ? first_characters : maximum,
            scale, false);
        second_width = second_line == NULL ? 0 : chrome_text_width_bytes(
            second_line,
            second_characters < maximum ? second_characters : maximum,
            scale, false);
        text_width = first_width > second_width ? first_width : second_width;
    }
    int box_width = multiline
        ? text_width + 24 : (int) characters * 6 * scale + 24;
    if (box_width > width - 24) box_width = width - 24;
    int reserved_bottom = ui->screen == PSP_UI_SCREEN_PAGE
                           && ui->chrome_visible
        ? browser_bottom_height(ui)
        : (psp_ui_screen_is_native_surface(ui->screen)
               ? UI_SURFACE_HINT_HEIGHT : 0);
    int box_height = multiline
        ? (scale == 2 ? 52 : 38) : (scale == 2 ? 32 : 23);
    UiRect box = { (width - box_width) / 2,
                   height - reserved_bottom - box_height - 8,
                   box_width, box_height };
    unsigned toast_frames = ui->toast_entry_frames;
    if (toast_frames > PSP_THEME_MOTION_TOAST_FRAMES)
        toast_frames = PSP_THEME_MOTION_TOAST_FRAMES;
    box.y += (int) ui_toast_rise[toast_frames];
    (void) panel;
    (void) text;
    fill_round_rect(
        pixels, width, height, stride,
        (UiRect) {box.x + 2, box.y + 3, box.width, box.height},
        PSP_THEME_RADIUS_PANEL, rgb565(0, 0, 0), 2);
    fill_round_rect(pixels, width, height, stride, box,
                    PSP_THEME_RADIUS_PANEL, PSP_THEME_PANEL, 4);
    fill_round_edge_bar(pixels, width, height, stride, box,
                        PSP_THEME_RADIUS_PANEL, 4, accent);
    int first_y = box.y + (multiline
        ? (scale == 2 ? 5 : 4) : (scale == 2 ? 9 : 8));
    draw_text(pixels, width, height, stride, box.x + 12, first_y,
              presented, maximum, PSP_THEME_TEXT, scale);
    if (multiline) {
        draw_text(
            pixels, width, height, stride, box.x + 12,
            box.y + (scale == 2 ? 27 : 20), second_line, maximum,
            PSP_THEME_TEXT, scale);
    }
}

typedef struct {
    const PspUiState *ui;
    uint16_t *pixels;
    int width;
    int height;
    int stride;
    UiChromePalette palette;
} PspUiCompositeContext;

#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
static PspUiCompositeTiming psp_ui_validation_timing;
#endif

/* Native surfaces are common during a session but absent from the steady
   browser frame. Keep their substantial painters out of the browser's
   instruction-cache footprint without mislabeling them as cold code. */
static TILEFINCH_OUT_OF_LINE void psp_ui_composite_native(
    const PspUiCompositeContext *context)
{
    const PspUiState *ui = context->ui;
    UiSurfaceAccent surface_accent = ui_surface_accent(
        (BrowserChromeTheme) ui->chrome_theme);
    if (ui->screen == PSP_UI_SCREEN_HOME) {
        draw_home(ui, context->pixels, context->width, context->height,
                  context->stride, surface_accent);
    } else {
        draw_collections(ui, context->pixels, context->width,
                         context->height, context->stride, surface_accent);
    }
    draw_cursor(ui, context->pixels, context->width, context->height,
                context->stride, surface_accent.accent);
    draw_toast(ui, context->pixels, context->width, context->height,
               context->stride, PSP_THEME_PANEL, surface_accent.accent,
               PSP_THEME_TEXT);
}

/* This is the resident browser-composition core. Mode-specific painters are
   deliberately out of line; their order here remains the visible z-order. */
static TILEFINCH_OUT_OF_LINE void psp_ui_composite_browser(
    const PspUiCompositeContext *context)
{
    const PspUiState *ui = context->ui;
    uint16_t *pixels = context->pixels;
    int width = context->width;
    int height = context->height;
    int stride = context->stride;
    uint16_t panel = context->palette.panel;
    uint16_t accent = context->palette.accent;
    uint16_t text = context->palette.text;
    uint16_t muted = context->palette.muted;
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    uint64_t timing_started_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t timing_focus_us = timing_started_us;
    uint64_t timing_chrome_us = timing_started_us;
    uint64_t timing_cursor_us = timing_started_us;
    uint64_t timing_screen_us = timing_started_us;
    uint64_t timing_toast_us = timing_started_us;
#endif

    if (ui->screen == PSP_UI_SCREEN_DIAGNOSTIC_QR) {
        draw_diagnostic_qr(ui, pixels, width, height, stride, accent);
        return;
    }

    if (ui->screen != PSP_UI_SCREEN_FIND)
        draw_focus(ui, pixels, width, height, stride, accent);
    draw_scrollbar(ui, pixels, width, height, stride, accent);
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    timing_focus_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (ui->chrome_visible) {
        draw_cached_top_bar(ui, pixels, width, height, stride, panel, accent,
                            text, muted);
        if (ui->screen == PSP_UI_SCREEN_PAGE)
            draw_cached_bottom_bar(
                ui, pixels, width, height, stride, panel, accent, text);
    }
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    timing_chrome_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (ui->screen != PSP_UI_SCREEN_FIND)
        draw_cursor(ui, pixels, width, height, stride, accent);
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    timing_cursor_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (ui->screen == PSP_UI_SCREEN_MENU) {
        draw_menu(ui, pixels, width, height, stride, panel, accent, text,
                  muted);
    }
    if (ui->screen == PSP_UI_SCREEN_UPDATE) {
        draw_update(
            ui, pixels, width, height, stride, panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_UPDATE_VERSIONS) {
        draw_update_versions(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS) {
        draw_experimental_options(
            ui, pixels, width, height, stride, panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_THEME_OPTIONS) {
        draw_theme_options(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_GLYPH_OPTIONS) {
        draw_glyph_options(
            ui, pixels, width, height, stride, panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_VIDEO_LANGUAGE_OPTIONS) {
        draw_video_language_options(
            ui, pixels, width, height, stride, panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_DATA_OPTIONS) {
        draw_data_options(
            ui, pixels, width, height, stride, panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_OPTIONS) {
        draw_options(ui, pixels, width, height, stride, panel, accent, text,
                     muted);
    } else if (ui->screen == PSP_UI_SCREEN_OPTION_ITEMS) {
        draw_option_items(ui, pixels, width, height, stride, panel, accent,
                          text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_PAGE_TOOLS) {
        draw_page_tools(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_SITE_CONTROLS) {
        draw_site_controls(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_PAGE_INFORMATION) {
        draw_page_information(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_OFFLINE_APP_PREVIEW) {
        draw_offline_app_preview(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_FAILURE_RECOVERY) {
        draw_failure_recovery(
            ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_HELP) {
        draw_help(ui, pixels, width, height, stride, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_HELP_DETAIL) {
        draw_help_detail(
            ui, pixels, width, height, stride, accent, text, muted);
    }
    if (ui->screen == PSP_UI_SCREEN_TABS) {
        draw_tabs(ui, pixels, width, height, stride, panel, accent, text,
                  muted);
    } else if (ui->screen == PSP_UI_SCREEN_TEXT_ENTRY) {
        draw_text_entry(
            ui, pixels, width, height, stride,
            panel, accent, text, muted);
    } else if (ui->screen == PSP_UI_SCREEN_FIND) {
        draw_find(ui, pixels, width, height, stride,
                  panel, accent, text, muted);
    }
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    timing_screen_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (ui->screen != PSP_UI_SCREEN_TEXT_ENTRY)
        draw_toast(ui, pixels, width, height, stride, panel, accent, text);
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    timing_toast_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (ui->screen == PSP_UI_SCREEN_PAGE)
        draw_loading(ui, pixels, width, height, stride, accent);
#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
    uint64_t timing_finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint32_t sequence = psp_ui_validation_timing.sequence + 1u;
    if (sequence == 0) sequence = 1u;
    psp_ui_validation_timing = (PspUiCompositeTiming) {
        .focus_scroll_us = timing_focus_us - timing_started_us,
        .chrome_us = timing_chrome_us - timing_focus_us,
        .cursor_us = timing_cursor_us - timing_chrome_us,
        .screen_us = timing_screen_us - timing_cursor_us,
        .toast_us = timing_toast_us - timing_screen_us,
        .loading_us = timing_finished_us - timing_toast_us,
        .sequence = sequence
    };
#endif
}

#if defined(TILEFINCH_PSP_UI_TIMING) && defined(__PSP__)
bool psp_ui_validation_last_timing(PspUiCompositeTiming *timing)
{
    if (timing == NULL || psp_ui_validation_timing.sequence == 0)
        return false;
    *timing = psp_ui_validation_timing;
    return true;
}
#endif

void psp_ui_composite(const PspUiState *ui, uint16_t *pixels,
                      int width, int height, int stride)
{
    if (ui == NULL || pixels == NULL || width <= 0 || height <= 0
        || stride < width) return;
    PspUiCompositeContext context = {
        .ui = ui,
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = stride,
        .palette = ui_chrome_palette((BrowserChromeTheme) ui->chrome_theme)
    };
    /* A native surface owns the panel, so page chrome must never run below
       it. The dispatcher is intentionally small; the two child symbols are
       what the PSP hot-path ratchets measure. */
    if (psp_ui_screen_is_native_surface(ui->screen))
        psp_ui_composite_native(&context);
    else
        psp_ui_composite_browser(&context);
}

/*
 * The mark, drawn from primitives at any size so the entrance can glide it
 * from the centre into the status line. It is deliberately not an asset:
 * the first visible frame must never depend on a file the Memory Stick has
 * not handed over yet.
 *
 * Every offset below is in the mark's own 48-unit box, scaled on the way
 * out. The layout is the splash's finch, unchanged.
 */
static void draw_boot_mark(uint16_t *pixels, int width, int height,
                           int stride, int x, int y, int size,
                           uint16_t accent)
{
#define MARK(value) ((value) * size / TILEFINCH_BOOT_MARK_UNITS)
    if (size < 8) return;
    uint16_t body = PSP_THEME_ON_ACCENT;
    uint16_t eye = PSP_THEME_TEXT;
#define UI_DRAW_MARK_ROUND(rx, ry, rw, rh, radius, color)          \
    fill_round_rect(                                                \
        pixels, width, height, stride,                              \
        (UiRect) {x + MARK(rx), y + MARK(ry), MARK(rw), MARK(rh)}, \
        MARK(radius), color, 4);
    TILEFINCH_BOOT_MARK_ROUND_RECTS(UI_DRAW_MARK_ROUND)
#undef UI_DRAW_MARK_ROUND
#define UI_DRAW_MARK_TRIANGLE(ax, ay, bx, by, cx, cy, color) \
    fill_triangle(                                           \
        pixels, width, height, stride,                       \
        x + MARK(ax), y + MARK(ay), x + MARK(bx), y + MARK(by), \
        x + MARK(cx), y + MARK(cy), color);
    TILEFINCH_BOOT_MARK_TRIANGLES(UI_DRAW_MARK_TRIANGLE)
#undef UI_DRAW_MARK_TRIANGLE
    if (size >= 24) {
#define UI_DRAW_MARK_RECT(rx, ry, rw, rh, color)                    \
        fill_rect(                                                   \
            pixels, width, height, stride,                          \
            (UiRect) {x + MARK(rx), y + MARK(ry), MARK(rw), MARK(rh)}, \
            color, 4);
        TILEFINCH_BOOT_MARK_RECTS(UI_DRAW_MARK_RECT)
#undef UI_DRAW_MARK_RECT
    }
#undef MARK
}

/* Where the mark rests on frame one, and where it lands: the status line's
   own text rail, which HOME's heading then takes over. */
#define UI_ENTRANCE_MARK_SIZE TILEFINCH_BOOT_MARK_UNITS
#define UI_ENTRANCE_MARK_TOP TILEFINCH_BOOT_MARK_TOP
#define UI_ENTRANCE_MARK_LANDED_SIZE 12
#define UI_ENTRANCE_MARK_LANDED_X PSP_THEME_SPACE_L
#define UI_ENTRANCE_MARK_LANDED_Y 3

void psp_ui_boot_entrance_composite(
    const PspUiBootEntranceView *view, const PspUiState *home,
    uint16_t *pixels, int width, int height, int stride)
{
    if (view == NULL || pixels == NULL || width <= 0 || height <= 0
        || stride < width) return;
    UiSurfaceAccent accent = ui_surface_accent(
        home == NULL ? BROWSER_CHROME_THEME_FINCH
                     : (BrowserChromeTheme) home->chrome_theme);
    unsigned frame = view->frame;
    if (frame > PSP_UI_BOOT_ENTRANCE_FRAMES)
        frame = PSP_UI_BOOT_ENTRANCE_FRAMES;
    /*
     * The slow branch holds everything: a boot that has gone somewhere
     * unusual should not also be animating. The mark stays where frame one
     * put it and one muted line says where boot is.
     */
    bool held = view->branch_status != NULL
        && view->branch_status[0] != '\0';
    if (held) frame = 0u;

    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, height}, PSP_THEME_GROUND, 4);

    if (frame >= PSP_UI_BOOT_ENTRANCE_GLIDE_FRAME && home != NULL) {
        unsigned step = frame - PSP_UI_BOOT_ENTRANCE_GLIDE_FRAME;
        unsigned eased = ui_entrance_ease[step];
        int rise = 40 * (int) (64u - eased) / 64;
        UiHomeEntrance entrance = {
            .tile_rise = rise,
            /* The furniture is the last thing to arrive, and it arrives
               through the text ramp rather than through an alpha blend. */
            .furniture = eased >= 62u ? 2u : (eased >= 47u ? 1u : 0u),
            .entering = eased < 64u
        };
        draw_home_entering(
            home, pixels, width, height, stride, accent, entrance);
        /* The mark hands the status line over to HOME's own heading, so it
           stops being drawn once it has arrived. */
        if (eased < 64u) {
            int mark_x = (width - UI_ENTRANCE_MARK_SIZE) / 2;
            /*
             * The mark climbs to the status line's row at twice the rate it
             * crosses to it, so it leaves the grid the tiles are rising
             * into almost at once and spends the rest of the glide on the
             * rail it is going to land on. A straight line between the two
             * points flew through the tiles for the whole transit.
             */
            unsigned lift = eased * 3u / 2u;
            if (lift > 64u) lift = 64u;
            int size = UI_ENTRANCE_MARK_SIZE
                - (UI_ENTRANCE_MARK_SIZE - UI_ENTRANCE_MARK_LANDED_SIZE)
                      * (int) lift / 64;
            draw_boot_mark(
                pixels, width, height, stride,
                mark_x
                    + (UI_ENTRANCE_MARK_LANDED_X - mark_x)
                          * (int) eased / 64,
                UI_ENTRANCE_MARK_TOP
                    + (UI_ENTRANCE_MARK_LANDED_Y - UI_ENTRANCE_MARK_TOP)
                          * (int) lift / 64,
                size, accent.accent);
        }
        return;
    }

    /*
     * Steps one and two. The band fades up behind the mark in whole
     * pre-rendered brightness steps -- each rendered once, then blitted --
     * and is skipped entirely when the wave option is off.
     */
    draw_boot_mark(
        pixels, width, height, stride,
        (width - UI_ENTRANCE_MARK_SIZE) / 2, UI_ENTRANCE_MARK_TOP,
        UI_ENTRANCE_MARK_SIZE, accent.accent);
    if (held) {
        int text_width = chrome_text_width_bytes(
            view->branch_status, strlen(view->branch_status), 1, false);
        draw_text(
            pixels, width, height, stride, (width - text_width) / 2,
            UI_ENTRANCE_MARK_TOP + UI_ENTRANCE_MARK_SIZE + PSP_THEME_SPACE_XL,
            view->branch_status, 44, PSP_THEME_TEXT_MUTED, 1);
    }
    /* Safe-start input belongs to the launcher before this process exists.
       Advertising it on the browser's later splash makes a correctly timed
       press look broken, so this surface intentionally carries no controls. */
}

static void startup_fill(
    uint16_t *pixels, int width, int height, int stride, uint16_t color)
{
    fill_rect(
        pixels, width, height, stride, (UiRect) {0, 0, width, height},
        color, 4);
}

static void draw_startup_splash(
    const char *status, int progress_per_mille, uint16_t *pixels,
    int width, int height, int stride)
{
    uint16_t background = PSP_THEME_GROUND;
    uint16_t panel = rgb565(13, 32, 43);
    uint16_t accent = PSP_THEME_ACCENT_EMBER;
    uint16_t text = rgb565(236, 244, 244);
    uint16_t muted = rgb565(128, 151, 161);
    startup_fill(pixels, width, height, stride, background);

    int mark_x = width / 2 - TILEFINCH_BOOT_MARK_UNITS / 2;
    /* Use the same canonical mark as the launcher and ordinary entrance.
       This diagnostic/failure path must not revive the older navy variant. */
    draw_boot_mark(
        pixels, width, height, stride, mark_x, 54,
        TILEFINCH_BOOT_MARK_UNITS, accent);

    int title_width = 9 * 12 - 2;
    draw_text(
        pixels, width, height, stride, (width - title_width) / 2, 119,
        "TILEFINCH", 9, text, 2);

    /* This view normally lasts only a fraction of boot. Keep one quiet,
       truthful progress cue so a slow Memory Stick never looks like a hang. */
    UiRect track = {width / 2 - 90, 158, 180, 3};
    fill_round_rect(
        pixels, width, height, stride, track, 1, panel, 4);
    int progress = progress_per_mille;
    if (progress < 0) progress = 80;
    if (progress > 1000) progress = 1000;
    int filled = track.width * progress / 1000;
    if (filled < 20) filled = 20;
    fill_round_rect(
        pixels, width, height, stride,
        (UiRect) {track.x, track.y, filled, track.height}, 1, accent, 4);

    const char *message =
        status == NULL || status[0] == '\0' ? "STARTING BROWSER" : status;
    size_t message_length = strlen(message);
    if (message_length > 44u) message_length = 44u;
    int message_width = (int) message_length * 6 - 1;
    draw_text(
        pixels, width, height, stride,
        (width - message_width) / 2, 174, message, message_length,
        text, 1);
    draw_text(
        pixels, width, height, stride, width / 2 - 63, height - 18,
        "HOLD L WHILE STARTING", 21, muted, 1);
}

static void draw_startup_homepage(
    const char *status, int progress_per_mille, uint16_t *pixels,
    int width, int height, int stride)
{
    uint16_t page = rgb565(247, 247, 248);
    uint16_t card = rgb565(255, 255, 255);
    uint16_t border = rgb565(215, 219, 223);
    uint16_t heading = rgb565(16, 20, 24);
    uint16_t muted = rgb565(92, 102, 112);
    uint16_t accent = PSP_THEME_ACCENT_EMBER;
    startup_fill(pixels, width, height, stride, page);

    /* Match the profile-backed homepage without reading page or font assets. */
    draw_text(
        pixels, width, height, stride, 16, 53, "TILEFINCH", 9,
        heading, 2);
    UiRect search = {16, 75, width - 32, 34};
    fill_round_rect(
        pixels, width, height, stride, search, 9, card, 4);
    outline_rect(
        pixels, width, height, stride, search, accent, 2);
    draw_text(
        pixels, width, height, stride, search.x + 13, search.y + 12,
        "SEARCH OR ENTER AN ADDRESS", 30, muted, 1);

    const UiRect cards[2] = {
        {16, 119, (width - 41) / 2, 64},
        {25 + (width - 41) / 2, 119, (width - 41) / 2, 64}
    };
    const char *names[2] = {"YOUTUBE", "WIKIPEDIA"};
    const char *descriptions[2] = {
        "BROWSE AND PLAY VIDEO",
        "ARTICLES AND SEARCH"
    };
    for (size_t at = 0; at < 2; at++) {
        fill_round_rect(
            pixels, width, height, stride, cards[at], 8, card, 4);
        outline_rect(
            pixels, width, height, stride, cards[at], border, 1);
        draw_text(
            pixels, width, height, stride,
            cards[at].x + 14, cards[at].y + 11, names[at],
            strlen(names[at]),
            heading, 2);
        draw_text(
            pixels, width, height, stride,
            cards[at].x + 14, cards[at].y + 39,
            descriptions[at], strlen(descriptions[at]), muted, 1);
    }

    const char *message =
        status == NULL || status[0] == '\0' ? "PREPARING HOME" : status;
    size_t message_length = strlen(message);
    if (message_length > 46u) message_length = 46u;
    draw_text(
        pixels, width, height, stride, 16, 232, message, message_length,
        muted, 1);

    PspUiState shell;
    psp_ui_init(&shell);
    shell.loading_phase =
        progress_per_mille < 0 ? 0u : (unsigned) progress_per_mille;
    psp_ui_set_page(&shell, "TILEFINCH", "tilefinch://home", false);
    psp_ui_set_loading(&shell, true, progress_per_mille);
    shell.toast_frames = 0;
    psp_ui_composite(
        &shell, pixels, width, height, stride);

    /* Mirror the first real target's focus marker so the final homepage
       handoff is spatially stable as well as stylistically stable. */
    outline_rect(
        pixels, width, height, stride,
        (UiRect) {12, 71, width - 24, 42}, accent, 2);
}

void psp_ui_startup_composite(
    PspUiStartupView view, const char *status, int progress_per_mille,
    uint16_t *pixels, int width, int height, int stride)
{
    /* Startup is deliberately file-independent. A custom palette is selected
       only after the profile has explicitly requested and loaded it. */
    psp_ui_theme_select(BROWSER_CHROME_THEME_FINCH);
    if (pixels == NULL || width <= 0 || height <= 0 || stride < width)
        return;
    if (view == PSP_UI_STARTUP_HOMEPAGE) {
        draw_startup_homepage(
            status, progress_per_mille, pixels, width, height, stride);
    } else {
        draw_startup_splash(
            status, progress_per_mille, pixels, width, height, stride);
    }
}

size_t psp_ui_state_bytes(void)
{
    return sizeof(PspUiState);
}

static void media_format_time(uint64_t microseconds,
                              char *text, size_t capacity)
{
    if (microseconds <= UINT32_MAX) {
        uint32_t total_seconds = (uint32_t) microseconds / UINT32_C(1000000);
        uint32_t minutes = total_seconds / 60u;
        uint32_t seconds = total_seconds % 60u;
        if (minutes > 999u) minutes = 999u;
        snprintf(text, capacity, "%lu:%02lu",
                 (unsigned long) minutes, (unsigned long) seconds);
    } else {
        uint64_t total_seconds = microseconds / UINT64_C(1000000);
        uint64_t minutes = total_seconds / 60u;
        uint64_t seconds = total_seconds % 60u;
        if (minutes > 999u) minutes = 999u;
        snprintf(text, capacity, "%llu:%02llu",
                 (unsigned long long) minutes,
                 (unsigned long long) seconds);
    }
}

#define UI_MEDIA_TIMELINE_PIXELS 432u
#define UI_MEDIA_BUFFER_VISUAL_INTERVAL_MS 500u

static uint16_t media_timeline_pixel(uint64_t time_us, uint64_t duration_us)
{
    if (duration_us == 0u || time_us == 0u) return 0u;
    uint64_t bounded = time_us < duration_us ? time_us : duration_us;
    return (uint16_t) psp_ui_ratio_extent_u64(
        bounded, duration_us, UI_MEDIA_TIMELINE_PIXELS);
}

static void media_timeline_visual_set(
    PspUiMediaState *media, uint64_t time_us, bool force)
{
    if (media == NULL) return;
    uint16_t next = media_timeline_pixel(time_us, media->duration_us);
    uint16_t previous = media->timeline_visual_pixel;
    unsigned distance = next >= previous
        ? (unsigned) (next - previous) : (unsigned) (previous - next);
    /* A one-pixel dead band prevents the AA knob fringe from alternating
       between adjacent columns as timestamp rounding jitters at a boundary.
       Larger moves, endpoints and explicit lifecycle resets remain prompt. */
    if (force || distance > 1u || next == 0u
        || next == UI_MEDIA_TIMELINE_PIXELS) {
        media->timeline_visual_pixel = next;
    }
}

static void media_buffer_visual_refresh(PspUiMediaState *media)
{
    if (media == NULL) return;
    media->buffered_visual_pixel = media_timeline_pixel(
        media->buffered_until_us, media->duration_us);
}

void psp_ui_media_init(PspUiMediaState *media)
{
    if (media == NULL) return;
    memset(media, 0, sizeof(*media));
    media->controls_visible = true;
    media->controls_remaining_ms = UI_MEDIA_CONTROLS_MS;
}

void psp_ui_media_bind_presentation(
    PspUiMediaState *media, PspUiMediaPresentation *presentation)
{
    if (media == NULL) return;
    media->presentation = presentation;
    if (presentation == NULL) return;
    memset(presentation, 0, sizeof(*presentation));
    presentation->selected_audio_track = -1;
    presentation->selected_subtitle_track = -1;
}

void psp_ui_media_set_title_font(PspUiMediaState *media,
                                 const FontFace *font)
{
    if (media != NULL && media->presentation != NULL)
        media->presentation->title_font = font;
}

void psp_ui_media_set_tracks(
    PspUiMediaState *media,
    const PspUiMediaTrack *audio_tracks, size_t audio_count,
    int selected_audio,
    const PspUiMediaTrack *subtitle_tracks, size_t subtitle_count,
    int selected_subtitle)
{
    if (media == NULL || media->presentation == NULL) return;
    PspUiMediaPresentation *presentation = media->presentation;
    if (audio_count > PSP_UI_MEDIA_TRACK_LIMIT)
        audio_count = PSP_UI_MEDIA_TRACK_LIMIT;
    if (subtitle_count > PSP_UI_MEDIA_TRACK_LIMIT)
        subtitle_count = PSP_UI_MEDIA_TRACK_LIMIT;
    memset(presentation->audio_tracks, 0,
           sizeof(presentation->audio_tracks));
    memset(presentation->subtitle_tracks, 0,
           sizeof(presentation->subtitle_tracks));
    if (audio_tracks != NULL && audio_count != 0)
        memcpy(presentation->audio_tracks, audio_tracks,
               audio_count * sizeof(*audio_tracks));
    if (subtitle_tracks != NULL && subtitle_count != 0)
        memcpy(presentation->subtitle_tracks, subtitle_tracks,
               subtitle_count * sizeof(*subtitle_tracks));
    presentation->audio_track_count = (uint8_t) audio_count;
    presentation->subtitle_track_count = (uint8_t) subtitle_count;
    presentation->selected_audio_track = selected_audio >= 0
            && (size_t) selected_audio < audio_count
        ? (int8_t) selected_audio : -1;
    presentation->selected_subtitle_track = selected_subtitle >= 0
            && (size_t) selected_subtitle < subtitle_count
        ? (int8_t) selected_subtitle : -1;
}

void psp_ui_media_set_subtitle(PspUiMediaState *media, const char *text)
{
    if (media == NULL || media->presentation == NULL) return;
    copy_string(media->presentation->subtitle_text,
                sizeof(media->presentation->subtitle_text),
                text == NULL ? "" : text);
}

void psp_ui_media_set_subtitle_style(
    PspUiMediaState *media, BrowserSubtitleSize size,
    BrowserSubtitleBackground background)
{
    if (media == NULL || media->presentation == NULL) return;
    media->presentation->subtitle_size = (uint8_t) (
        size >= BROWSER_SUBTITLE_SIZE_STANDARD
            && size < BROWSER_SUBTITLE_SIZE_COUNT
            ? size : BROWSER_SUBTITLE_SIZE_STANDARD);
    media->presentation->subtitle_background = (uint8_t) (
        background >= BROWSER_SUBTITLE_BACKGROUND_BOX
            && background < BROWSER_SUBTITLE_BACKGROUND_COUNT
            ? background : BROWSER_SUBTITLE_BACKGROUND_BOX);
}

void psp_ui_media_set(PspUiMediaState *media, bool visible, bool playing,
                      bool ended, uint64_t current_time_us,
                      uint64_t duration_us, const char *title)
{
    if (media == NULL) return;
    bool became_visible = visible && !media->visible;
    bool duration_changed = duration_us != media->duration_us;
    media->visible = visible;
    media->resolving = false;
    media->seek_in_progress = false;
    media->failed = false;
    media->audio_only_recovery_available = false;
    media->lower_quality_recovery_available = false;
    media->status[0] = '\0';
    media->playing = playing;
    media->ended = ended;
    media->buffering = false;
    media->live = false;
    media->controls_enabled = visible;
    media->play_pause_enabled = visible;
    media->seek_enabled = visible && duration_us != 0;
    media->resolving_progress_per_mille = 0;
    media->current_time_us = current_time_us;
    media->duration_us = duration_us;
    if (duration_changed) {
        media->buffered_visual_pixel = 0u;
        media->buffered_visual_elapsed_ms = 0u;
    }
    if (!media->seek_preview_active)
        media_timeline_visual_set(
            media, current_time_us, became_visible || duration_changed);
    copy_string(media->title, sizeof(media->title),
                title == NULL || title[0] == '\0' ? "VIDEO" : title);
    if (became_visible || !playing || ended) {
        media->controls_visible = true;
        media->controls_remaining_ms = UI_MEDIA_CONTROLS_MS;
    }
}

void psp_ui_media_set_resolving(PspUiMediaState *media, const char *title)
{
    if (media == NULL) return;
    if (media->presentation != NULL)
        media->presentation->track_menu_open = false;
    media->visible = true;
    media->controls_visible = true;
    media->resolving = true;
    media->seek_in_progress = false;
    media->failed = false;
    media->audio_only_recovery_available = false;
    media->lower_quality_recovery_available = false;
    media->playing = false;
    media->ended = false;
    media->buffering = false;
    media->live = false;
    media->seek_preview_active = false;
    media->controls_enabled = false;
    media->play_pause_enabled = false;
    media->seek_enabled = false;
    media->analog_seek_direction = 0;
    media->buffered_until_us = 0;
    media->timeline_visual_pixel = 0u;
    media->buffered_visual_pixel = 0u;
    media->buffered_visual_elapsed_ms = 0u;
    media->current_time_us = 0;
    media->duration_us = 0;
    media->controls_remaining_ms = UI_MEDIA_CONTROLS_MS;
    copy_string(media->title, sizeof(media->title),
                title == NULL || title[0] == '\0' ? "VIDEO" : title);
    copy_string(media->status, sizeof(media->status), "Loading...");
    media->resolving_progress_per_mille = 0;
}

void psp_ui_media_set_resolving_progress(
    PspUiMediaState *media, const char *status,
    unsigned progress_per_mille)
{
    if (media == NULL || !media->resolving) return;
    copy_string(media->status, sizeof(media->status),
                status == NULL || status[0] == '\0'
                    ? "STARTING VIDEO" : status);
    media->resolving_progress_per_mille =
        progress_per_mille > 1000u ? 1000u : progress_per_mille;
}

/*
 * The failed player surface carries two lines. The headline names the failed
 * stage the way it always has; the muted second line names the underlying
 * cause, which shipping builds otherwise lose entirely because their printf
 * telemetry compiles to nothing. Both share the existing bounded `status`
 * field, separated by a newline, so the media state keeps its size ratchet.
 */
void psp_ui_media_set_error_reason(PspUiMediaState *media,
                                   const char *message, const char *reason)
{
    psp_ui_media_set_error(media, message);
    if (media == NULL || reason == NULL || reason[0] == '\0'
        || strchr(media->status, '\n') != NULL) return;
    size_t used = strlen(media->status);
    if (used + 2u >= sizeof(media->status)) return;
    media->status[used] = '\n';
    copy_string(media->status + used + 1u,
                sizeof(media->status) - used - 1u, reason);
}

/*
 * A backend failure string is the only cause a shipping build can still show,
 * and the interesting part -- the firmware status code -- is at its end. One
 * headline row fits about 32 glyphs, so fold the message onto the panel's
 * second row at a word boundary rather than clipping the code away. Replacing
 * a space keeps the bounded field's length unchanged.
 */
#define UI_MEDIA_ERROR_ROW_CHARACTERS 32u

static void media_error_wrap(char *status, size_t capacity)
{
    size_t length = strlen(status);
    if (length <= UI_MEDIA_ERROR_ROW_CHARACTERS
        || capacity <= UI_MEDIA_ERROR_ROW_CHARACTERS) return;
    size_t limit = length < UI_MEDIA_ERROR_ROW_CHARACTERS + 2u
        ? length : UI_MEDIA_ERROR_ROW_CHARACTERS + 2u;
    for (size_t at = limit; at > 8u; at--) {
        if (status[at - 1u] != ' ') continue;
        status[at - 1u] = '\n';
        return;
    }
}

void psp_ui_media_set_error(PspUiMediaState *media, const char *message)
{
    if (media == NULL) return;
    if (media->presentation != NULL)
        media->presentation->track_menu_open = false;
    media->visible = true;
    media->controls_visible = true;
    media->resolving = false;
    media->seek_in_progress = false;
    media->failed = true;
    media->retry_unavailable = false;
    media->audio_only_recovery_available = false;
    media->lower_quality_recovery_available = false;
    media->playing = false;
    media->ended = false;
    media->buffering = false;
    media->live = false;
    media->seek_preview_active = false;
    media->controls_enabled = true;
    media->play_pause_enabled = false;
    media->seek_enabled = false;
    media->analog_seek_direction = 0;
    media->seek_preview_time_us = 0;
    media->buffered_until_us = 0;
    media->timeline_visual_pixel = 0u;
    media->buffered_visual_pixel = 0u;
    media->buffered_visual_elapsed_ms = 0u;
    media->controls_remaining_ms = UI_MEDIA_CONTROLS_MS;
    media->resolving_progress_per_mille = 0;
    copy_string(media->status, sizeof(media->status),
                message == NULL || message[0] == '\0'
                    ? "VIDEO UNAVAILABLE" : message);
    media_error_wrap(media->status, sizeof(media->status));
}

void psp_ui_media_set_buffering(PspUiMediaState *media, bool buffering,
                                uint64_t buffered_until_us)
{
    if (media == NULL) return;
    bool moved_backward = buffered_until_us < media->buffered_until_us;
    media->buffering = buffering;
    media->buffered_until_us = buffered_until_us;
    if (moved_backward
        || (media->buffered_visual_pixel == 0u
            && buffered_until_us != 0u)) {
        media_buffer_visual_refresh(media);
        media->buffered_visual_elapsed_ms = 0u;
    }
    /* A network pause is not a user request to reopen the chrome. Forcing
       controls visible made the opaque title and scrubber bars flash in and
       out around short refills on the physical panel. The compositor can draw
       the bounded centre pill by itself while preserving an already-visible
       controls state. */
}

void psp_ui_media_apply_projection(
    PspUiMediaState *media, const PspMediaUiProjection *projection)
{
    if (media == NULL || projection == NULL) return;
    bool continuing_seek = media->seek_in_progress
        && (projection->mode == PSP_MEDIA_UI_OPENING
            || projection->mode == PSP_MEDIA_UI_PRIMING
            || projection->mode == PSP_MEDIA_UI_SEEKING
            || projection->mode == PSP_MEDIA_UI_RECOVERING);
    bool local_analog_preview = media->analog_seek_direction != 0
        && media->seek_preview_active && projection->seek_enabled;
    media->visible = projection->visible;
    media->playing = projection->playing;
    media->failed = projection->mode == PSP_MEDIA_UI_FAILED;
    media->retry_unavailable = media->failed
        && !projection->retry_available;
    media->resolving = projection->mode == PSP_MEDIA_UI_OPENING
        || projection->mode == PSP_MEDIA_UI_PRIMING
        || projection->mode == PSP_MEDIA_UI_SEEKING
        || projection->mode == PSP_MEDIA_UI_RECOVERING
        || projection->mode == PSP_MEDIA_UI_STOPPING;
    media->seek_in_progress =
        projection->mode == PSP_MEDIA_UI_SEEKING || continuing_seek;
    media->buffering = projection->mode == PSP_MEDIA_UI_BUFFERING;
    if (media->seek_in_progress) {
        copy_string(media->status, sizeof(media->status),
                    projection->mode == PSP_MEDIA_UI_SEEKING
                        ? "Seeking..." : "Buffering after seek");
    }
    /* The controller owns decoder-preview transactions, but the nub's target
       is deliberately UI-local until release coalesces it into one request.
       Preserve that target across unrelated controller projections (notably
       source-starved/stable edges); otherwise the timeline alternates between
       the current and preview positions while the nub remains held. A state
       which disables seeking still clears it immediately. */
    media->seek_preview_active = projection->preview_active
        || local_analog_preview;
    media->ended = projection->ended;
    media->controls_enabled = projection->controls_enabled;
    media->play_pause_enabled = projection->play_pause_enabled;
    media->seek_enabled = projection->seek_enabled;
}

void psp_ui_media_set_seek_preview(PspUiMediaState *media,
                                   uint64_t target_time_us)
{
    if (media == NULL || !media->visible || media->duration_us == 0) return;
    media->seek_preview_active = true;
    media->seek_preview_time_us = target_time_us < media->duration_us
        ? target_time_us : media->duration_us;
    media_timeline_visual_set(media, media->seek_preview_time_us, false);
    psp_ui_media_show_controls(media);
}

void psp_ui_media_commit_seek(PspUiMediaState *media,
                              uint64_t target_time_us)
{
    if (media == NULL || !media->visible) return;
    uint64_t target = media->duration_us != 0u
            && target_time_us > media->duration_us
        ? media->duration_us : target_time_us;
    media->seek_preview_active = false;
    media->seek_preview_time_us = target;
    media->analog_seek_direction = 0;
    media->current_time_us = target;
    media->seek_in_progress = true;
    media->resolving = true;
    media_timeline_visual_set(media, target, true);
    copy_string(media->status, sizeof(media->status), "Seeking...");
    psp_ui_media_show_controls(media);
}

void psp_ui_media_cancel_seek_preview(PspUiMediaState *media)
{
    if (media == NULL) return;
    media->seek_preview_active = false;
    media->seek_preview_time_us = 0;
    media->analog_seek_direction = 0;
    media_timeline_visual_set(media, media->current_time_us, true);
}

void psp_ui_media_show_controls(PspUiMediaState *media)
{
    if (media == NULL || !media->visible) return;
    media->controls_visible = true;
    media->controls_remaining_ms = UI_MEDIA_CONTROLS_MS;
}

void psp_ui_media_tick(PspUiMediaState *media, unsigned elapsed_ms)
{
    if (media == NULL) return;
    unsigned visual_elapsed = media->buffered_visual_elapsed_ms;
    visual_elapsed = elapsed_ms > UINT16_MAX - visual_elapsed
        ? UINT16_MAX : visual_elapsed + elapsed_ms;
    if (visual_elapsed >= UI_MEDIA_BUFFER_VISUAL_INTERVAL_MS) {
        media_buffer_visual_refresh(media);
        visual_elapsed %= UI_MEDIA_BUFFER_VISUAL_INTERVAL_MS;
    }
    media->buffered_visual_elapsed_ms = (uint16_t) visual_elapsed;
    if (!media->visible || !media->controls_visible
        || !media->playing || media->ended || media->resolving
        || media->seek_preview_active
        || (media->presentation != NULL
            && media->presentation->track_menu_open)) return;
    if (elapsed_ms >= media->controls_remaining_ms) {
        media->controls_remaining_ms = 0;
        media->controls_visible = false;
    } else {
        media->controls_remaining_ms -= elapsed_ms;
    }
}

static uint64_t media_seek_clamped(const PspUiMediaState *media,
                                   uint64_t current_time_us,
                                   int64_t delta_us)
{
    uint64_t target;
    if (delta_us < 0) {
        uint64_t magnitude = (uint64_t) (-(delta_us + 1)) + 1u;
        target = magnitude >= current_time_us
            ? 0 : current_time_us - magnitude;
    } else {
        uint64_t delta = (uint64_t) delta_us;
        target = delta > UINT64_MAX - current_time_us
            ? UINT64_MAX : current_time_us + delta;
    }
    if (media->duration_us != 0 && target > media->duration_us) {
        target = media->duration_us;
    }
    return target;
}

static uint64_t media_seek_position_time(
    unsigned position, unsigned extent, uint64_t duration_us)
{
    if (extent == 0 || duration_us == 0) return 0;
    if (position >= extent) return duration_us;
    uint64_t whole = duration_us / extent;
    uint64_t remainder = duration_us % extent;
    return whole * position + remainder * position / extent;
}

PspUiMediaIntent psp_ui_media_update(PspUiMediaState *media,
                                     const PspUiInput *input)
{
    PspUiMediaIntent intent = {0};
    if (media == NULL || input == NULL || !media->visible) return intent;
    PspUiMediaPresentation *presentation = media->presentation;
    uint32_t pressed = input->pressed;
    if (presentation != NULL && presentation->track_menu_open) {
        unsigned count = presentation->track_menu_tab == 0
            ? presentation->audio_track_count
            : (unsigned) presentation->subtitle_track_count + 1u;
        if (pressed & PSP_UI_BUTTON_CANCEL) {
            presentation->track_menu_open = false;
            /* Circle dismisses the modal back to unobscured playback. Keeping
               controls alive here rebuilt two full-width chrome bands on the
               same frame and caused a visible one-frame hitch on hardware. */
            media->controls_visible = false;
            media->controls_remaining_ms = 0u;
        } else if (pressed & (PSP_UI_BUTTON_PAGE_UP
                              | PSP_UI_BUTTON_PAGE_DOWN)) {
            presentation->track_menu_tab ^= 1u;
            count = presentation->track_menu_tab == 0
                ? presentation->audio_track_count
                : (unsigned) presentation->subtitle_track_count + 1u;
            presentation->track_menu_selection =
                presentation->track_menu_tab == 0
                ? (presentation->selected_audio_track < 0
                    ? 0u : (uint8_t) presentation->selected_audio_track)
                : (presentation->selected_subtitle_track < 0
                    ? 0u
                    : (uint8_t) presentation->selected_subtitle_track + 1u);
            if (count != 0 && presentation->track_menu_selection >= count)
                presentation->track_menu_selection =
                    (uint8_t) (count - 1u);
        } else if (count != 0 && (pressed & PSP_UI_BUTTON_UP)) {
            presentation->track_menu_selection =
                presentation->track_menu_selection == 0
                ? (uint8_t) (count - 1u)
                : (uint8_t) (presentation->track_menu_selection - 1u);
        } else if (count != 0 && (pressed & PSP_UI_BUTTON_DOWN)) {
            presentation->track_menu_selection = (uint8_t)
                ((presentation->track_menu_selection + 1u) % count);
        } else if (count != 0 && (pressed & PSP_UI_BUTTON_CONFIRM)) {
            if (presentation->track_menu_tab == 0) {
                intent.action = PSP_UI_MEDIA_ACTION_SELECT_AUDIO_TRACK;
                intent.track_index = presentation->track_menu_selection;
            } else {
                intent.action = PSP_UI_MEDIA_ACTION_SELECT_SUBTITLE_TRACK;
                intent.track_index = presentation->track_menu_selection == 0
                    ? UINT8_MAX
                    : (uint8_t) (presentation->track_menu_selection - 1u);
            }
            presentation->track_menu_open = false;
        }
        intent.visual_changed = pressed != 0;
        return intent;
    }
    if (pressed == 0 && !media->failed && media->seek_enabled
        && media->duration_us != 0) {
        int analog = (int) input->analog_x - 128;
        int magnitude = analog < 0 ? -analog : analog;
        if (magnitude > UI_ANALOG_DEAD_ZONE) {
            int direction = analog < 0 ? -1 : 1;
            unsigned elapsed_ms =
                input->elapsed_ms == 0 ? 16u : input->elapsed_ms;
            if (elapsed_ms > UI_ANALOG_MAX_ELAPSED_MS)
                elapsed_ms = UI_ANALOG_MAX_ELAPSED_MS;
            unsigned active =
                (unsigned) (magnitude - UI_ANALOG_DEAD_ZONE);
            unsigned velocity = 1u + active * 29u / 103u;
            int64_t delta_us =
                (int64_t) velocity * elapsed_ms * 1000;
            uint64_t base = media->seek_preview_active
                ? media->seek_preview_time_us : media->current_time_us;
            intent.seek_time_us = media_seek_clamped(
                media, base, direction < 0 ? -delta_us : delta_us);
            psp_ui_media_set_seek_preview(media, intent.seek_time_us);
            media->analog_seek_direction = (int8_t) direction;
            intent.visual_changed = true;
            return intent;
        }
        if (media->analog_seek_direction != 0) {
            media->analog_seek_direction = 0;
            if (media->seek_preview_active) {
                intent.action = PSP_UI_MEDIA_ACTION_PREVIEW_SEEK;
                intent.seek_time_us = media->seek_preview_time_us;
                intent.visual_changed = true;
            }
            return intent;
        }
    }
    if (pressed == 0) {
        media->analog_seek_direction = 0;
        return intent;
    }
    media->analog_seek_direction = 0;
    psp_ui_media_show_controls(media);
    intent.visual_changed = true;
    if (!media->failed && presentation != NULL
        && (pressed & PSP_UI_BUTTON_TOOLBAR)
        && (presentation->audio_track_count != 0
            || presentation->subtitle_track_count != 0)) {
        presentation->track_menu_open = true;
        presentation->track_menu_tab =
            presentation->audio_track_count == 0 ? 1u : 0u;
        presentation->track_menu_selection =
            presentation->track_menu_tab == 0
            ? (presentation->selected_audio_track < 0
                ? 0u : (uint8_t) presentation->selected_audio_track)
            : (presentation->selected_subtitle_track < 0
                ? 0u
                : (uint8_t) presentation->selected_subtitle_track + 1u);
    } else if (pressed & PSP_UI_BUTTON_CANCEL) {
        if (media->seek_preview_active) {
            psp_ui_media_cancel_seek_preview(media);
            intent.action = PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW;
        } else {
            intent.action = PSP_UI_MEDIA_ACTION_CLOSE;
        }
    } else if (media->failed) {
        if (!media->retry_unavailable
            && (pressed & PSP_UI_BUTTON_TOOLBAR) != 0
            && media->audio_only_recovery_available)
            intent.action = PSP_UI_MEDIA_ACTION_AUDIO_ONLY;
        else if (!media->retry_unavailable
                 && (pressed & PSP_UI_BUTTON_RELOAD) != 0
                 && media->lower_quality_recovery_available)
            intent.action = PSP_UI_MEDIA_ACTION_LOWER_QUALITY;
        else if ((pressed & PSP_UI_BUTTON_CONFIRM) != 0
            && !media->retry_unavailable)
            intent.action = PSP_UI_MEDIA_ACTION_RETRY;
        return intent;
    } else if (pressed & PSP_UI_BUTTON_CONFIRM) {
        if (media->seek_preview_active) {
            /* A preview decoder transaction may still be restoring the
               original source position. Direction input remains responsive
               and is coalesced by the session. Commit is also a one-entry
               request: the session applies it at the first safe boundary
               instead of dropping Cross until the picture returns. */
            intent.action = PSP_UI_MEDIA_ACTION_SEEK;
            intent.seek_time_us = media->seek_preview_time_us;
            /* The session atomically replaces this tentative target with a
               committed one. Clearing it here exposed the old playback clock
               during the pre-dispatch repaint, making the scrubber snap back
               for one or more frames before decoder preparation began. */
        } else if (media->play_pause_enabled) {
            intent.action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE;
        }
    } else if (media->seek_enabled
               && (pressed
                   & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_PAGE_UP)) != 0) {
        uint64_t base = media->seek_preview_active
            ? media->seek_preview_time_us : media->current_time_us;
        intent.action = PSP_UI_MEDIA_ACTION_PREVIEW_SEEK;
        intent.seek_time_us = media_seek_clamped(
            media, base, pressed & PSP_UI_BUTTON_PAGE_UP
                ? -UINT64_C(60000000) : -UINT64_C(10000000));
        psp_ui_media_set_seek_preview(media, intent.seek_time_us);
    } else if (media->seek_enabled
               && (pressed
                   & (PSP_UI_BUTTON_RIGHT | PSP_UI_BUTTON_PAGE_DOWN)) != 0) {
        uint64_t base = media->seek_preview_active
            ? media->seek_preview_time_us : media->current_time_us;
        intent.action = PSP_UI_MEDIA_ACTION_PREVIEW_SEEK;
        intent.seek_time_us = media_seek_clamped(
            media, base, pressed & PSP_UI_BUTTON_PAGE_DOWN
                ? INT64_C(60000000) : INT64_C(10000000));
        psp_ui_media_set_seek_preview(media, intent.seek_time_us);
    } else if (media->controls_enabled) {
        media->controls_visible = !media->controls_visible;
    }
    return intent;
}

bool psp_ui_media_intent_has_predispatch_visual(
    const PspUiMediaIntent *intent)
{
    if (intent == NULL || !intent->visual_changed) return false;
    /* Play/Pause has not changed presentation state yet: the session reducer
       does that when it consumes the intent. An ACTION_NONE update is the
       analog seek preview moving while the nub remains held; it performs no
       slow backend work and belongs in the ordinary end-of-frame present.
       Publishing either here would rotate scanout once now and once again at
       frame end. The old/new pair flashes on resume, while repeating it at
       nub cadence makes the whole control surface flicker. Actual actions
       keep the early present because seek, retry, and close can block while
       the already-mutated preview or controls should remain responsive. */
    return intent->action != PSP_UI_MEDIA_ACTION_NONE
        && intent->action != PSP_UI_MEDIA_ACTION_PLAY_PAUSE;
}

/*
 * The failed panel and its two chips have one definition so compositing and
 * hit testing cannot drift apart. A quarantined decoder replaces the Retry
 * chip with the restart note and drops Back onto the row below it.
 */
static UiRect media_failed_panel_rect(int width, int height)
{
    return (UiRect) {width / 2 - 162, height / 2 - 63, 324, 126};
}

static UiRect media_failed_retry_rect(UiRect message)
{
    return (UiRect) {message.x + 18, message.y + 72, 132, 36};
}

static UiRect media_failed_back_rect(UiRect message, bool retry_unavailable)
{
    return retry_unavailable
        ? (UiRect) {message.x + message.width - 150,
                    message.y + 84, 132, 32}
        : (UiRect) {message.x + message.width - 150,
                    message.y + 72, 132, 36};
}

static bool ui_rect_contains(UiRect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width
        && y >= rect.y && y < rect.y + rect.height;
}

PspUiMediaIntent psp_ui_media_activate_at(PspUiMediaState *media,
                                          int x, int y,
                                          int width, int height)
{
    PspUiMediaIntent intent = {0};
    if (media == NULL || !media->visible || width <= 0 || height <= 0
        || x < 0 || y < 0 || x >= width || y >= height) return intent;
    bool controls_were_hidden = !media->controls_visible;
    psp_ui_media_show_controls(media);
    intent.visual_changed = true;
    if (controls_were_hidden) return intent;
    if (media->failed) {
        UiRect message = media_failed_panel_rect(width, height);
        UiRect retry = media_failed_retry_rect(message);
        UiRect back = media_failed_back_rect(
            message, media->retry_unavailable);
        if (!media->retry_unavailable
            && ui_rect_contains(retry, x, y)) {
            intent.action = PSP_UI_MEDIA_ACTION_RETRY;
        } else if (ui_rect_contains(back, x, y)) {
            intent.action = PSP_UI_MEDIA_ACTION_CLOSE;
        }
        return intent;
    }
    /* The black startup surface is intentionally non-interactive. A pointer
       click must not toggle a backend which is only partially constructed;
       Circle is what stops an opening video. */
    if (media->resolving) return intent;
    int progress_y = height - 69;
    if (y >= progress_y - 8 && y <= progress_y + 10
        && media->duration_us != 0) {
        int left = 24, right = width - 24;
        if (x < left) x = left;
        if (x > right) x = right;
        intent.action = PSP_UI_MEDIA_ACTION_PREVIEW_SEEK;
        intent.seek_time_us = media_seek_position_time(
            (unsigned) (x - left), (unsigned) (right - left),
            media->duration_us);
        psp_ui_media_set_seek_preview(media, intent.seek_time_us);
        return intent;
    }
    intent.action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE;
    return intent;
}

/*
 * Four subpixel samples per axis, at the centres of the sixteen sub-cells a
 * pixel splits into. The engine's own rasterizer answers the same coverage
 * question for a page's rounded box with four samples
 * (rounded_rect_coverage_quarters, src/render/raster_primitives.inc); the
 * chrome blends in eighths, so sixteen samples are what that ladder can
 * actually spend.
 */
static const int ui_subpixel_offsets[4] = {1, 3, 5, 7};

/* Exact coverage produced by the sampler below for the two fixed player
   radii and the fixed play glyph. Keeping the sampled result in rodata
   removes thousands of multiplies from every controls-visible frame while
   retaining the generic sampler for variable geometry. */
static const uint8_t ui_round_12_coverage[144] = {
     0,  0,  0,  0,  0,  0,  0,  2,  7, 12, 15, 16,
     0,  0,  0,  0,  0,  2, 10, 16, 16, 16, 16, 16,
     0,  0,  0,  0,  6, 15, 16, 16, 16, 16, 16, 16,
     0,  0,  0,  6, 16, 16, 16, 16, 16, 16, 16, 16,
     0,  0,  6, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     0,  2, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     0, 10, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     2, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     7, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    12, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

static const uint8_t ui_round_11_coverage[121] = {
     0,  0,  0,  0,  0,  0,  1,  7, 11, 15, 16,
     0,  0,  0,  0,  1,  8, 15, 16, 16, 16, 16,
     0,  0,  0,  3, 13, 16, 16, 16, 16, 16, 16,
     0,  0,  3, 15, 16, 16, 16, 16, 16, 16, 16,
     0,  1, 13, 16, 16, 16, 16, 16, 16, 16, 16,
     0,  8, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     1, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     7, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    11, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

static const uint8_t ui_play_15_coverage[225] = {
    12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  8,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16, 16, 12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    12,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

/*
 * fill_round_rect decides each corner pixel with one inside/outside test, so
 * a 12px arc lands as five visible stair steps -- fine for a chip whose
 * radius is 4, wrong for the largest rounded silhouette the chrome draws.
 * This is the same decomposition with sampled corners, and it is deliberately
 * a separate primitive: every other panel keeps the cheap path.
 *
 * The three straight bands below do not overlap, unlike fill_round_rect's
 * two, whose intersection is blended twice and leaves a translucent surface
 * denser in the middle than along its own edges.
 */
static __attribute__((noinline)) void fill_round_rect_aa_resolved(
    uint16_t *pixels, int width, int height, int stride,
    UiRect rect, int radius, uint16_t color, unsigned opacity,
    uint16_t matte, bool edge_over_destination)
{
    unsigned parts = opacity >= 4u ? UI_BLEND_PARTS : opacity * 2u;
    if (parts == 0u) return;
    if (radius < 1 || rect.width < radius * 2 || rect.height < radius * 2) {
        uint16_t surface = parts >= UI_BLEND_PARTS
            ? color : blend565(color, matte, parts);
        fill_round_rect(pixels, width, height, stride, rect, radius,
                        surface, 4);
        return;
    }
    /*
     * Player chrome sits over a different decoded picture every frame. The
     * authored surface therefore resolves against one fixed matte and is
     * written opaquely. Most sampled fringes use that matte too. The light
     * play/pause rim is the exception: its outer fringe resolves against the
     * destination so a dark matte cannot leave an aliased halo around it.
     */
    uint16_t surface = parts >= UI_BLEND_PARTS
        ? color : blend565(color, matte, parts);
    int inner_top = rect.y + radius;
    int inner_height = rect.height - radius * 2;
    fill_rect(pixels, width, height, stride,
              (UiRect) {rect.x + radius, rect.y,
                        rect.width - radius * 2, rect.height},
              surface, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) {rect.x, inner_top, radius, inner_height},
              surface, 4);
    fill_rect(pixels, width, height, stride,
              (UiRect) {rect.x + rect.width - radius, inner_top,
                        radius, inner_height},
              surface, 4);
    const uint8_t *fixed_coverage = radius == 12
        ? ui_round_12_coverage
        : (radius == 11 ? ui_round_11_coverage : NULL);
    int radius_squared = radius * 8 * (radius * 8);
    for (unsigned corner = 0; corner < 4u; corner++) {
        bool right = (corner & 1u) != 0u;
        bool bottom = (corner & 2u) != 0u;
        int corner_x = right ? rect.x + rect.width - radius : rect.x;
        int corner_y = bottom ? rect.y + rect.height - radius : rect.y;
        int center_x = (right ? corner_x : corner_x + radius) * 8;
        int center_y = (bottom ? corner_y : corner_y + radius) * 8;
        for (int y = corner_y; y < corner_y + radius; y++) {
            if (y < 0 || y >= height) continue;
            int reach[4];
            if (fixed_coverage == NULL) {
                for (unsigned sy = 0; sy < 4u; sy++) {
                    int dy = y * 8 + ui_subpixel_offsets[sy] - center_y;
                    reach[sy] = radius_squared - dy * dy;
                }
            }
            uint16_t *row = pixels + (size_t) y * (size_t) stride;
            for (int x = corner_x; x < corner_x + radius; x++) {
                if (x < 0 || x >= width) continue;
                unsigned covered;
                if (fixed_coverage != NULL) {
                    int local_x = x - corner_x;
                    int local_y = y - corner_y;
                    if (right) local_x = radius - 1 - local_x;
                    if (bottom) local_y = radius - 1 - local_y;
                    covered = fixed_coverage[
                        (size_t) local_y * (size_t) radius
                        + (size_t) local_x];
                } else {
                    covered = 0;
                    for (unsigned sx = 0; sx < 4u; sx++) {
                        int dx = x * 8 + ui_subpixel_offsets[sx] - center_x;
                        int squared = dx * dx;
                        for (unsigned sy = 0; sy < 4u; sy++)
                            if (squared <= reach[sy]) covered++;
                    }
                }
                unsigned blend =
                    (covered * UI_BLEND_PARTS + 8u) / 16u;
                if (blend == 0u) continue;
                row[x] = blend >= UI_BLEND_PARTS
                    ? surface
                    : blend565(surface,
                               edge_over_destination ? row[x] : matte,
                               blend);
            }
        }
    }
}

/*
 * The pointer, drawn rather than typeset: a right-pointing isoceles triangle
 * whose flat edge is the left of a `size`-square box and whose apex is the
 * middle of the right. Because the flat edge sits on a pixel boundary it
 * needs no coverage, and the two slopes are symmetric about the box's
 * horizontal axis, so a row's reach is one subtraction.
 */
static __attribute__((noinline)) void fill_play_triangle_aa(
    uint16_t *pixels, int width, int height, int stride,
    int left, int top, int size, uint16_t color)
{
    const uint8_t *fixed_coverage = size == 15
        ? ui_play_15_coverage : NULL;
    int apex = (left + size) * 8;
    int axis = top * 8 + size * 4;
    for (int y = top; y < top + size; y++) {
        if (y < 0 || y >= height) continue;
        int reach[4];
        if (fixed_coverage == NULL) {
            for (unsigned sy = 0; sy < 4u; sy++) {
                int dy = y * 8 + ui_subpixel_offsets[sy] - axis;
                if (dy < 0) dy = -dy;
                reach[sy] = apex - dy * 2;
            }
        }
        uint16_t *row = pixels + (size_t) y * (size_t) stride;
        for (int x = left; x < left + size; x++) {
            if (x < 0 || x >= width) continue;
            unsigned covered;
            if (fixed_coverage != NULL) {
                covered = fixed_coverage[
                    (size_t) (y - top) * (size_t) size
                    + (size_t) (x - left)];
            } else {
                covered = 0;
                for (unsigned sx = 0; sx < 4u; sx++) {
                    int sample = x * 8 + ui_subpixel_offsets[sx];
                    for (unsigned sy = 0; sy < 4u; sy++)
                        if (sample <= reach[sy]) covered++;
                }
            }
            if (covered == 0u) continue;
            unsigned parts = (covered + 1u) / 2u;
            row[x] = parts >= UI_BLEND_PARTS
                ? color : blend565(color, row[x], parts);
        }
    }
}

/*
 * The watch page's own overlay, which this control is held to, is authored in
 * src/youtube_lite.c: `.play-icon` reserves a 52x40 box over the thumbnail
 * and paints a 50x38 rounded rectangle inset by one pixel, radius 12,
 * #25282b at fill-opacity .76; `.play-glyph` centres a #e4e6e8 pointer in the
 * same box, which the engine renders about 14px square. Those numbers are
 * this control's, scaled to nothing and tinted by nothing. The fullscreen
 * player adds a one-pixel light rim inside that silhouette: the thumbnail
 * always has image context around it, while the player badge must remain
 * legible over a nearly-black decoded frame. The rim and fill use the same
 * sampled rounded primitive. Its outer fringe resolves against the decoded
 * picture rather than black, while its inner fringe resolves against the
 * light rim, so neither side exposes a dark rectangular halo.
 *
 * What it replaces: a mid-grey surface under a lighter halo, both with
 * stepped corners, and a pointer taken from the chrome face at U+25BA. That
 * codepoint is absent from the shipped Latin face on the device and on the
 * host, so the fallback ran every frame and drew a 21-row scanline staircase
 * a third taller than it was wide.
 */
#define UI_MEDIA_BADGE_WIDTH 50
#define UI_MEDIA_BADGE_HEIGHT 38
#define UI_MEDIA_BADGE_RADIUS 12
#define UI_MEDIA_BADGE_OUTLINE 1
#define UI_MEDIA_GLYPH_SIZE 15

static void draw_media_play_pause(uint16_t *pixels, int width, int height,
                                  int stride, int center_x, int center_y,
                                  bool playing)
{
    uint16_t badge = rgb565(37, 40, 43);
    uint16_t color = rgb565(228, 230, 232);
    UiRect badge_bounds = {
        center_x - UI_MEDIA_BADGE_WIDTH / 2,
        center_y - UI_MEDIA_BADGE_HEIGHT / 2,
        UI_MEDIA_BADGE_WIDTH, UI_MEDIA_BADGE_HEIGHT
    };
    /* Resolve the rim's outer fringe against the actual picture. A dark
       matte leaves a visible aliased seam outside the light outline. */
    fill_round_rect_aa_resolved(
        pixels, width, height, stride, badge_bounds,
        UI_MEDIA_BADGE_RADIUS, color, 4, rgb565(0, 0, 0), true);
    UiRect badge_fill = {
        badge_bounds.x + UI_MEDIA_BADGE_OUTLINE,
        badge_bounds.y + UI_MEDIA_BADGE_OUTLINE,
        badge_bounds.width - UI_MEDIA_BADGE_OUTLINE * 2,
        badge_bounds.height - UI_MEDIA_BADGE_OUTLINE * 2
    };
    fill_round_rect_aa_resolved(
        pixels, width, height, stride, badge_fill,
        UI_MEDIA_BADGE_RADIUS - UI_MEDIA_BADGE_OUTLINE,
        badge, 4, color, false);
    int top = center_y - UI_MEDIA_GLYPH_SIZE / 2;
    if (playing) {
        /* Two bars in the pointer's own 15px envelope, softened at the ends
           by the same sampler, so neither state looks heavier than the
           other when CROSS swaps them. */
        fill_round_rect_aa_resolved(
            pixels, width, height, stride,
            (UiRect) {center_x - 7, top, 4, UI_MEDIA_GLYPH_SIZE},
            1, color, 4, badge, false);
        fill_round_rect_aa_resolved(
            pixels, width, height, stride,
            (UiRect) {center_x + 4, top, 4, UI_MEDIA_GLYPH_SIZE},
            1, color, 4, badge, false);
        return;
    }
    /* A triangle centred on its bounding box reads as sitting left of
       centre, because its mass is not. One pixel of rightward bias is what
       that costs at this size. */
    fill_play_triangle_aa(
        pixels, width, height, stride,
        center_x - UI_MEDIA_GLYPH_SIZE / 2 + 1, top,
        UI_MEDIA_GLYPH_SIZE, color);
}

static void draw_media_buffering(uint16_t *pixels, int width, int height,
                                 int stride, int center_x, int center_y)
{
    UiRect pill = {center_x - 63, center_y - 19, 126, 38};
    /* Keep the message ground and its sampled fringe independent of the
       moving picture. The old blend changed the badge at video cadence,
       which reads as chrome shimmer during a short refill. */
    fill_round_rect_aa_resolved(
        pixels, width, height, stride, pill, 12,
        rgb565(37, 40, 43), 4, rgb565(0, 0, 0), false);
    const char *label = "Buffering...";
    int label_width = chrome_text_width_bytes(
        label, strlen(label), 2, false);
    draw_text_with_font(
        pixels, width, height, stride,
        center_x - label_width / 2, center_y - 8,
        label, strlen(label), pill.x + pill.width - 10,
        rgb565(228, 230, 232), 2, NULL, false);
}

/* The overlay's geometry, named once so the composite below and the band
   report above it cannot describe different rectangles. */
#define UI_MEDIA_TITLE_BAR_HEIGHT 42
#define UI_MEDIA_CONTROL_BAR_HEIGHT 78
#define UI_MEDIA_PREVIEW_TOP 48
#define UI_MEDIA_PREVIEW_HEIGHT 72
#define UI_MEDIA_PREVIEW_PANEL_MARGIN 3
#define UI_MEDIA_PREVIEW_PANEL_EXTRA 31
#define UI_MEDIA_BUFFERING_REGION_HALF_WIDTH 80

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
    bool needs_backdrop;
} UiMediaRawRegion;

#define UI_MEDIA_RAW_REGION_LIMIT 6u

static UiRect media_track_menu_rect(int width, int height)
{
    (void) height;
    return (UiRect) {
        width / 2 - (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH / 2,
        48,
        (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
        (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT
    };
}

static UiRect media_subtitle_rect(
    const PspUiMediaState *media, int width, int height)
{
    int bottom = media != NULL && media->controls_visible
        ? height - UI_MEDIA_CONTROL_BAR_HEIGHT - 8 : height - 18;
    bool small = media != NULL && media->presentation != NULL
        && media->presentation->subtitle_size
               == BROWSER_SUBTITLE_SIZE_SMALL;
    /* Keep the opaque ground stable while adjacent cues alternate between
       one and two rows. Resizing this rectangle at a cue boundary reads as
       black flicker against moving video on the physical panel. One-row text
       is centered in the same bounded two-row box. */
    int subtitle_height = small ? 36 : 50;
    return (UiRect) {
        width / 2 - 202, bottom - subtitle_height, 404, subtitle_height
    };
}

static int media_badge_center_y(
    const PspUiMediaState *media, int height)
{
    if (media != NULL && media->controls_visible
        && media->presentation != NULL
        && media->presentation->subtitle_text[0] != '\0') {
        /* The two-row caption ground grows upward from the control bar. Lift
           the transient play/buffering badge into the free picture above it
           rather than allowing the opaque caption ground to cover the lower
           edge of the badge. */
        return height / 2 - 26;
    }
    return height / 2;
}

static void ui_media_add_raw_region(
    UiMediaRawRegion *raw, size_t *count, int left, int top,
    int right, int bottom, int width, int height, bool needs_backdrop)
{
    if (raw == NULL || count == NULL || *count >= UI_MEDIA_RAW_REGION_LIMIT)
        return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) return;
    raw[*count] = (UiMediaRawRegion) {
        left, top, right, bottom, needs_backdrop
    };
    (*count)++;
}

static void ui_media_sort_unique_edges(int *edges, size_t *count)
{
    if (edges == NULL || count == NULL) return;
    for (size_t at = 1; at < *count; at++) {
        int value = edges[at];
        size_t before = at;
        while (before != 0u && edges[before - 1u] > value) {
            edges[before] = edges[before - 1u];
            before--;
        }
        edges[before] = value;
    }
    size_t unique = 0;
    for (size_t at = 0; at < *count; at++) {
        if (unique == 0u || edges[at] != edges[unique - 1u])
            edges[unique++] = edges[at];
    }
    *count = unique;
}

static bool ui_media_emit_region(
    PspUiOverlayRegion *regions, size_t *count,
    const PspUiOverlayRegion *region)
{
    if (regions == NULL || count == NULL || region == NULL) return false;
    /* Identical horizontal spans in adjacent sweep strips are one rectangle,
       so ordinary player states retain only three or four regions. */
    for (size_t at = 0; at < *count; at++) {
        PspUiOverlayRegion *previous = &regions[at];
        if (previous->left == region->left
            && previous->right == region->right
            && previous->bottom == region->top
            && previous->needs_backdrop == region->needs_backdrop) {
            previous->bottom = region->bottom;
            return true;
        }
    }
    if (*count >= PSP_UI_MEDIA_OVERLAY_REGION_LIMIT) return false;
    regions[(*count)++] = *region;
    return true;
}

static size_t ui_media_union_regions(
    const UiMediaRawRegion *raw, size_t raw_count,
    int width, int height, PspUiOverlayRegion *regions, size_t capacity)
{
    if (raw_count == 0u || regions == NULL || capacity == 0u) return 0u;
    int x_edges[UI_MEDIA_RAW_REGION_LIMIT * 2u];
    int y_edges[UI_MEDIA_RAW_REGION_LIMIT * 2u];
    size_t x_count = 0u, y_count = 0u;
    for (size_t at = 0; at < raw_count; at++) {
        x_edges[x_count++] = raw[at].left;
        x_edges[x_count++] = raw[at].right;
        y_edges[y_count++] = raw[at].top;
        y_edges[y_count++] = raw[at].bottom;
    }
    ui_media_sort_unique_edges(x_edges, &x_count);
    ui_media_sort_unique_edges(y_edges, &y_count);

    PspUiOverlayRegion exact[PSP_UI_MEDIA_OVERLAY_REGION_LIMIT];
    size_t exact_count = 0u;
    bool overflow = false;
    for (size_t y = 0; y + 1u < y_count && !overflow; y++) {
        PspUiOverlayRegion strip[UI_MEDIA_RAW_REGION_LIMIT * 2u];
        size_t strip_count = 0u;
        for (size_t x = 0; x + 1u < x_count; x++) {
            bool covered = false;
            bool needs_backdrop = false;
            for (size_t at = 0; at < raw_count; at++) {
                bool contains = raw[at].left <= x_edges[x]
                    && raw[at].right >= x_edges[x + 1u]
                    && raw[at].top <= y_edges[y]
                    && raw[at].bottom >= y_edges[y + 1u];
                if (!contains) continue;
                covered = true;
                needs_backdrop = needs_backdrop || raw[at].needs_backdrop;
            }
            if (!covered) continue;
            if (strip_count != 0u
                && strip[strip_count - 1u].right == x_edges[x]
                && strip[strip_count - 1u].needs_backdrop
                       == needs_backdrop) {
                strip[strip_count - 1u].right = x_edges[x + 1u];
            } else {
                strip[strip_count++] = (PspUiOverlayRegion) {
                    x_edges[x], y_edges[y], x_edges[x + 1u],
                    y_edges[y + 1u], needs_backdrop
                };
            }
        }
        for (size_t at = 0; at < strip_count; at++) {
            if (!ui_media_emit_region(exact, &exact_count, &strip[at])) {
                overflow = true;
                break;
            }
        }
    }
    if (overflow || exact_count > capacity) {
        /* The fixed player geometry never reaches this path. If a future
           overlay exceeds the compact union, retain correctness by importing
           one full backdrop rather than silently omitting written pixels. */
        regions[0] = (PspUiOverlayRegion) {
            0, 0, width, height, true
        };
        return 1u;
    }
    memcpy(regions, exact, exact_count * sizeof(*regions));
    return exact_count;
}

static size_t ui_media_overlay_regions(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    int width, int height, PspUiOverlayRegion *regions, size_t capacity,
    bool include_track_menu)
{
    if (media == NULL || !media->visible || regions == NULL || capacity == 0u
        || width <= 0 || height <= 0) return 0u;
    const PspUiMediaPresentation *presentation = media->presentation;
    UiMediaRawRegion raw[UI_MEDIA_RAW_REGION_LIMIT];
    size_t raw_count = 0u;
    if (!media->controls_visible) {
        if (media->buffering) {
            ui_media_add_raw_region(
                raw, &raw_count,
                width / 2 - UI_MEDIA_BUFFERING_REGION_HALF_WIDTH,
                height / 2 - 19,
                width / 2 + UI_MEDIA_BUFFERING_REGION_HALF_WIDTH,
                height / 2 + 19,
                width, height, true);
        }
        if (presentation != NULL && presentation->subtitle_text[0] != '\0') {
            UiRect subtitle = media_subtitle_rect(media, width, height);
            ui_media_add_raw_region(
                raw, &raw_count, subtitle.x, subtitle.y,
                subtitle.x + subtitle.width, subtitle.y + subtitle.height,
                width, height,
                presentation->subtitle_background
                    == BROWSER_SUBTITLE_BACKGROUND_SHADOW);
        }
        return ui_media_union_regions(
            raw, raw_count, width, height, regions, capacity);
    }
    bool track_menu_open = presentation != NULL
        && presentation->track_menu_open;
    /* The chooser is modal and already carries its own opaque ground. Do not
       spend a video frame painting the title, timeline, badge, or subtitles
       behind it; on the PSP those obscured layers materially slow decoding. */
    if (track_menu_open) {
        if (include_track_menu) {
            UiRect menu = media_track_menu_rect(width, height);
            ui_media_add_raw_region(
                raw, &raw_count, menu.x, menu.y,
                menu.x + menu.width, menu.y + menu.height,
                width, height, false);
        }
        return ui_media_union_regions(
            raw, raw_count, width, height, regions, capacity);
    }
    ui_media_add_raw_region(
        raw, &raw_count, 0, 0, width, UI_MEDIA_TITLE_BAR_HEIGHT,
        width, height, false);
    if (media->resolving || media->failed) {
        UiRect message = media->failed
            ? media_failed_panel_rect(width, height)
            : (UiRect) {width / 2 - 152, height / 2 - 41, 304, 82};
        ui_media_add_raw_region(
            raw, &raw_count, message.x, message.y,
            message.x + message.width + 4,
            message.y + message.height + 5,
            width, height, true);
        if (media->resolving) {
            ui_media_add_raw_region(
                raw, &raw_count, 0,
                height - UI_MEDIA_CONTROL_BAR_HEIGHT, width, height,
                width, height, false);
        }
        return ui_media_union_regions(
            raw, raw_count, width, height, regions, capacity);
    }
    if (media->seek_preview_active && preview != NULL
        && preview->pixels != NULL && preview->width > 0
        && preview->height > 0 && preview->stride >= preview->width) {
        int top = UI_MEDIA_PREVIEW_TOP - UI_MEDIA_PREVIEW_PANEL_MARGIN;
        ui_media_add_raw_region(
            raw, &raw_count,
            18 - UI_MEDIA_PREVIEW_PANEL_MARGIN, top,
            18 + 128 + UI_MEDIA_PREVIEW_PANEL_MARGIN,
            top + UI_MEDIA_PREVIEW_HEIGHT + UI_MEDIA_PREVIEW_PANEL_EXTRA,
            width, height, true);
    }
    int badge_center_y = media_badge_center_y(media, height);
    int half_width = media->buffering
        ? UI_MEDIA_BUFFERING_REGION_HALF_WIDTH
        : UI_MEDIA_BADGE_WIDTH / 2;
    ui_media_add_raw_region(
        raw, &raw_count, width / 2 - half_width,
        badge_center_y - UI_MEDIA_BADGE_HEIGHT / 2,
        width / 2 + half_width,
        badge_center_y - UI_MEDIA_BADGE_HEIGHT / 2
            + UI_MEDIA_BADGE_HEIGHT,
        width, height, true);
    ui_media_add_raw_region(
        raw, &raw_count, 0, height - UI_MEDIA_CONTROL_BAR_HEIGHT,
        width, height, width, height, false);
    if (presentation != NULL && presentation->subtitle_text[0] != '\0') {
        UiRect subtitle = media_subtitle_rect(media, width, height);
        ui_media_add_raw_region(
            raw, &raw_count, subtitle.x, subtitle.y,
            subtitle.x + subtitle.width, subtitle.y + subtitle.height,
            width, height,
            presentation->subtitle_background
                == BROWSER_SUBTITLE_BACKGROUND_SHADOW);
    }
    return ui_media_union_regions(
        raw, raw_count, width, height, regions, capacity);
}

size_t psp_ui_media_overlay_regions(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    int width, int height, PspUiOverlayRegion *regions, size_t capacity)
{
    return ui_media_overlay_regions(
        media, preview, width, height, regions, capacity, true);
}

size_t psp_ui_media_overlay_regions_without_track_menu(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    int width, int height, PspUiOverlayRegion *regions, size_t capacity)
{
    return ui_media_overlay_regions(
        media, preview, width, height, regions, capacity, false);
}

static void draw_media_control_bar_ground(
    uint16_t *pixels, int width, int height, int stride)
{
    int bar_top = height - UI_MEDIA_CONTROL_BAR_HEIGHT;
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, bar_top, width, height - bar_top},
              PSP_THEME_CHROME_BAR, 4);
    /* Control legend sits on the darker hint ground, like the page chrome. */
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, height - 38, width, 38}, PSP_THEME_HINT_BAR, 3);
}

static void draw_media_control_bar(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    uint16_t text = PSP_THEME_TEXT;
    uint16_t muted = PSP_THEME_TEXT_MUTED;
    uint16_t accent = PSP_THEME_ACCENT_EMBER;
    bool tracks_available = media->presentation != NULL
        && (media->presentation->audio_track_count != 0
            || media->presentation->subtitle_track_count != 0);
    draw_media_control_bar_ground(pixels, width, height, stride);
    int left = 24, right = width - 24, track_y = height - 69;
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) {left, track_y, right - left, 4}, 2,
                    PSP_THEME_TEXT_FAINT, 4);
    if (media->live) {
        /* A rolling window has no stable zero or duration. Keep the familiar
           control surface but make its timeline explicitly non-seekable. */
        fill_round_rect(pixels, width, height, stride,
                        (UiRect) {right - 26, track_y, 26, 4}, 2,
                        accent, 4);
        fill_round_rect(pixels, width, height, stride,
                        (UiRect) {right - 5, track_y - 3, 10, 10}, 5,
                        PSP_THEME_ACCENT_EMBER_HI, 4);
        draw_text(pixels, width, height, stride, left, height - 53,
                  "LIVE", 8, text, 2);
        draw_text(pixels, width, height, stride, left, height - 29,
                  tracks_available
                      ? (media->playing
                          ? "X PAUSE  TRI TRACKS"
                          : "X PLAY  TRI TRACKS")
                      : (media->playing ? "X PAUSE" : "X PLAY"),
                  24, muted, 2);
        return;
    }
    if (media->duration_us != 0 && media->buffered_visual_pixel != 0u) {
        int buffered_width = (int) (
            (unsigned) media->buffered_visual_pixel
                * (unsigned) (right - left)
            / UI_MEDIA_TIMELINE_PIXELS);
        if (buffered_width > 0)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {left, track_y, buffered_width, 4},
                2, muted, 4);
    }
    int filled = (int) (
        (unsigned) media->timeline_visual_pixel
            * (unsigned) (right - left)
        / UI_MEDIA_TIMELINE_PIXELS);
    uint64_t display_time = media->seek_preview_active
        ? media->seek_preview_time_us : media->current_time_us;
    if (filled > 0) {
        fill_round_rect(pixels, width, height, stride,
                        (UiRect) {left, track_y, filled, 4},
                        2, accent, 4);
    }
    fill_round_rect(pixels, width, height, stride,
                    (UiRect) {left + filled - 4, track_y - 3, 10, 10},
                    5, PSP_THEME_ACCENT_EMBER_HI, 4);
    char current[24], duration[24], legend[64];
    media_format_time(display_time, current, sizeof(current));
    media_format_time(media->duration_us, duration, sizeof(duration));
    snprintf(legend, sizeof(legend), "%s%s / %s",
             media->seek_preview_active ? "SEEK " : "",
             current, duration);
    draw_text(pixels, width, height, stride, left, height - 53,
              legend, 30, text, 2);
    draw_text(pixels, width, height, stride, left, height - 29,
              media->seek_preview_active
                  ? "X GO   O CANCEL"
                  : tracks_available
                      ? (media->playing
                          ? "X PAUSE  TRI TRACKS  L/R SEEK"
                          : "X PLAY  TRI TRACKS  L/R SEEK")
                      : (media->playing
                          ? "STICK/L/R SEEK  X PAUSE"
                          : "STICK/L/R SEEK  X PLAY"),
              32, muted, 2);
}

static void draw_media_subtitle(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    if (media == NULL || media->presentation == NULL
        || media->presentation->subtitle_text[0] == '\0') return;
    const PspUiMediaPresentation *presentation = media->presentation;
    UiRect box = media_subtitle_rect(media, width, height);
    int scale = presentation->subtitle_size == BROWSER_SUBTITLE_SIZE_SMALL
        ? 1 : 2;
    char lines[2][PSP_UI_MEDIA_SUBTITLE_TEXT_CAPACITY] = {{0}};
    const char *text = presentation->subtitle_text;
    size_t bytes = strlen(text);
    size_t at = 0, last_break = SIZE_MAX, after_break = 0;
    int line_width = 0;
    int maximum_width = box.width - 20;
    while (at < bytes) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, bytes - at, &codepoint);
        if (used == 0) break;
        bool known_missing = false;
        const FontGlyph *glyph = chrome_font_glyph(
            codepoint, scale, false, &known_missing);
        int advance = glyph != NULL ? glyph->advance : 6 * scale;
        if (advance < 0) advance = 0;
        if (codepoint == '\n' || line_width > maximum_width - advance) break;
        if (codepoint == ' ' || codepoint == '\t') {
            last_break = at;
            after_break = at + used;
        }
        line_width += advance;
        at += used;
    }
    size_t first_bytes = bytes;
    size_t second_at = bytes;
    if (at < bytes) {
        if (last_break != SIZE_MAX) {
            first_bytes = last_break;
            second_at = after_break;
        } else if (at != 0) {
            first_bytes = at;
            second_at = at;
        } else {
            unsigned ignored = 0;
            size_t used = font_utf8_next(text, bytes, &ignored);
            first_bytes = used;
            second_at = used;
        }
        while (second_at < bytes
               && (text[second_at] == ' ' || text[second_at] == '\t'
                   || text[second_at] == '\n' || text[second_at] == '\r'))
            second_at++;
    }
    if (first_bytes >= sizeof(lines[0])) first_bytes = sizeof(lines[0]) - 1u;
    memcpy(lines[0], text, first_bytes);
    lines[0][first_bytes] = '\0';
    size_t second_bytes = bytes - second_at;
    if (second_bytes >= sizeof(lines[1]))
        second_bytes = sizeof(lines[1]) - 1u;
    memcpy(lines[1], text + second_at, second_bytes);
    lines[1][second_bytes] = '\0';
    unsigned line_count = lines[1][0] == '\0' ? 1u : 2u;
    int line_step = scale == 1 ? 14 : 20;
    int text_height = (int) line_count * line_step;
    int text_y = box.y + (box.height - text_height) / 2;
    if (presentation->subtitle_background
            == BROWSER_SUBTITLE_BACKGROUND_BOX) {
        /* An opaque ground avoids importing the moving 8888 picture for
           every captioned frame. */
        fill_rect(pixels, width, height, stride, box,
                  PSP_THEME_CHROME_BAR, 4);
    }
    for (unsigned line = 0; line < line_count; line++) {
        int y = text_y + (int) line * line_step;
        if (presentation->subtitle_background
                == BROWSER_SUBTITLE_BACKGROUND_SHADOW) {
            draw_text_with_font(
                pixels, width, height, stride, box.x + 11, y + 1,
                lines[line], 96, box.x + box.width - 9,
                rgb565(0, 0, 0), scale, NULL, false);
        }
        draw_text_with_font(
            pixels, width, height, stride, box.x + 10, y,
            lines[line], 96, box.x + box.width - 10,
            PSP_THEME_TEXT, scale, NULL, false);
    }
}

static void draw_media_track_menu_box(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride, UiRect box)
{
    if (media == NULL || media->presentation == NULL
        || !media->presentation->track_menu_open) return;
    const PspUiMediaPresentation *presentation = media->presentation;
    /* This menu covers almost half the display and remains visible for many
       video frames. Its surface is intentionally opaque so the 8888 bridge
       can clear and redraw it without narrowing 61,952 moving video pixels
       to RGB565 on every frame. */
    fill_rect(pixels, width, height, stride, box, PSP_THEME_PANEL, 4);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 16, box.y + 12, "Audio", 5,
                   presentation->track_menu_tab == 0
                       ? PSP_THEME_ACCENT_EMBER_HI : PSP_THEME_TEXT_MUTED, 2);
    draw_text_bold(pixels, width, height, stride,
                   box.x + 110, box.y + 12, "Subtitles", 9,
                   presentation->track_menu_tab == 1
                       ? PSP_THEME_ACCENT_EMBER_HI : PSP_THEME_TEXT_MUTED, 2);
    draw_panel_rule(pixels, width, height, stride, box, box.y + 35);
    unsigned count = presentation->track_menu_tab == 0
        ? presentation->audio_track_count
        : (unsigned) presentation->subtitle_track_count + 1u;
    for (unsigned at = 0; at < count && at < PSP_UI_MEDIA_TRACK_LIMIT + 1u;
         at++) {
        int y = box.y + 44 + (int) at * 16;
        bool selected = presentation->track_menu_tab == 0
            ? presentation->selected_audio_track == (int) at
            : (at == 0 ? presentation->selected_subtitle_track < 0
                       : presentation->selected_subtitle_track
                             == (int) at - 1);
        if (at == presentation->track_menu_selection)
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {box.x + 10, y - 3, box.width - 20, 17},
                4, PSP_THEME_SURFACE, 4);
        const char *label = presentation->track_menu_tab == 0
            ? presentation->audio_tracks[at].label
            : (at == 0 ? "Off"
                       : presentation->subtitle_tracks[at - 1u].label);
        draw_text_with_font(
            pixels, width, height, stride, box.x + 20, y,
            label, 42, box.x + box.width - 42,
            at == presentation->track_menu_selection
                ? PSP_THEME_TEXT : PSP_THEME_TEXT_MUTED,
            1, NULL, false);
        if (selected)
            draw_text(pixels, width, height, stride,
                      box.x + box.width - 30, y, "X", 1,
                      PSP_THEME_ACCENT_EMBER_HI, 1);
    }
    draw_text(pixels, width, height, stride,
              box.x + 16, box.y + box.height - 17,
              "L/R CATEGORY   X SELECT   O BACK", 34,
              PSP_THEME_TEXT_MUTED, 1);
}

static void draw_media_track_menu(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    draw_media_track_menu_box(
        media, pixels, width, height, stride,
        media_track_menu_rect(width, height));
}

void psp_ui_media_raster_track_menu(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    if (width < (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH
        || height < (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT) return;
    draw_media_track_menu_box(
        media, pixels, width, height, stride,
        (UiRect) {0, 0, (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
                  (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT});
}

void psp_ui_media_composite_controls(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    if (media == NULL || !media->visible || !media->controls_visible
        || media->failed || pixels == NULL
        || width <= 0 || height < UI_MEDIA_CONTROL_BAR_HEIGHT
        || stride < width) return;
    if (media->resolving) {
        if (media->seek_in_progress)
            draw_media_control_bar(media, pixels, width, height, stride);
        else
            draw_media_control_bar_ground(pixels, width, height, stride);
        return;
    }
    draw_media_control_bar(media, pixels, width, height, stride);
}

void psp_ui_media_composite_supervisor_565(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride)
{
    if (media != NULL && media->visible && media->resolving
        && !media->seek_in_progress && pixels != NULL
        && width > 0 && height > 0 && stride >= width) {
        /* Fresh opens remain on the 565 loading stage until a picture exists.
           The supervisor owns presentation throughout asynchronous resolve;
           updating only the footer copied the original central "0%" forever.
           Rebuild on a clean stage, not already blended panel edges. The
           active 8888 path and picture-preserving seek path remain unchanged. */
        for (int y = 0; y < height; y++)
            memset(pixels + (size_t) y * (size_t) stride, 0,
                   (size_t) width * sizeof(*pixels));
        psp_ui_media_composite(media, pixels, width, height, stride);
        return;
    }
    psp_ui_media_composite_controls(media, pixels, width, height, stride);
}

bool psp_ui_media_overlay_paints(const PspUiMediaState *media)
{
    return media != NULL && media->visible
        && (media->controls_visible || media->buffering
            || (media->presentation != NULL
                && media->presentation->subtitle_text[0] != '\0'));
}

void psp_ui_media_composite_layers(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint16_t *pixels, int width, int height, int stride,
    bool include_track_menu)
{
    if (!psp_ui_media_overlay_paints(media) || pixels == NULL
        || width <= 0 || height <= 0 || stride < width) return;
    if (!media->controls_visible) {
        if (media->buffering)
            draw_media_buffering(
                pixels, width, height, stride, width / 2, height / 2);
        draw_media_subtitle(media, pixels, width, height, stride);
        return;
    }
    bool track_menu_open = media->presentation != NULL
        && media->presentation->track_menu_open;
    if (track_menu_open) {
        if (include_track_menu)
            draw_media_track_menu(media, pixels, width, height, stride);
        return;
    }
    /* Ember overlay language: warm near-black chrome, token text, accent
       scrubber and play glyph. All static chrome uses a stable opaque ground.
       Moving video behind an RGB565 blend visibly shimmers at glyph and shape
       edges on the physical panel. */
    uint16_t bar = PSP_THEME_CHROME_BAR;
    uint16_t panel = PSP_THEME_PANEL;
    uint16_t text = PSP_THEME_TEXT;
    uint16_t muted = PSP_THEME_TEXT_MUTED;
    uint16_t accent = PSP_THEME_ACCENT_EMBER;
    fill_rect(pixels, width, height, stride,
              (UiRect) {0, 0, width, UI_MEDIA_TITLE_BAR_HEIGHT}, bar, 4);
    draw_text_with_font(
        pixels, width, height, stride, 14, 12, media->title, 34,
        width - 46, text, 2,
        media->presentation == NULL
            ? NULL : media->presentation->title_font,
        false);

    if (media->resolving || media->failed) {
        if (media->resolving) {
            /* A decoded frame may already own the 32-bit scanout during
               Priming. Paint the eventual bar's stable ground now so the
               loading-to-playing handoff changes controls, not 78 rows of
               moving picture. */
            if (media->seek_in_progress)
                draw_media_control_bar(
                    media, pixels, width, height, stride);
            else
                draw_media_control_bar_ground(
                    pixels, width, height, stride);
        }
        UiRect message = media->failed
            ? media_failed_panel_rect(width, height)
            : (UiRect) {width / 2 - 152, height / 2 - 41, 304, 82};
        draw_panel_shell(pixels, width, height, stride, message);
        char headline[PSP_UI_STATUS_CAPACITY];
        const char *reason = strchr(media->status, '\n');
        copy_string(headline, sizeof(headline), media->status);
        if (reason != NULL) {
            headline[(size_t) (reason - media->status)] = '\0';
            reason++;
        }
        draw_text_with_font(
            pixels, width, height, stride,
            message.x + 18, message.y + 17,
            headline, 32, message.x + message.width - 18,
            media->failed ? rgb565(248, 160, 160) : text,
            2, NULL, false);
        if (reason != NULL) {
            draw_text_with_font(
                pixels, width, height, stride,
                message.x + 18, message.y + 36,
                reason, 34, message.x + message.width - 18,
                muted, 2, NULL, false);
        }
        if (media->resolving) {
            int track_x = message.x + 18;
            int track_y = message.y + 50;
            int track_width = message.width - 74;
            fill_round_rect(
                pixels, width, height, stride,
                (UiRect) {track_x, track_y, track_width, 5}, 2,
                PSP_THEME_TEXT_FAINT, 4);
            unsigned progress = media->resolving_progress_per_mille > 1000u
                ? 1000u : media->resolving_progress_per_mille;
            int filled = (int) ((unsigned) track_width
                * progress / 1000u);
            if (filled > 0)
                fill_round_rect(
                    pixels, width, height, stride,
                    (UiRect) {track_x, track_y, filled, 5}, 2,
                    accent, 4);
            char percent[8];
            snprintf(percent, sizeof(percent), "%u%%",
                     progress / 10u);
            draw_text_right_aligned(
                pixels, width, height, stride,
                message.x + message.width - 16, message.y + 44,
                percent, 5, muted, 1, false);
        } else {
            draw_panel_rule(
                pixels, width, height, stride, message,
                message.y + 55);
            UiRect back = media_failed_back_rect(
                message, media->retry_unavailable);
            if (media->retry_unavailable) {
                /* A quarantined firmware decoder refuses every later open
                   for the rest of the process, so the only true affordance
                   is the restart. Name it where Retry used to sit rather
                   than drawing a button that can only fail again. */
                draw_text_with_font(
                    pixels, width, height, stride,
                    message.x + 18, message.y + 63,
                    "Restart Tilefinch to use video", 30,
                    message.x + message.width - 18, muted, 2, NULL, false);
            } else {
                UiRect retry = media_failed_retry_rect(message);
                fill_round_rect(
                    pixels, width, height, stride, retry,
                    PSP_THEME_RADIUS_CHIP, accent, 4);
                int retry_width = chrome_text_width_bytes(
                    "X Retry", strlen("X Retry"), 2, true);
                draw_text_bold(
                    pixels, width, height, stride,
                    retry.x + (retry.width - retry_width) / 2,
                    retry.y + 9, "X Retry", 7,
                    PSP_THEME_ON_ACCENT, 2);
            }
            fill_round_rect(
                pixels, width, height, stride, back,
                PSP_THEME_RADIUS_CHIP, PSP_THEME_SURFACE, 4);
            int back_width = chrome_text_width_bytes(
                "O Back", strlen("O Back"), 2, true);
            draw_text_bold(
                pixels, width, height, stride,
                back.x + (back.width - back_width) / 2,
                back.y + (back.height - 18) / 2, "O Back", 6, text, 2);
            if (!media->retry_unavailable
                && (media->audio_only_recovery_available
                    || media->lower_quality_recovery_available)) {
                const char *recovery =
                    media->audio_only_recovery_available
                        && media->lower_quality_recovery_available
                    ? "Triangle Audio-only   Square 240p"
                    : media->audio_only_recovery_available
                        ? "Triangle Audio-only"
                        : "Square 240p";
                draw_text_with_font(
                    pixels, width, height, stride,
                    message.x + 18, message.y + 112, recovery, 38,
                    message.x + message.width - 18, muted, 1, NULL, false);
            }
        }
        return;
    }

    if (media->seek_preview_active && preview != NULL
        && preview->pixels != NULL && preview->width > 0
        && preview->height > 0 && preview->stride >= preview->width) {
        int preview_width = 128;
        int preview_height = UI_MEDIA_PREVIEW_HEIGHT;
        int preview_left = 18;
        int preview_top = UI_MEDIA_PREVIEW_TOP;
        fill_round_rect(
            pixels, width, height, stride,
            (UiRect) {preview_left - UI_MEDIA_PREVIEW_PANEL_MARGIN,
                      preview_top - UI_MEDIA_PREVIEW_PANEL_MARGIN,
                      preview_width + 2 * UI_MEDIA_PREVIEW_PANEL_MARGIN,
                      preview_height + UI_MEDIA_PREVIEW_PANEL_EXTRA},
            PSP_THEME_RADIUS_PANEL, panel, 5);
        fill_rect(
            pixels, width, height, stride,
            (UiRect) {preview_left, preview_top,
                      preview_width, preview_height}, bar, 6);
        for (int y = 0; y < preview_height; y++) {
            int source_y = y * preview->height / preview_height;
            uint16_t *destination = pixels
                + (size_t) (preview_top + y) * stride + preview_left;
            const uint16_t *source = preview->pixels
                + (size_t) source_y * preview->stride;
            for (int x = 0; x < preview_width; x++)
                destination[x] =
                    source[x * preview->width / preview_width];
        }
        char target[24];
        media_format_time(
            media->seek_preview_time_us, target, sizeof(target));
        draw_text(pixels, width, height, stride,
                  preview_left + 6, preview_top + preview_height + 7,
                  target, 9, text, 2);
    }

    int badge_center_y = media_badge_center_y(media, height);
    if (media->buffering)
        draw_media_buffering(
            pixels, width, height, stride, width / 2, badge_center_y);
    else
        draw_media_play_pause(pixels, width, height, stride,
                              width / 2, badge_center_y,
                              media->playing && !media->ended);

    draw_media_control_bar(media, pixels, width, height, stride);
    draw_media_subtitle(media, pixels, width, height, stride);
}

void psp_ui_media_composite_with_preview(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint16_t *pixels, int width, int height, int stride)
{
    psp_ui_media_composite_layers(
        media, preview, pixels, width, height, stride, true);
}

void psp_ui_media_composite_without_track_menu(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint16_t *pixels, int width, int height, int stride)
{
    psp_ui_media_composite_layers(
        media, preview, pixels, width, height, stride, false);
}

void psp_ui_media_composite(const PspUiMediaState *media, uint16_t *pixels,
                            int width, int height, int stride)
{
    psp_ui_media_composite_with_preview(
        media, NULL, pixels, width, height, stride);
}

size_t psp_ui_media_state_bytes(void)
{
    return sizeof(PspUiMediaState);
}
