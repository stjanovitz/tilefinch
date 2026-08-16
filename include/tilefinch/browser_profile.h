#ifndef TILEFINCH_BROWSER_PROFILE_H
#define TILEFINCH_BROWSER_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/omnibox.h"

#define BROWSER_PROFILE_BOOKMARK_LIMIT 32
#define BROWSER_PROFILE_HISTORY_LIMIT 100
#define BROWSER_PROFILE_RESUME_LIMIT 16
#define BROWSER_PROFILE_READER_SITE_LIMIT 16
#define BROWSER_PROFILE_JAVASCRIPT_SITE_LIMIT 16
#define BROWSER_PROFILE_SECURITY_SITE_LIMIT 16
#define BROWSER_PROFILE_SUGGESTION_LIMIT 4
#define BROWSER_PROFILE_URL_LIMIT 1024
#define BROWSER_PROFILE_TITLE_LIMIT 128
#define BROWSER_PROFILE_TRANSIENT_CACHE_KIB 512u
#define BROWSER_PROFILE_HOMEPAGE_URL \
    "https://tilefinch.local/my-home"
/* At most two background update-metadata checks per week. */
#define BROWSER_PROFILE_UPDATE_CHECK_INTERVAL_SECONDS UINT64_C(302400)

typedef struct {
    char url[BROWSER_PROFILE_URL_LIMIT];
    int scroll_y;
} BrowserRecoveryCheckpoint;

typedef struct {
    char url[BROWSER_PROFILE_URL_LIMIT];
    char title[BROWSER_PROFILE_TITLE_LIMIT];
    uint16_t visits;
} BrowserProfilePage;

typedef struct {
    char video_id[24];
    uint64_t position_us;
    uint64_t duration_us;
} BrowserProfileResume;

typedef struct BrowserProfile BrowserProfile;

typedef struct {
    size_t added;
    size_t duplicate;
    size_t ignored;
    bool resident_full;
    bool truncated;
} BrowserProfileAllowlistImport;

typedef enum {
    BROWSER_COLOR_MODE_AUTO = 0,
    BROWSER_COLOR_MODE_LIGHT,
    BROWSER_COLOR_MODE_DARK
} BrowserColorMode;

typedef enum {
    BROWSER_CHROME_THEME_FINCH = 0,
    BROWSER_CHROME_THEME_OCEAN,
    BROWSER_CHROME_THEME_PLUM,
    BROWSER_CHROME_THEME_EMBER
} BrowserChromeTheme;

typedef enum {
    BROWSER_YOUTUBE_QUALITY_240P = 240,
    BROWSER_YOUTUBE_QUALITY_360P = 360
} BrowserYoutubeQuality;

/*
 * How a decoded video frame is scaled onto the panel.
 *
 * Smooth is bilinear, drawn by the graphics engine, and is the default: it
 * was chosen on measured PSNR/SSIM and side-by-sides of real frames, and it
 * costs the main CPU almost nothing because the GE does the work. Sharp is
 * the software scaler's nearest neighbour -- a genuinely different pipeline,
 * not the same filter with a flag flipped -- kept because some viewers prefer
 * hard pixel edges on low-resolution sources, and because it is what runs
 * whenever the graphics engine is unavailable.
 */
typedef enum {
    BROWSER_VIDEO_SCALING_SMOOTH = 0,
    BROWSER_VIDEO_SCALING_SHARP = 1
} BrowserVideoScaling;

typedef enum {
    BROWSER_TEXT_ENTRY_OSK = 0,
    BROWSER_TEXT_ENTRY_DANZEFF
} BrowserTextEntryMode;

typedef enum {
    BROWSER_READER_FONT_SANS = 0,
    BROWSER_READER_FONT_SERIF
} BrowserReaderFont;

typedef enum {
    BROWSER_UPDATE_CHANNEL_STABLE = 0,
    BROWSER_UPDATE_CHANNEL_BETA,
    BROWSER_UPDATE_CHANNEL_DEVELOPER
} BrowserUpdateChannel;

/* Optional packs never replace the embedded fallback. OFF means the
   zero-install built-in CJK/emoji path and, importantly, performs no font
   component I/O during startup. */
typedef enum {
    BROWSER_GLYPH_LANGUAGE_EMBEDDED = 0,
    BROWSER_GLYPH_LANGUAGE_JAPANESE,
    BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED,
    BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL,
    BROWSER_GLYPH_LANGUAGE_KOREAN
} BrowserGlyphLanguage;

/* Pointers remain owned by the profile and are valid until it is mutated. */
typedef struct {
    const char *url;
    const char *title;
    bool bookmark;
} BrowserProfileSuggestion;

BrowserProfile *browser_profile_create(Budget *budget);
void browser_profile_destroy(BrowserProfile *profile);
bool browser_profile_load(BrowserProfile *profile, const char *path);
bool browser_profile_load_without_content_blocker_sites(
    BrowserProfile *profile, const char *path);
bool browser_profile_save(const BrowserProfile *profile, const char *path);

unsigned browser_profile_ui_scale(const BrowserProfile *profile);
unsigned browser_profile_page_font_percent(const BrowserProfile *profile);
bool browser_profile_history_enabled(const BrowserProfile *profile);
bool browser_profile_restore_last_page(const BrowserProfile *profile);
bool browser_profile_tab_hibernation_enabled(const BrowserProfile *profile);
bool browser_profile_custom_homepage_enabled(
    const BrowserProfile *profile);
bool browser_profile_experimental_voice_input(
    const BrowserProfile *profile);
bool browser_profile_adaptive_voice_memory(const BrowserProfile *profile);
bool browser_profile_analog_cursor_enabled(const BrowserProfile *profile);
unsigned browser_profile_persistent_cache_mb(
    const BrowserProfile *profile);
unsigned browser_profile_live_cache_kib(
    const BrowserProfile *profile);
bool browser_profile_persist_local_storage(
    const BrowserProfile *profile);
/* Cross-boot TLS ticket persistence. Same-process TLS reuse remains a
   transport concern and is not controlled by this preference. */
bool browser_profile_tls_session_persistence(
    const BrowserProfile *profile);
/* Global page policy. Both default to enabled for legacy profiles. */
bool browser_profile_javascript_enabled(const BrowserProfile *profile);
/* The site preference is independent of the global switch. The effective
   policy requires both to be enabled. Internal/non-network URLs are not
   persisted as site exceptions. */
bool browser_profile_site_javascript_enabled(
    const BrowserProfile *profile, const char *url);
bool browser_profile_javascript_allowed_for_url(
    const BrowserProfile *profile, const char *url);
bool browser_profile_site_data_allowed(const BrowserProfile *profile);
bool browser_profile_third_party_cookie_site_allowed(
    const BrowserProfile *profile, const char *url);
/* Cookie notices are hidden by default. A bounded per-site exception lets a
   user restore the page's own consent UI without changing ad blocking. */
bool browser_profile_cookie_banner_hidden(
    const BrowserProfile *profile, const char *url);
BrowserSearchEngine browser_profile_search_engine(
    const BrowserProfile *profile);
BrowserColorMode browser_profile_color_mode(
    const BrowserProfile *profile);
BrowserChromeTheme browser_profile_chrome_theme(
    const BrowserProfile *profile);
BrowserYoutubeQuality browser_profile_youtube_quality(
    const BrowserProfile *profile);
bool browser_profile_youtube_compact_results(
    const BrowserProfile *profile);
BrowserVideoScaling browser_profile_video_scaling(
    const BrowserProfile *profile);
/* Hold the presentation clock briefly at playback start until the existing
   bounded HTTP windows contain a useful prefix. Enabled by default. */
bool browser_profile_video_startup_buffering(
    const BrowserProfile *profile);
bool browser_profile_resume_offline_downloads(
    const BrowserProfile *profile);
BrowserTextEntryMode browser_profile_text_entry_mode(
    const BrowserProfile *profile);
ContentBlockerMode browser_profile_content_blocker_mode(
    const BrowserProfile *profile);
bool browser_profile_content_blocker_cosmetic_hiding(
    const BrowserProfile *profile);
uint64_t browser_profile_content_blocker_total_blocked(
    const BrowserProfile *profile);
BrowserReaderFont browser_profile_reader_font(const BrowserProfile *profile);
bool browser_profile_remember_reader_site_scale(
    const BrowserProfile *profile);
bool browser_profile_update_check_enabled(const BrowserProfile *profile);
BrowserUpdateChannel browser_profile_update_channel(
    const BrowserProfile *profile);
BrowserGlyphLanguage browser_profile_glyph_language(
    const BrowserProfile *profile);
bool browser_profile_color_emoji(const BrowserProfile *profile);
/* Ambient motion on the start surface. Default on. */
bool browser_profile_wave_background(const BrowserProfile *profile);
uint64_t browser_profile_update_check_last_unix(
    const BrowserProfile *profile);
uint64_t browser_profile_update_check_available_sequence(
    const BrowserProfile *profile);
/*
 * The background update-check admission decision. True only when the check
 * is enabled, the wall clock is usable (now_unix_seconds != 0), and the
 * last completed check is unknown, at least
 * BROWSER_PROFILE_UPDATE_CHECK_INTERVAL_SECONDS old, or recorded in the
 * future (a wrong RTC must reset the cadence, never block it forever).
 */
bool browser_profile_update_check_due(
    const BrowserProfile *profile, uint64_t now_unix_seconds);
void browser_profile_set_ui_scale(BrowserProfile *profile, unsigned scale);
void browser_profile_set_page_font_percent(
    BrowserProfile *profile, unsigned percent);
void browser_profile_set_history_enabled(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_restore_last_page(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_tab_hibernation_enabled(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_custom_homepage_enabled(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_experimental_voice_input(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_adaptive_voice_memory(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_analog_cursor_enabled(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_persistent_cache_mb(
    BrowserProfile *profile, unsigned megabytes);
void browser_profile_set_live_cache_kib(
    BrowserProfile *profile, unsigned kibibytes);
void browser_profile_set_persist_local_storage(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_tls_session_persistence(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_javascript_enabled(
    BrowserProfile *profile, bool enabled);
bool browser_profile_set_site_javascript_enabled(
    BrowserProfile *profile, const char *url, bool enabled);
void browser_profile_set_site_data_allowed(
    BrowserProfile *profile, bool allowed);
bool browser_profile_set_third_party_cookie_site_allowed(
    BrowserProfile *profile, const char *url, bool allowed);
bool browser_profile_set_cookie_banner_hidden(
    BrowserProfile *profile, const char *url, bool hidden);
void browser_profile_set_search_engine(
    BrowserProfile *profile, BrowserSearchEngine engine);
void browser_profile_set_color_mode(
    BrowserProfile *profile, BrowserColorMode mode);
void browser_profile_set_chrome_theme(
    BrowserProfile *profile, BrowserChromeTheme theme);
void browser_profile_set_video_scaling(
    BrowserProfile *profile, BrowserVideoScaling scaling);
void browser_profile_set_youtube_quality(
    BrowserProfile *profile, BrowserYoutubeQuality quality);
void browser_profile_set_youtube_compact_results(
    BrowserProfile *profile, bool compact);
void browser_profile_set_video_startup_buffering(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_resume_offline_downloads(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_text_entry_mode(
    BrowserProfile *profile, BrowserTextEntryMode mode);
void browser_profile_set_content_blocker_mode(
    BrowserProfile *profile, ContentBlockerMode mode);
void browser_profile_set_content_blocker_cosmetic_hiding(
    BrowserProfile *profile, bool enabled);
void browser_profile_record_content_blocked(
    BrowserProfile *profile, uint64_t count);
void browser_profile_set_reader_font(
    BrowserProfile *profile, BrowserReaderFont font);
void browser_profile_set_remember_reader_site_scale(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_wave_background(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_update_check_enabled(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_update_channel(
    BrowserProfile *profile, BrowserUpdateChannel channel);
void browser_profile_set_glyph_language(
    BrowserProfile *profile, BrowserGlyphLanguage language);
void browser_profile_set_color_emoji(
    BrowserProfile *profile, bool enabled);
void browser_profile_set_update_check_last_unix(
    BrowserProfile *profile, uint64_t unix_seconds);
void browser_profile_set_update_check_available_sequence(
    BrowserProfile *profile, uint64_t sequence);
bool browser_profile_reader_site_font_percent(
    const BrowserProfile *profile, const char *url, unsigned *percent);
bool browser_profile_record_reader_site_font_percent(
    BrowserProfile *profile, const char *url, unsigned percent);
bool browser_profile_reader_site_always(
    const BrowserProfile *profile, const char *url);
bool browser_profile_set_reader_site_always(
    BrowserProfile *profile, const char *url, bool enabled);
size_t browser_profile_reader_site_count(const BrowserProfile *profile);

bool browser_profile_content_blocker_site_allowed(
    const BrowserProfile *profile, const char *url);
bool browser_profile_set_content_blocker_site_allowed(
    BrowserProfile *profile, const char *url, bool allowed);
size_t browser_profile_content_blocker_allowed_site_count(
    const BrowserProfile *profile);
const char *browser_profile_content_blocker_allowed_site(
    const BrowserProfile *profile, size_t index);
bool browser_profile_import_content_blocker_allowed_sites(
    BrowserProfile *profile, const char *path,
    BrowserProfileAllowlistImport *result);

bool browser_profile_add_bookmark(
    BrowserProfile *profile, const char *url, const char *title);
bool browser_profile_has_bookmark(
    const BrowserProfile *profile, const char *url);
bool browser_profile_remove_bookmark(
    BrowserProfile *profile, const char *url);
size_t browser_profile_bookmark_count(const BrowserProfile *profile);
const BrowserProfilePage *browser_profile_bookmark(
    const BrowserProfile *profile, size_t index);

bool browser_profile_record_history(
    BrowserProfile *profile, const char *url, const char *title);
size_t browser_profile_history_count(const BrowserProfile *profile);
const BrowserProfilePage *browser_profile_history(
    const BrowserProfile *profile, size_t index);
/* Removes one address from history. The file is rewritten whole on save. */
bool browser_profile_forget_history(
    BrowserProfile *profile, const char *url);
size_t browser_profile_suggest(
    const BrowserProfile *profile, const char *query,
    BrowserProfileSuggestion *suggestions, size_t capacity);

bool browser_profile_record_resume(
    BrowserProfile *profile, const char *video_id,
    uint64_t position_us, uint64_t duration_us);
bool browser_profile_resume(
    const BrowserProfile *profile, const char *video_id,
    BrowserProfileResume *resume);

/*
 * A separate, bounded crash-recovery journal. It deliberately retains only
 * an HTTP(S) URL and clamped scroll offset; page, form, DOM, and JS state
 * never enter the file.
 */
bool browser_recovery_save(
    const char *path, const char *url, int scroll_y);
bool browser_recovery_load(
    const char *path, BrowserRecoveryCheckpoint *checkpoint);
bool browser_recovery_clear(const char *path);

bool browser_profile_build_page(
    BrowserProfile *profile, bool history,
    char **html, size_t *length);
bool browser_profile_build_homepage(
    BrowserProfile *profile, char **html, size_t *length);

#endif
