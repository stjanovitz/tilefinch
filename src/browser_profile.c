#include "tilefinch/browser_profile.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PROFILE_HTML_LIMIT (192u * 1024u)
#define PROFILE_ALLOWLIST_FILE_LIMIT (32u * 1024u)
#define PROFILE_ALLOWLIST_LINE_LIMIT 512u
#define PROFILE_FILE_LIMIT (2u * 1024u * 1024u)
#define PROFILE_FNV_OFFSET UINT32_C(2166136261)
#define PROFILE_FNV_PRIME UINT32_C(16777619)

typedef struct {
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    unsigned percent;
    bool always;
} BrowserReaderSiteScale;

struct BrowserProfile {
    Budget *budget;
    unsigned ui_scale;
    unsigned page_font_percent;
    bool history_enabled;
    bool restore_last_page;
    bool tab_hibernation_enabled;
    bool custom_homepage_enabled;
    bool experimental_voice_input;
    bool adaptive_voice_memory;
    bool analog_cursor_enabled;
    unsigned persistent_cache_mb;
    unsigned live_cache_kib;
    bool persist_local_storage;
    bool tls_session_persistence;
    bool javascript_enabled;
    bool site_data_allowed;
    size_t javascript_disabled_site_count;
    char javascript_disabled_sites[BROWSER_PROFILE_JAVASCRIPT_SITE_LIMIT]
                                  [CONTENT_BLOCKER_HOST_LIMIT];
    size_t third_party_cookie_allowed_site_count;
    char third_party_cookie_allowed_sites[BROWSER_PROFILE_SECURITY_SITE_LIMIT]
                                        [CONTENT_BLOCKER_HOST_LIMIT];
    size_t cookie_banner_visible_site_count;
    char cookie_banner_visible_sites[BROWSER_PROFILE_SECURITY_SITE_LIMIT]
                                    [CONTENT_BLOCKER_HOST_LIMIT];
    BrowserSearchEngine search_engine;
    BrowserColorMode color_mode;
    BrowserChromeTheme chrome_theme;
    BrowserYoutubeQuality youtube_quality;
    bool youtube_compact_results;
    bool resume_offline_downloads;
    /* Two-valued, so it is stored as the bool it is and lands in the padding
       the neighbouring bool already leaves. The public type stays an enum
       because callers should not have to know which way the flag points. */
    bool video_scaling_sharp;
    bool video_startup_buffering;
    BrowserTextEntryMode text_entry_mode;
    BrowserReaderFont reader_font;
    bool remember_reader_site_scale;
    bool update_check_enabled;
    BrowserUpdateChannel update_channel;
    BrowserGlyphLanguage glyph_language;
    bool color_emoji;
    bool wave_background;
    uint64_t update_check_last_unix;
    uint64_t update_check_available_sequence;
    size_t reader_site_count;
    BrowserReaderSiteScale reader_sites[BROWSER_PROFILE_READER_SITE_LIMIT];
    ContentBlockerMode content_blocker_mode;
    bool content_blocker_cosmetic_hiding;
    uint64_t content_blocker_total_blocked;
    size_t content_blocker_allowed_site_count;
    char content_blocker_allowed_sites[CONTENT_BLOCKER_ALLOW_SITE_LIMIT]
                                      [CONTENT_BLOCKER_HOST_LIMIT];
    size_t bookmark_count;
    size_t history_count;
    size_t resume_count;
    BrowserProfilePage bookmarks[BROWSER_PROFILE_BOOKMARK_LIMIT];
    BrowserProfilePage history[BROWSER_PROFILE_HISTORY_LIMIT];
    BrowserProfileResume resumes[BROWSER_PROFILE_RESUME_LIMIT];
};

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
} ProfileHtml;

static bool profile_valid_percent(unsigned percent)
{
    return percent == 80u || percent == 100u
        || percent == 125u || percent == 150u;
}

static bool profile_valid_cache_mb(unsigned megabytes)
{
    return megabytes == 0u || megabytes == 1u
        || megabytes == 2u || megabytes == 4u;
}

static bool profile_valid_live_cache_kib(unsigned kibibytes)
{
    return kibibytes == 256u || kibibytes == 512u
        || kibibytes == 1024u || kibibytes == 2048u
        || kibibytes == 4096u;
}

static bool profile_valid_color_mode(BrowserColorMode mode)
{
    return mode >= BROWSER_COLOR_MODE_AUTO
        && mode <= BROWSER_COLOR_MODE_DARK;
}

static bool profile_valid_chrome_theme(BrowserChromeTheme theme)
{
    return theme >= BROWSER_CHROME_THEME_FINCH
        && theme <= BROWSER_CHROME_THEME_EMBER;
}

static bool profile_valid_youtube_quality(BrowserYoutubeQuality quality)
{
    return quality == BROWSER_YOUTUBE_QUALITY_240P
        || quality == BROWSER_YOUTUBE_QUALITY_360P;
}

static bool profile_valid_video_scaling(BrowserVideoScaling scaling)
{
    return scaling == BROWSER_VIDEO_SCALING_SMOOTH
        || scaling == BROWSER_VIDEO_SCALING_SHARP;
}

static bool profile_valid_text_entry_mode(BrowserTextEntryMode mode)
{
    return mode == BROWSER_TEXT_ENTRY_OSK
        || mode == BROWSER_TEXT_ENTRY_DANZEFF;
}

static bool profile_valid_reader_font(BrowserReaderFont font)
{
    return font == BROWSER_READER_FONT_SANS
        || font == BROWSER_READER_FONT_SERIF;
}

static bool profile_valid_content_blocker_mode(ContentBlockerMode mode)
{
    return mode >= CONTENT_BLOCKER_OFF && mode <= CONTENT_BLOCKER_CUSTOM;
}

static bool profile_valid_update_channel(BrowserUpdateChannel channel)
{
    return channel >= BROWSER_UPDATE_CHANNEL_STABLE
        && channel <= BROWSER_UPDATE_CHANNEL_DEVELOPER;
}

static bool profile_valid_glyph_language(BrowserGlyphLanguage language)
{
    return language >= BROWSER_GLYPH_LANGUAGE_EMBEDDED
        && language <= BROWSER_GLYPH_LANGUAGE_KOREAN;
}

static bool profile_valid_block_site(const char *host)
{
    if (host == NULL || host[0] == '\0') return false;
    char url[CONTENT_BLOCKER_HOST_LIMIT + 16u];
    char normalized[CONTENT_BLOCKER_HOST_LIMIT];
    int length = snprintf(url, sizeof(url), "https://%s/", host);
    return length > 0 && (size_t) length < sizeof(url)
        && content_blocker_site_from_url(url, normalized)
        && strcmp(host, normalized) == 0;
}

static bool profile_javascript_site_from_url(
    const char *url, char site[CONTENT_BLOCKER_HOST_LIMIT])
{
    return content_blocker_site_from_url(url, site)
        && strcmp(site, "tilefinch.local") != 0;
}

static bool recovery_url_supported(const char *url)
{
    return url != NULL
        && (strncmp(url, "https://", 8) == 0
            || strncmp(url, "http://", 7) == 0)
        && strlen(url) < BROWSER_PROFILE_URL_LIMIT;
}

static void profile_copy(char *output, size_t capacity, const char *input)
{
    if (output == NULL || capacity == 0) return;
    snprintf(output, capacity, "%s", input == NULL ? "" : input);
}

BrowserProfile *browser_profile_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    BrowserProfile *profile = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*profile));
    if (profile == NULL) return NULL;
    profile->budget = budget;
    profile->ui_scale = 1;
    profile->page_font_percent = 100;
    profile->analog_cursor_enabled = true;
    profile->live_cache_kib = BROWSER_PROFILE_TRANSIENT_CACHE_KIB;
    profile->youtube_quality = BROWSER_YOUTUBE_QUALITY_360P;
    profile->video_startup_buffering = true;
    profile->content_blocker_mode = CONTENT_BLOCKER_BASIC;
    profile->content_blocker_cosmetic_hiding = true;
    profile->update_check_enabled = true;
    profile->tls_session_persistence = true;
    profile->wave_background = true;
    profile->javascript_enabled = true;
    profile->site_data_allowed = true;
    return profile;
}

void browser_profile_destroy(BrowserProfile *profile)
{
    if (profile == NULL) return;
    Budget *budget = profile->budget;
    memset(profile, 0, sizeof(*profile));
    budget_free(budget, profile);
}

static bool profile_encode(const char *input, char *output, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *at = (const unsigned char *)
             (input == NULL ? "" : input); *at != '\0'; at++) {
        if (isalnum(*at) || *at == '-' || *at == '_' || *at == '.'
            || *at == '~' || *at == '/' || *at == ':') {
            if (used >= capacity - 1u) return false;
            output[used++] = (char) *at;
        } else {
            if (used > capacity - 4u) return false;
            output[used++] = '%';
            output[used++] = hex[*at >> 4];
            output[used++] = hex[*at & 15u];
        }
    }
    output[used] = '\0';
    return true;
}

static int profile_hex(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool profile_decode(const char *input, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t at = 0; input != NULL && input[at] != '\0'; at++) {
        unsigned char byte = (unsigned char) input[at];
        if (byte == '%' && input[at + 1] != '\0'
            && input[at + 2] != '\0') {
            int high = profile_hex(input[at + 1]);
            int low = profile_hex(input[at + 2]);
            if (high < 0 || low < 0) return false;
            byte = (unsigned char) ((high << 4) | low);
            at += 2;
        }
        if (byte == '\0' || used >= capacity - 1u) return false;
        output[used++] = (char) byte;
    }
    output[used] = '\0';
    return true;
}

static void profile_page_move_front(
    BrowserProfilePage *pages, size_t *count, size_t limit,
    const char *url, const char *title)
{
    size_t found = *count;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(pages[i].url, url) == 0) {
            found = i;
            break;
        }
    }
    if (found < *count) {
        BrowserProfilePage existing = pages[found];
        if (existing.visits != UINT16_MAX) existing.visits++;
        memmove(&pages[1], &pages[0], found * sizeof(*pages));
        pages[0] = existing;
    } else {
        size_t move = *count < limit ? *count : limit - 1u;
        memmove(&pages[1], &pages[0], move * sizeof(*pages));
        if (*count < limit) (*count)++;
        memset(&pages[0], 0, sizeof(*pages));
        pages[0].visits = 1;
    }
    profile_copy(pages[0].url, sizeof(pages[0].url), url);
    profile_copy(
        pages[0].title, sizeof(pages[0].title),
        title == NULL || title[0] == '\0' ? url : title);
}

bool browser_profile_add_bookmark(
    BrowserProfile *profile, const char *url, const char *title)
{
    if (profile == NULL || url == NULL || url[0] == '\0'
        || strlen(url) >= BROWSER_PROFILE_URL_LIMIT) return false;
    profile_page_move_front(
        profile->bookmarks, &profile->bookmark_count,
        BROWSER_PROFILE_BOOKMARK_LIMIT, url, title);
    return true;
}

bool browser_profile_has_bookmark(
    const BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL || url[0] == '\0') return false;
    for (size_t i = 0; i < profile->bookmark_count; i++) {
        if (strcmp(profile->bookmarks[i].url, url) == 0) return true;
    }
    return false;
}

bool browser_profile_remove_bookmark(
    BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL || url[0] == '\0') return false;
    for (size_t i = 0; i < profile->bookmark_count; i++) {
        if (strcmp(profile->bookmarks[i].url, url) != 0) continue;
        size_t remaining = profile->bookmark_count - i - 1u;
        if (remaining != 0) {
            memmove(
                &profile->bookmarks[i], &profile->bookmarks[i + 1u],
                remaining * sizeof(profile->bookmarks[0]));
        }
        profile->bookmark_count--;
        memset(
            &profile->bookmarks[profile->bookmark_count], 0,
            sizeof(profile->bookmarks[0]));
        return true;
    }
    return false;
}

bool browser_profile_record_history(
    BrowserProfile *profile, const char *url, const char *title)
{
    if (profile == NULL || !profile->history_enabled
        || url == NULL || url[0] == '\0'
        || strlen(url) >= BROWSER_PROFILE_URL_LIMIT) return false;
    profile_page_move_front(
        profile->history, &profile->history_count,
        BROWSER_PROFILE_HISTORY_LIMIT, url, title);
    return true;
}

static bool profile_write_page(FILE *file, char kind,
                               const BrowserProfilePage *page)
{
    char url[BROWSER_PROFILE_URL_LIMIT * 3u];
    char title[BROWSER_PROFILE_TITLE_LIMIT * 3u];
    return profile_encode(page->url, url, sizeof(url))
        && profile_encode(page->title, title, sizeof(title))
        && fprintf(file, "%c\t%s\t%s\t%u\n", kind, url, title,
                   (unsigned) page->visits) > 0;
}

static uint32_t profile_checksum_update(
    uint32_t checksum, const unsigned char *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
        checksum *= PROFILE_FNV_PRIME;
    }
    return checksum;
}

static bool profile_checksum_file(
    const char *path, uint32_t *checksum, size_t *length)
{
    if (path == NULL || checksum == NULL || length == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char buffer[1024];
    uint32_t value = PROFILE_FNV_OFFSET;
    size_t total = 0;
    bool ok = true;
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), file);
        if (got != 0) {
            if (got > PROFILE_FILE_LIMIT - total) {
                ok = false;
                break;
            }
            value = profile_checksum_update(value, buffer, got);
            total += got;
        }
        if (got != sizeof(buffer)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    *checksum = value;
    *length = total;
    return true;
}

static bool profile_install_temporary(
    const char *temporary, const char *path, const char *backup)
{
    /*
     * Rotate the primary through backup before installing the closed
     * temporary file, so that every instant of the publish has at least one
     * complete generation on the stick. POSIX replaces an older backup
     * atomically. FAT refuses that replacement, in which case remove only the
     * older backup while the primary is still intact and retry.
     */
    bool had_previous = rename(path, backup) == 0;
    if (!had_previous) {
        if (errno == ENOENT) return rename(temporary, path) == 0;
        (void) remove(backup);
        had_previous = rename(path, backup) == 0;
        if (!had_previous && errno != ENOENT) return false;
    }
    if (rename(temporary, path) == 0) return true;
    if (had_previous) (void) rename(backup, path);
    return false;
}

bool browser_profile_save(const BrowserProfile *profile, const char *path)
{
    if (profile == NULL || path == NULL || path[0] == '\0') return false;
    char temporary[1200], backup[1200];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t) length >= sizeof(temporary)) return false;
    length = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (length < 0 || (size_t) length >= sizeof(backup)) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    /*
     * Compatibility contract: record kinds and existing field positions are
     * stable; new scalar fields are appended only. Readers accept missing
     * trailing fields and ignore unknown trailing fields/record kinds. This
     * lets an older A/B slot read a profile last written by a newer slot
     * after rollback without rejecting or shifting established settings.
     */
    bool ok = fprintf(
        file,
        "TILEFINCH_PROFILE\t1\n"
        "UI\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
        profile->ui_scale, profile->page_font_percent,
        profile->history_enabled ? 1u : 0u,
        (unsigned) profile->search_engine,
        profile->restore_last_page ? 1u : 0u,
        profile->adaptive_voice_memory ? 1u : 0u,
        profile->analog_cursor_enabled ? 1u : 0u,
        (unsigned) profile->color_mode,
        profile->custom_homepage_enabled ? 1u : 0u,
        profile->experimental_voice_input ? 1u : 0u,
        (unsigned) profile->text_entry_mode) > 0;
    if (ok) {
        ok = fprintf(
            file, "THEME\t%u\n",
            (unsigned) profile->chrome_theme) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "DATA\t%u\t%u\t%u\n",
            profile->persistent_cache_mb,
            profile->persist_local_storage ? 1u : 0u,
            profile->live_cache_kib) > 0;
    }
    if (ok) {
        /* Fields are append-only: old binaries ignore a suffix and newer
           binaries supply defaults for a missing suffix. */
        ok = fprintf(
            file, "MEDIA\t%u\t%u\t%u\t%u\t%u\n",
            (unsigned) profile->youtube_quality,
            profile->resume_offline_downloads ? 1u : 0u,
            profile->video_scaling_sharp ? 1u : 0u,
            profile->video_startup_buffering ? 1u : 0u,
            profile->youtube_compact_results ? 1u : 0u) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "BLOCK\t%u\t%u\n",
            (unsigned) profile->content_blocker_mode,
            profile->content_blocker_cosmetic_hiding ? 1u : 0u) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "STATS\t%llu\n",
            (unsigned long long)
                profile->content_blocker_total_blocked) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "TABS\t%u\n",
            profile->tab_hibernation_enabled ? 1u : 0u) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "UPDCHK\t%u\t%llu\t%llu\n",
            profile->update_check_enabled ? 1u : 0u,
            (unsigned long long) profile->update_check_last_unix,
            (unsigned long long)
                profile->update_check_available_sequence) > 0;
    }
    if (ok) {
        /* Append-only: old builds ignore the channel and stay stable. */
        ok = fprintf(file, "UPDCHAN\t%u\n",
                     (unsigned) profile->update_channel) > 0;
    }
    if (ok) {
        /* Optional installed components are selected independently of their
           on-disk presence. Missing packs degrade to the embedded fallback. */
        ok = fprintf(file, "GLYPHS\t%u\t%u\n",
                     (unsigned) profile->glyph_language,
                     profile->color_emoji ? 1u : 0u) > 0;
    }
    if (ok) {
        /* Appended after UPDCHK: an older build ignores the unknown key and
           a newer build reading an older file keeps the default. */
        ok = fprintf(
            file, "WAVE\t%u\n",
            profile->wave_background ? 1u : 0u) > 0;
    }
    if (ok) {
        /* Independent global page policies. This is a separate append-only
           record so older builds safely ignore it. */
        ok = fprintf(
            file, "POLICY\t%u\t%u\n",
            profile->javascript_enabled ? 1u : 0u,
            profile->site_data_allowed ? 1u : 0u) > 0;
    }
    if (ok) {
        /* Cross-boot transport persistence is independent of page storage.
           Older builds ignore this record and retain the historical default. */
        ok = fprintf(
            file, "TLSSESS\t%u\n",
            profile->tls_session_persistence ? 1u : 0u) > 0;
    }
    for (size_t i = 0;
         ok && i < profile->javascript_disabled_site_count; i++) {
        char encoded[CONTENT_BLOCKER_HOST_LIMIT * 3u];
        ok = profile_encode(
                 profile->javascript_disabled_sites[i], encoded,
                 sizeof(encoded))
            && fprintf(file, "JSD\t%s\n", encoded) > 0;
    }
    for (size_t i = 0;
         ok && i < profile->third_party_cookie_allowed_site_count; i++) {
        char encoded[CONTENT_BLOCKER_HOST_LIMIT * 3u];
        ok = profile_encode(
                 profile->third_party_cookie_allowed_sites[i], encoded,
                 sizeof(encoded))
            && fprintf(file, "TPC\t%s\n", encoded) > 0;
    }
    for (size_t i = 0;
         ok && i < profile->cookie_banner_visible_site_count; i++) {
        char encoded[CONTENT_BLOCKER_HOST_LIMIT * 3u];
        ok = profile_encode(
                 profile->cookie_banner_visible_sites[i], encoded,
                 sizeof(encoded))
            && fprintf(file, "CBV\t%s\n", encoded) > 0;
    }
    if (ok) {
        ok = fprintf(
            file, "READER\t%u\t%u\n",
            (unsigned) profile->reader_font,
            profile->remember_reader_site_scale ? 1u : 0u) > 0;
    }
    for (size_t i = 0;
         ok && i < profile->reader_site_count; i++) {
        char encoded[CONTENT_BLOCKER_HOST_LIMIT * 3u];
        ok = profile_encode(
                 profile->reader_sites[i].site, encoded, sizeof(encoded))
            && fprintf(
                   file, "RS\t%s\t%u\t%u\n", encoded,
                   profile->reader_sites[i].percent,
                   profile->reader_sites[i].always ? 1u : 0u) > 0;
    }
    for (size_t i = 0;
         ok && i < profile->content_blocker_allowed_site_count; i++) {
        char encoded[CONTENT_BLOCKER_HOST_LIMIT * 3u];
        ok = profile_encode(
                 profile->content_blocker_allowed_sites[i], encoded,
                 sizeof(encoded))
            && fprintf(file, "A\t%s\n", encoded) > 0;
    }
    for (size_t i = 0; ok && i < profile->bookmark_count; i++)
        ok = profile_write_page(file, 'B', &profile->bookmarks[i]);
    for (size_t i = 0; ok && i < profile->history_count; i++)
        ok = profile_write_page(file, 'H', &profile->history[i]);
    for (size_t i = 0; ok && i < profile->resume_count; i++)
        ok = fprintf(
            file, "R\t%s\t%llu\t%llu\n", profile->resumes[i].video_id,
            (unsigned long long) profile->resumes[i].position_us,
            (unsigned long long) profile->resumes[i].duration_us) > 0;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove(temporary);
        return false;
    }
    uint32_t checksum = 0;
    size_t body_length = 0;
    if (!profile_checksum_file(temporary, &checksum, &body_length)) {
        remove(temporary);
        return false;
    }
    file = fopen(temporary, "ab");
    if (file == NULL) {
        remove(temporary);
        return false;
    }
    ok = fprintf(file, "END\t%zu\t%08X\n", body_length,
                 (unsigned) checksum) > 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    if (profile_install_temporary(temporary, path, backup)) return true;
    remove(temporary);
    return false;
}

static bool profile_load_internal(
    BrowserProfile *profile, const char *path, bool load_allowed_sites)
{
    if (profile == NULL || path == NULL || path[0] == '\0') return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    BrowserProfile *loaded = budget_calloc_category(
        profile->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*loaded));
    if (loaded == NULL) {
        fclose(file);
        return false;
    }
    *loaded = (BrowserProfile) {
        .budget = profile->budget,
        .ui_scale = 1,
        .page_font_percent = 100,
        .analog_cursor_enabled = true,
        .live_cache_kib = BROWSER_PROFILE_TRANSIENT_CACHE_KIB,
        .youtube_quality = BROWSER_YOUTUBE_QUALITY_360P,
        .video_startup_buffering = true,
        .content_blocker_mode = CONTENT_BLOCKER_BASIC,
        .content_blocker_cosmetic_hiding = true,
        .update_check_enabled = true,
        .tls_session_persistence = true,
        .wave_background = true,
        .javascript_enabled = true,
        .site_data_allowed = true
    };
    char line[4096];
    bool header = false;
    bool complete = true;
    bool footer_seen = false;
    uint32_t checksum = PROFILE_FNV_OFFSET;
    size_t checksum_length = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t raw_length = strlen(line);
        bool terminated = raw_length != 0
            && (line[raw_length - 1u] == '\n'
                || line[raw_length - 1u] == '\r');
        if (!terminated) {
            /* Saves terminate every record. A full buffer or unterminated EOF
               is therefore a torn generation, not a shorter valid profile;
               accepting it would silently skip the intact backup. */
            complete = false;
            break;
        }
        if (footer_seen) {
            complete = false;
            break;
        }
        if (strncmp(line, "END\t", 4) == 0) {
            char footer[64];
            if (raw_length >= sizeof(footer)) {
                complete = false;
                break;
            }
            memcpy(footer, line, raw_length + 1u);
            footer[strcspn(footer, "\r\n")] = '\0';
            size_t expected_length = 0;
            unsigned expected_checksum = 0;
            char extra = '\0';
            if (sscanf(footer, "END\t%zu\t%8X%c", &expected_length,
                       &expected_checksum, &extra) != 2
                || expected_length != checksum_length
                || expected_checksum != checksum) {
                complete = false;
                break;
            }
            footer_seen = true;
            continue;
        }
        if (raw_length > PROFILE_FILE_LIMIT - checksum_length) {
            complete = false;
            break;
        }
        checksum = profile_checksum_update(
            checksum, (const unsigned char *) line, raw_length);
        checksum_length += raw_length;
        line[strcspn(line, "\r\n")] = '\0';
        if (!header) {
            header = strcmp(line, "TILEFINCH_PROFILE\t1") == 0;
            if (!header) break;
            continue;
        }
        char *first = strchr(line, '\t');
        if (first == NULL) continue;
        *first++ = '\0';
        char *second = strchr(first, '\t');
        if (second != NULL) *second++ = '\0';
        if (strcmp(line, "UI") == 0 && second != NULL) {
            char *third = strchr(second, '\t');
            if (third == NULL) continue;
            *third++ = '\0';
            char *fourth = strchr(third, '\t');
            if (fourth != NULL) *fourth++ = '\0';
            char *fifth = fourth == NULL ? NULL : strchr(fourth, '\t');
            if (fifth != NULL) *fifth++ = '\0';
            char *sixth = fifth == NULL ? NULL : strchr(fifth, '\t');
            if (sixth != NULL) *sixth++ = '\0';
            char *seventh = sixth == NULL ? NULL : strchr(sixth, '\t');
            if (seventh != NULL) *seventh++ = '\0';
            char *eighth =
                seventh == NULL ? NULL : strchr(seventh, '\t');
            if (eighth != NULL) *eighth++ = '\0';
            char *ninth =
                eighth == NULL ? NULL : strchr(eighth, '\t');
            if (ninth != NULL) *ninth++ = '\0';
            char *tenth =
                ninth == NULL ? NULL : strchr(ninth, '\t');
            if (tenth != NULL) *tenth++ = '\0';
            char *eleventh =
                tenth == NULL ? NULL : strchr(tenth, '\t');
            if (eleventh != NULL) *eleventh++ = '\0';
            unsigned scale = (unsigned) strtoul(first, NULL, 10);
            unsigned font = (unsigned) strtoul(second, NULL, 10);
            loaded->ui_scale = scale >= 2u ? 2u : 1u;
            loaded->page_font_percent =
                profile_valid_percent(font) ? font : 100u;
            loaded->history_enabled =
                strtoul(third, NULL, 10) != 0;
            if (fourth != NULL) {
                BrowserSearchEngine engine =
                    (BrowserSearchEngine) strtoul(fourth, NULL, 10);
                if (browser_search_engine_valid(engine))
                    loaded->search_engine = engine;
            }
            if (fifth != NULL)
                loaded->restore_last_page =
                    strtoul(fifth, NULL, 10) != 0;
            if (sixth != NULL)
                loaded->adaptive_voice_memory =
                    strtoul(sixth, NULL, 10) != 0;
            if (seventh != NULL)
                loaded->analog_cursor_enabled =
                    strtoul(seventh, NULL, 10) != 0;
            if (eighth != NULL) {
                BrowserColorMode mode =
                    (BrowserColorMode) strtoul(eighth, NULL, 10);
                if (profile_valid_color_mode(mode))
                    loaded->color_mode = mode;
            }
            if (ninth != NULL)
                loaded->custom_homepage_enabled =
                    strtoul(ninth, NULL, 10) != 0;
            if (tenth != NULL)
                loaded->experimental_voice_input =
                    strtoul(tenth, NULL, 10) != 0;
            if (eleventh != NULL) {
                BrowserTextEntryMode mode =
                    (BrowserTextEntryMode) strtoul(eleventh, NULL, 10);
                if (profile_valid_text_entry_mode(mode))
                    loaded->text_entry_mode = mode;
            }
        } else if (strcmp(line, "THEME") == 0) {
            BrowserChromeTheme theme =
                (BrowserChromeTheme) strtoul(first, NULL, 10);
            if (profile_valid_chrome_theme(theme))
                loaded->chrome_theme = theme;
        } else if (strcmp(line, "DATA") == 0 && second != NULL) {
            char *third = strchr(second, '\t');
            if (third != NULL) *third++ = '\0';
            unsigned cache_mb = (unsigned) strtoul(first, NULL, 10);
            loaded->persistent_cache_mb =
                profile_valid_cache_mb(cache_mb) ? cache_mb : 0u;
            loaded->persist_local_storage =
                strtoul(second, NULL, 10) != 0;
            if (third != NULL) {
                unsigned live_kib = (unsigned) strtoul(third, NULL, 10);
                if (profile_valid_live_cache_kib(live_kib))
                    loaded->live_cache_kib = live_kib;
            }
        } else if (strcmp(line, "MEDIA") == 0) {
            char *third = second == NULL ? NULL : strchr(second, '\t');
            if (third != NULL) *third++ = '\0';
            char *fourth = third == NULL ? NULL : strchr(third, '\t');
            if (fourth != NULL) *fourth++ = '\0';
            char *fifth = fourth == NULL ? NULL : strchr(fourth, '\t');
            if (fifth != NULL) *fifth++ = '\0';
            BrowserYoutubeQuality quality =
                (BrowserYoutubeQuality) strtoul(first, NULL, 10);
            if (profile_valid_youtube_quality(quality))
                loaded->youtube_quality = quality;
            if (second != NULL)
                loaded->resume_offline_downloads =
                    strtoul(second, NULL, 10) != 0;
            if (third != NULL) {
                BrowserVideoScaling scaling =
                    (BrowserVideoScaling) strtoul(third, NULL, 10);
                if (profile_valid_video_scaling(scaling))
                    loaded->video_scaling_sharp =
                        scaling == BROWSER_VIDEO_SCALING_SHARP;
            }
            if (fourth != NULL)
                loaded->video_startup_buffering =
                    strtoul(fourth, NULL, 10) != 0;
            if (fifth != NULL)
                loaded->youtube_compact_results =
                    strtoul(fifth, NULL, 10) != 0;
        } else if (strcmp(line, "BLOCK") == 0) {
            ContentBlockerMode mode =
                (ContentBlockerMode) strtoul(first, NULL, 10);
            if (profile_valid_content_blocker_mode(mode))
                loaded->content_blocker_mode = mode;
            if (second != NULL)
                loaded->content_blocker_cosmetic_hiding =
                    strtoul(second, NULL, 10) != 0;
        } else if (strcmp(line, "STATS") == 0) {
            loaded->content_blocker_total_blocked =
                strtoull(first, NULL, 10);
        } else if (strcmp(line, "TABS") == 0) {
            loaded->tab_hibernation_enabled =
                strtoul(first, NULL, 10) != 0;
        } else if (strcmp(line, "WAVE") == 0) {
            loaded->wave_background = strtoul(first, NULL, 10) != 0;
        } else if (strcmp(line, "POLICY") == 0 && second != NULL) {
            loaded->javascript_enabled = strtoul(first, NULL, 10) != 0;
            loaded->site_data_allowed = strtoul(second, NULL, 10) != 0;
        } else if (strcmp(line, "TLSSESS") == 0) {
            loaded->tls_session_persistence =
                strtoul(first, NULL, 10) != 0;
        } else if (strcmp(line, "JSD") == 0
                   && loaded->javascript_disabled_site_count
                          < BROWSER_PROFILE_JAVASCRIPT_SITE_LIMIT) {
            char *site = loaded->javascript_disabled_sites[
                loaded->javascript_disabled_site_count];
            if (profile_decode(first, site, CONTENT_BLOCKER_HOST_LIMIT)
                && profile_valid_block_site(site)) {
                bool duplicate = false;
                for (size_t i = 0;
                     i < loaded->javascript_disabled_site_count; i++) {
                    if (strcmp(
                            loaded->javascript_disabled_sites[i], site)
                        == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) loaded->javascript_disabled_site_count++;
            }
        } else if (strcmp(line, "MIXED") == 0) {
            /* Legacy releases persisted this compatibility grant. It is now
               session-only, so accepting an old record must not silently
               restore a durable HTTPS downgrade exception. */
            continue;
        } else if (strcmp(line, "TPC") == 0) {
            char (*sites)[CONTENT_BLOCKER_HOST_LIMIT] =
                loaded->third_party_cookie_allowed_sites;
            size_t *count = &loaded->third_party_cookie_allowed_site_count;
            if (*count < BROWSER_PROFILE_SECURITY_SITE_LIMIT) {
                char site[CONTENT_BLOCKER_HOST_LIMIT];
                if (profile_decode(first, site, sizeof(site))
                    && profile_valid_block_site(site)) {
                    bool duplicate = false;
                    for (size_t i = 0; i < *count; i++)
                        if (strcmp(sites[i], site) == 0) duplicate = true;
                    if (!duplicate)
                        snprintf(sites[(*count)++], sizeof(sites[0]),
                                 "%s", site);
                }
            }
        } else if (strcmp(line, "CBV") == 0) {
            char (*sites)[CONTENT_BLOCKER_HOST_LIMIT] =
                loaded->cookie_banner_visible_sites;
            size_t *count = &loaded->cookie_banner_visible_site_count;
            if (*count < BROWSER_PROFILE_SECURITY_SITE_LIMIT) {
                char site[CONTENT_BLOCKER_HOST_LIMIT];
                if (profile_decode(first, site, sizeof(site))
                    && profile_valid_block_site(site)) {
                    bool duplicate = false;
                    for (size_t i = 0; i < *count; i++)
                        if (strcmp(sites[i], site) == 0) duplicate = true;
                    if (!duplicate)
                        snprintf(sites[(*count)++], sizeof(sites[0]),
                                 "%s", site);
                }
            }
        } else if (strcmp(line, "UPDCHK") == 0) {
            loaded->update_check_enabled =
                strtoul(first, NULL, 10) != 0;
            if (second != NULL) {
                char *third = strchr(second, '\t');
                if (third != NULL) *third++ = '\0';
                loaded->update_check_last_unix =
                    strtoull(second, NULL, 10);
                if (third != NULL)
                    loaded->update_check_available_sequence =
                        strtoull(third, NULL, 10);
            }
        } else if (strcmp(line, "UPDCHAN") == 0) {
            BrowserUpdateChannel channel =
                (BrowserUpdateChannel) strtoul(first, NULL, 10);
            if (profile_valid_update_channel(channel))
                loaded->update_channel = channel;
        } else if (strcmp(line, "GLYPHS") == 0) {
            BrowserGlyphLanguage language =
                (BrowserGlyphLanguage) strtoul(first, NULL, 10);
            if (profile_valid_glyph_language(language))
                loaded->glyph_language = language;
            if (second != NULL)
                loaded->color_emoji = strtoul(second, NULL, 10) != 0;
        } else if (strcmp(line, "READER") == 0 && second != NULL) {
            BrowserReaderFont font =
                (BrowserReaderFont) strtoul(first, NULL, 10);
            if (profile_valid_reader_font(font)) loaded->reader_font = font;
            loaded->remember_reader_site_scale =
                strtoul(second, NULL, 10) != 0;
            if (!loaded->remember_reader_site_scale) {
                loaded->reader_site_count = 0;
                memset(loaded->reader_sites, 0, sizeof(loaded->reader_sites));
            }
        } else if (strcmp(line, "RS") == 0 && second != NULL
                   && loaded->reader_site_count
                          < BROWSER_PROFILE_READER_SITE_LIMIT) {
            BrowserReaderSiteScale *scale =
                &loaded->reader_sites[loaded->reader_site_count];
            char *always = strchr(second, '\t');
            if (always != NULL) *always++ = '\0';
            unsigned percent = (unsigned) strtoul(second, NULL, 10);
            if (profile_decode(first, scale->site, sizeof(scale->site))
                && profile_valid_block_site(scale->site)
                && profile_valid_percent(percent)) {
                bool duplicate = false;
                for (size_t i = 0; i < loaded->reader_site_count; i++) {
                    if (strcmp(loaded->reader_sites[i].site, scale->site) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    scale->percent = percent;
                    scale->always = always != NULL
                        && strtoul(always, NULL, 10) != 0;
                    if (!loaded->remember_reader_site_scale
                        && !scale->always) continue;
                    loaded->reader_site_count++;
                }
            }
        } else if (load_allowed_sites && strcmp(line, "A") == 0
                   && loaded->content_blocker_allowed_site_count
                          < CONTENT_BLOCKER_ALLOW_SITE_LIMIT) {
            char *site = loaded->content_blocker_allowed_sites[
                loaded->content_blocker_allowed_site_count];
            if (profile_decode(first, site, CONTENT_BLOCKER_HOST_LIMIT)
                && profile_valid_block_site(site)) {
                bool duplicate = false;
                for (size_t i = 0;
                     i < loaded->content_blocker_allowed_site_count; i++) {
                    if (strcmp(
                            loaded->content_blocker_allowed_sites[i], site)
                            == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    loaded->content_blocker_allowed_site_count++;
            }
        } else if ((strcmp(line, "B") == 0
                    || strcmp(line, "H") == 0)
                   && second != NULL) {
            BrowserProfilePage page = {0};
            char *visits = strchr(second, '\t');
            if (visits != NULL) *visits++ = '\0';
            if (!profile_decode(
                    first, page.url, sizeof(page.url))
                || !profile_decode(
                    second, page.title, sizeof(page.title)))
                continue;
            unsigned parsed_visits = visits == NULL
                ? 1u : (unsigned) strtoul(visits, NULL, 10);
            page.visits = (uint16_t) (parsed_visits == 0 ? 1u
                : (parsed_visits > UINT16_MAX ? UINT16_MAX
                                              : parsed_visits));
            if (line[0] == 'B'
                && loaded->bookmark_count
                       < BROWSER_PROFILE_BOOKMARK_LIMIT) {
                loaded->bookmarks[loaded->bookmark_count++] = page;
            } else if (line[0] == 'H'
                       && loaded->history_count
                              < BROWSER_PROFILE_HISTORY_LIMIT) {
                loaded->history[loaded->history_count++] = page;
            }
        } else if (strcmp(line, "R") == 0 && second != NULL
                   && loaded->resume_count
                          < BROWSER_PROFILE_RESUME_LIMIT) {
            char *third = strchr(second, '\t');
            if (third == NULL) continue;
            *third++ = '\0';
            BrowserProfileResume *resume =
                &loaded->resumes[loaded->resume_count];
            profile_copy(
                resume->video_id, sizeof(resume->video_id), first);
            resume->position_us = strtoull(second, NULL, 10);
            resume->duration_us = strtoull(third, NULL, 10);
            loaded->resume_count++;
        }
    }
    bool read_ok = !ferror(file);
    bool ok = fclose(file) == 0 && header && complete && footer_seen
        && read_ok;
    if (ok) *profile = *loaded;
    budget_free(profile->budget, loaded);
    return ok;
}

static bool profile_load_with_backup(
    BrowserProfile *profile, const char *path, bool load_allowed_sites)
{
    if (profile == NULL || path == NULL || path[0] == '\0') return false;
    if (profile_load_internal(profile, path, load_allowed_sites)) return true;
    char backup[1200];
    int length = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (length < 0 || (size_t) length >= sizeof(backup)) return false;
    if (!profile_load_internal(profile, backup, load_allowed_sites))
        return false;
    /*
     * The primary is missing or unreadable and the backup carried the
     * settings. Drop a primary that exists but does not parse so the next
     * save's rotation cannot promote it over the recovered generation.
     */
    (void) remove(path);
    return true;
}

bool browser_profile_load(BrowserProfile *profile, const char *path)
{
    return profile_load_with_backup(profile, path, true);
}

bool browser_profile_load_without_content_blocker_sites(
    BrowserProfile *profile, const char *path)
{
    return profile_load_with_backup(profile, path, false);
}

unsigned browser_profile_ui_scale(const BrowserProfile *profile)
{
    return profile == NULL ? 1u : profile->ui_scale;
}

unsigned browser_profile_page_font_percent(const BrowserProfile *profile)
{
    return profile == NULL ? 100u : profile->page_font_percent;
}

bool browser_profile_history_enabled(const BrowserProfile *profile)
{
    return profile != NULL && profile->history_enabled;
}

bool browser_profile_restore_last_page(const BrowserProfile *profile)
{
    return profile != NULL && profile->restore_last_page;
}

bool browser_profile_tab_hibernation_enabled(const BrowserProfile *profile)
{
    return profile != NULL && profile->tab_hibernation_enabled;
}

bool browser_profile_custom_homepage_enabled(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->custom_homepage_enabled;
}

bool browser_profile_adaptive_voice_memory(const BrowserProfile *profile)
{
    return profile != NULL && profile->adaptive_voice_memory;
}

bool browser_profile_experimental_voice_input(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->experimental_voice_input;
}

bool browser_profile_analog_cursor_enabled(const BrowserProfile *profile)
{
    return profile == NULL || profile->analog_cursor_enabled;
}

unsigned browser_profile_persistent_cache_mb(
    const BrowserProfile *profile)
{
    return profile == NULL ? 0u : profile->persistent_cache_mb;
}

unsigned browser_profile_live_cache_kib(
    const BrowserProfile *profile)
{
    return profile == NULL || profile->live_cache_kib == 0
        ? BROWSER_PROFILE_TRANSIENT_CACHE_KIB : profile->live_cache_kib;
}

bool browser_profile_persist_local_storage(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->persist_local_storage;
}

bool browser_profile_tls_session_persistence(
    const BrowserProfile *profile)
{
    return profile == NULL || profile->tls_session_persistence;
}

bool browser_profile_javascript_enabled(const BrowserProfile *profile)
{
    return profile == NULL || profile->javascript_enabled;
}

bool browser_profile_site_javascript_enabled(
    const BrowserProfile *profile, const char *url)
{
    if (profile == NULL) return true;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!profile_javascript_site_from_url(url, site)) return true;
    for (size_t i = 0; i < profile->javascript_disabled_site_count; i++) {
        if (strcmp(profile->javascript_disabled_sites[i], site) == 0)
            return false;
    }
    return true;
}

bool browser_profile_javascript_allowed_for_url(
    const BrowserProfile *profile, const char *url)
{
    return browser_profile_javascript_enabled(profile)
        && browser_profile_site_javascript_enabled(profile, url);
}

bool browser_profile_site_data_allowed(const BrowserProfile *profile)
{
    return profile == NULL || profile->site_data_allowed;
}

BrowserSearchEngine browser_profile_search_engine(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_SEARCH_GOOGLE : profile->search_engine;
}

BrowserColorMode browser_profile_color_mode(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_COLOR_MODE_AUTO : profile->color_mode;
}

BrowserChromeTheme browser_profile_chrome_theme(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_CHROME_THEME_FINCH : profile->chrome_theme;
}

BrowserYoutubeQuality browser_profile_youtube_quality(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_YOUTUBE_QUALITY_360P : profile->youtube_quality;
}

bool browser_profile_youtube_compact_results(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->youtube_compact_results;
}

BrowserVideoScaling browser_profile_video_scaling(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->video_scaling_sharp
        ? BROWSER_VIDEO_SCALING_SHARP : BROWSER_VIDEO_SCALING_SMOOTH;
}

bool browser_profile_video_startup_buffering(
    const BrowserProfile *profile)
{
    return profile == NULL || profile->video_startup_buffering;
}

bool browser_profile_resume_offline_downloads(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->resume_offline_downloads;
}

BrowserTextEntryMode browser_profile_text_entry_mode(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_TEXT_ENTRY_OSK : profile->text_entry_mode;
}

ContentBlockerMode browser_profile_content_blocker_mode(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? CONTENT_BLOCKER_BASIC : profile->content_blocker_mode;
}

bool browser_profile_content_blocker_cosmetic_hiding(
    const BrowserProfile *profile)
{
    return profile == NULL || profile->content_blocker_cosmetic_hiding;
}

uint64_t browser_profile_content_blocker_total_blocked(
    const BrowserProfile *profile)
{
    return profile == NULL ? 0 : profile->content_blocker_total_blocked;
}

BrowserReaderFont browser_profile_reader_font(const BrowserProfile *profile)
{
    return profile == NULL ? BROWSER_READER_FONT_SANS : profile->reader_font;
}

bool browser_profile_remember_reader_site_scale(
    const BrowserProfile *profile)
{
    return profile != NULL && profile->remember_reader_site_scale;
}

bool browser_profile_update_check_enabled(const BrowserProfile *profile)
{
    return profile == NULL || profile->update_check_enabled;
}

BrowserUpdateChannel browser_profile_update_channel(
    const BrowserProfile *profile)
{
    return profile == NULL
        ? BROWSER_UPDATE_CHANNEL_STABLE : profile->update_channel;
}

BrowserGlyphLanguage browser_profile_glyph_language(
    const BrowserProfile *profile)
{
    return profile == NULL ? BROWSER_GLYPH_LANGUAGE_EMBEDDED
                           : profile->glyph_language;
}

bool browser_profile_color_emoji(const BrowserProfile *profile)
{
    return profile != NULL && profile->color_emoji;
}

uint64_t browser_profile_update_check_last_unix(
    const BrowserProfile *profile)
{
    return profile == NULL ? 0 : profile->update_check_last_unix;
}

uint64_t browser_profile_update_check_available_sequence(
    const BrowserProfile *profile)
{
    return profile == NULL ? 0 : profile->update_check_available_sequence;
}

bool browser_profile_update_check_due(
    const BrowserProfile *profile, uint64_t now_unix_seconds)
{
    if (!browser_profile_update_check_enabled(profile)) return false;
    /* No usable wall clock: a completed check could not be recorded, so
       the cadence promise could not be kept. Stay silent instead. */
    if (now_unix_seconds == 0) return false;
    uint64_t last = browser_profile_update_check_last_unix(profile);
    if (last == 0) return true;
    /* A last check recorded in the future means the RTC moved backwards;
       treat it as never-checked rather than blocking until that date. */
    if (last > now_unix_seconds) return true;
    return now_unix_seconds - last
        >= BROWSER_PROFILE_UPDATE_CHECK_INTERVAL_SECONDS;
}

void browser_profile_set_ui_scale(BrowserProfile *profile, unsigned scale)
{
    if (profile != NULL) profile->ui_scale = scale >= 2u ? 2u : 1u;
}

void browser_profile_set_page_font_percent(
    BrowserProfile *profile, unsigned percent)
{
    if (profile != NULL && profile_valid_percent(percent))
        profile->page_font_percent = percent;
}

void browser_profile_set_history_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->history_enabled = enabled;
}

bool browser_profile_wave_background(const BrowserProfile *profile)
{
    return profile == NULL || profile->wave_background;
}

void browser_profile_set_wave_background(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->wave_background = enabled;
}

void browser_profile_set_update_check_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->update_check_enabled = enabled;
}

void browser_profile_set_update_channel(
    BrowserProfile *profile, BrowserUpdateChannel channel)
{
    if (profile != NULL && profile_valid_update_channel(channel))
        profile->update_channel = channel;
}

void browser_profile_set_glyph_language(
    BrowserProfile *profile, BrowserGlyphLanguage language)
{
    if (profile != NULL && profile_valid_glyph_language(language))
        profile->glyph_language = language;
}

void browser_profile_set_color_emoji(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->color_emoji = enabled;
}

void browser_profile_set_update_check_last_unix(
    BrowserProfile *profile, uint64_t unix_seconds)
{
    if (profile != NULL) profile->update_check_last_unix = unix_seconds;
}

void browser_profile_set_update_check_available_sequence(
    BrowserProfile *profile, uint64_t sequence)
{
    if (profile != NULL)
        profile->update_check_available_sequence = sequence;
}

void browser_profile_set_restore_last_page(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->restore_last_page = enabled;
}

void browser_profile_set_tab_hibernation_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->tab_hibernation_enabled = enabled;
}

void browser_profile_set_custom_homepage_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->custom_homepage_enabled = enabled;
}

void browser_profile_set_adaptive_voice_memory(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->adaptive_voice_memory = enabled;
}

void browser_profile_set_experimental_voice_input(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->experimental_voice_input = enabled;
}

void browser_profile_set_analog_cursor_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->analog_cursor_enabled = enabled;
}

void browser_profile_set_persistent_cache_mb(
    BrowserProfile *profile, unsigned megabytes)
{
    if (profile != NULL && profile_valid_cache_mb(megabytes))
        profile->persistent_cache_mb = megabytes;
}

void browser_profile_set_live_cache_kib(
    BrowserProfile *profile, unsigned kibibytes)
{
    if (profile != NULL && profile_valid_live_cache_kib(kibibytes))
        profile->live_cache_kib = kibibytes;
}

void browser_profile_set_persist_local_storage(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->persist_local_storage = enabled;
}

void browser_profile_set_tls_session_persistence(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->tls_session_persistence = enabled;
}

void browser_profile_set_javascript_enabled(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->javascript_enabled = enabled;
}

bool browser_profile_set_site_javascript_enabled(
    BrowserProfile *profile, const char *url, bool enabled)
{
    if (profile == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!profile_javascript_site_from_url(url, site)) return false;
    size_t found = profile->javascript_disabled_site_count;
    for (size_t i = 0; i < profile->javascript_disabled_site_count; i++) {
        if (strcmp(profile->javascript_disabled_sites[i], site) == 0) {
            found = i;
            break;
        }
    }
    if (enabled) {
        if (found == profile->javascript_disabled_site_count) return false;
        size_t last = --profile->javascript_disabled_site_count;
        if (found != last) {
            memcpy(profile->javascript_disabled_sites[found],
                   profile->javascript_disabled_sites[last],
                   CONTENT_BLOCKER_HOST_LIMIT);
        }
        memset(profile->javascript_disabled_sites[last], 0,
               CONTENT_BLOCKER_HOST_LIMIT);
        return true;
    }
    if (found != profile->javascript_disabled_site_count) return false;
    if (profile->javascript_disabled_site_count
        >= BROWSER_PROFILE_JAVASCRIPT_SITE_LIMIT) return false;
    snprintf(profile->javascript_disabled_sites[
                 profile->javascript_disabled_site_count++],
             CONTENT_BLOCKER_HOST_LIMIT, "%s", site);
    return true;
}

void browser_profile_set_site_data_allowed(
    BrowserProfile *profile, bool allowed)
{
    if (profile != NULL) profile->site_data_allowed = allowed;
}

void browser_profile_set_search_engine(
    BrowserProfile *profile, BrowserSearchEngine engine)
{
    if (profile != NULL && browser_search_engine_valid(engine))
        profile->search_engine = engine;
}

void browser_profile_set_color_mode(
    BrowserProfile *profile, BrowserColorMode mode)
{
    if (profile != NULL && profile_valid_color_mode(mode))
        profile->color_mode = mode;
}

void browser_profile_set_chrome_theme(
    BrowserProfile *profile, BrowserChromeTheme theme)
{
    if (profile != NULL && profile_valid_chrome_theme(theme))
        profile->chrome_theme = theme;
}

void browser_profile_set_video_scaling(
    BrowserProfile *profile, BrowserVideoScaling scaling)
{
    if (profile != NULL && profile_valid_video_scaling(scaling))
        profile->video_scaling_sharp =
            scaling == BROWSER_VIDEO_SCALING_SHARP;
}

void browser_profile_set_youtube_quality(
    BrowserProfile *profile, BrowserYoutubeQuality quality)
{
    if (profile != NULL && profile_valid_youtube_quality(quality))
        profile->youtube_quality = quality;
}

void browser_profile_set_youtube_compact_results(
    BrowserProfile *profile, bool compact)
{
    if (profile != NULL) profile->youtube_compact_results = compact;
}

void browser_profile_set_video_startup_buffering(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->video_startup_buffering = enabled;
}

void browser_profile_set_resume_offline_downloads(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->resume_offline_downloads = enabled;
}

void browser_profile_set_text_entry_mode(
    BrowserProfile *profile, BrowserTextEntryMode mode)
{
    if (profile != NULL && profile_valid_text_entry_mode(mode))
        profile->text_entry_mode = mode;
}

void browser_profile_set_content_blocker_mode(
    BrowserProfile *profile, ContentBlockerMode mode)
{
    if (profile != NULL && profile_valid_content_blocker_mode(mode))
        profile->content_blocker_mode = mode;
}

void browser_profile_set_content_blocker_cosmetic_hiding(
    BrowserProfile *profile, bool enabled)
{
    if (profile != NULL) profile->content_blocker_cosmetic_hiding = enabled;
}

void browser_profile_record_content_blocked(
    BrowserProfile *profile, uint64_t count)
{
    if (profile == NULL || count == 0) return;
    if (count > UINT64_MAX - profile->content_blocker_total_blocked)
        profile->content_blocker_total_blocked = UINT64_MAX;
    else
        profile->content_blocker_total_blocked += count;
}

void browser_profile_set_reader_font(
    BrowserProfile *profile, BrowserReaderFont font)
{
    if (profile != NULL && profile_valid_reader_font(font))
        profile->reader_font = font;
}

void browser_profile_set_remember_reader_site_scale(
    BrowserProfile *profile, bool enabled)
{
    if (profile == NULL) return;
    profile->remember_reader_site_scale = enabled;
    if (!enabled) {
        size_t kept = 0;
        for (size_t i = 0; i < profile->reader_site_count; i++) {
            if (!profile->reader_sites[i].always) continue;
            profile->reader_sites[i].percent = 100;
            profile->reader_sites[kept++] = profile->reader_sites[i];
        }
        memset(&profile->reader_sites[kept], 0,
               (BROWSER_PROFILE_READER_SITE_LIMIT - kept)
                   * sizeof(profile->reader_sites[0]));
        profile->reader_site_count = kept;
    }
}

bool browser_profile_reader_site_font_percent(
    const BrowserProfile *profile, const char *url, unsigned *percent)
{
    if (profile == NULL || url == NULL || percent == NULL
        || !profile->remember_reader_site_scale) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    for (size_t i = 0; i < profile->reader_site_count; i++) {
        if (strcmp(profile->reader_sites[i].site, site) == 0) {
            *percent = profile->reader_sites[i].percent;
            return true;
        }
    }
    return false;
}

bool browser_profile_record_reader_site_font_percent(
    BrowserProfile *profile, const char *url, unsigned percent)
{
    if (profile == NULL || url == NULL
        || !profile->remember_reader_site_scale
        || !profile_valid_percent(percent)) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    size_t found = profile->reader_site_count;
    for (size_t i = 0; i < profile->reader_site_count; i++) {
        if (strcmp(profile->reader_sites[i].site, site) == 0) {
            found = i;
            break;
        }
    }
    BrowserReaderSiteScale replacement = { .percent = percent };
    if (found < profile->reader_site_count)
        replacement.always = profile->reader_sites[found].always;
    snprintf(replacement.site, sizeof(replacement.site), "%s", site);
    if (found < profile->reader_site_count) {
        memmove(&profile->reader_sites[1], &profile->reader_sites[0],
                found * sizeof(profile->reader_sites[0]));
    } else {
        size_t retained = profile->reader_site_count;
        if (retained >= BROWSER_PROFILE_READER_SITE_LIMIT)
            retained = BROWSER_PROFILE_READER_SITE_LIMIT - 1u;
        memmove(&profile->reader_sites[1], &profile->reader_sites[0],
                retained * sizeof(profile->reader_sites[0]));
        if (profile->reader_site_count < BROWSER_PROFILE_READER_SITE_LIMIT)
            profile->reader_site_count++;
    }
    profile->reader_sites[0] = replacement;
    return true;
}

bool browser_profile_reader_site_always(
    const BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    for (size_t i = 0; i < profile->reader_site_count; i++)
        if (strcmp(profile->reader_sites[i].site, site) == 0)
            return profile->reader_sites[i].always;
    return false;
}

bool browser_profile_set_reader_site_always(
    BrowserProfile *profile, const char *url, bool enabled)
{
    if (profile == NULL || url == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    size_t found = profile->reader_site_count;
    for (size_t i = 0; i < profile->reader_site_count; i++)
        if (strcmp(profile->reader_sites[i].site, site) == 0) {
            found = i;
            break;
        }
    if (!enabled && found == profile->reader_site_count) return true;
    if (!enabled && !profile->remember_reader_site_scale) {
        if (found + 1u < profile->reader_site_count)
            memmove(&profile->reader_sites[found],
                    &profile->reader_sites[found + 1u],
                    (profile->reader_site_count - found - 1u)
                        * sizeof(profile->reader_sites[0]));
        memset(&profile->reader_sites[--profile->reader_site_count], 0,
               sizeof(profile->reader_sites[0]));
        return true;
    }
    if (found < profile->reader_site_count) {
        profile->reader_sites[found].always = enabled;
        return true;
    }
    BrowserReaderSiteScale added = {.percent = 100, .always = true};
    snprintf(added.site, sizeof(added.site), "%s", site);
    size_t retained = profile->reader_site_count;
    if (retained >= BROWSER_PROFILE_READER_SITE_LIMIT)
        retained = BROWSER_PROFILE_READER_SITE_LIMIT - 1u;
    memmove(&profile->reader_sites[1], &profile->reader_sites[0],
            retained * sizeof(profile->reader_sites[0]));
    profile->reader_sites[0] = added;
    if (profile->reader_site_count < BROWSER_PROFILE_READER_SITE_LIMIT)
        profile->reader_site_count++;
    return true;
}

size_t browser_profile_reader_site_count(const BrowserProfile *profile)
{
    return profile == NULL ? 0u : profile->reader_site_count;
}

bool browser_profile_content_blocker_site_allowed(
    const BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    for (size_t i = 0;
         i < profile->content_blocker_allowed_site_count; i++) {
        if (strcmp(profile->content_blocker_allowed_sites[i], site) == 0)
            return true;
    }
    return false;
}

static bool profile_security_site_allowed(
    const char (*sites)[CONTENT_BLOCKER_HOST_LIMIT], size_t count,
    const char *url)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (sites == NULL || url == NULL
        || !content_blocker_site_from_url(url, site)) return false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(sites[i], site) == 0) return true;
    return false;
}

static bool profile_security_site_set(
    char (*sites)[CONTENT_BLOCKER_HOST_LIMIT], size_t *count,
    const char *url, bool allowed)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (sites == NULL || count == NULL || url == NULL
        || !content_blocker_site_from_url(url, site)) return false;
    size_t found = *count;
    for (size_t i = 0; i < *count; i++)
        if (strcmp(sites[i], site) == 0) { found = i; break; }
    if (allowed) {
        if (found < *count) return true;
        if (*count >= BROWSER_PROFILE_SECURITY_SITE_LIMIT) return false;
        snprintf(sites[(*count)++], sizeof(sites[0]), "%s", site);
        return true;
    }
    if (found == *count) return true;
    if (found + 1u < *count)
        memmove(sites[found], sites[found + 1u],
                (*count - found - 1u) * sizeof(sites[0]));
    memset(sites[--*count], 0, sizeof(sites[0]));
    return true;
}

bool browser_profile_third_party_cookie_site_allowed(
    const BrowserProfile *profile, const char *url)
{
    return profile != NULL && profile_security_site_allowed(
        profile->third_party_cookie_allowed_sites,
        profile->third_party_cookie_allowed_site_count, url);
}

bool browser_profile_set_third_party_cookie_site_allowed(
    BrowserProfile *profile, const char *url, bool allowed)
{
    return profile != NULL && profile_security_site_set(
        profile->third_party_cookie_allowed_sites,
        &profile->third_party_cookie_allowed_site_count, url, allowed);
}

bool browser_profile_cookie_banner_hidden(
    const BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)
        || strcmp(site, "tilefinch.local") == 0) return false;
    return !profile_security_site_allowed(
        profile->cookie_banner_visible_sites,
        profile->cookie_banner_visible_site_count, url);
}

bool browser_profile_set_cookie_banner_hidden(
    BrowserProfile *profile, const char *url, bool hidden)
{
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (profile == NULL || !content_blocker_site_from_url(url, site)
        || strcmp(site, "tilefinch.local") == 0) return false;
    /* The table stores exceptions (visible banners), so the public API keeps
       the common/default case positive while persistence stays sparse. */
    return profile_security_site_set(
        profile->cookie_banner_visible_sites,
        &profile->cookie_banner_visible_site_count, url, !hidden);
}

bool browser_profile_set_content_blocker_site_allowed(
    BrowserProfile *profile, const char *url, bool allowed)
{
    if (profile == NULL || url == NULL) return false;
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    size_t found = profile->content_blocker_allowed_site_count;
    for (size_t i = 0;
         i < profile->content_blocker_allowed_site_count; i++) {
        if (strcmp(profile->content_blocker_allowed_sites[i], site) == 0) {
            found = i;
            break;
        }
    }
    if (allowed) {
        if (found < profile->content_blocker_allowed_site_count) return true;
        if (profile->content_blocker_allowed_site_count
                >= CONTENT_BLOCKER_ALLOW_SITE_LIMIT) return false;
        snprintf(
            profile->content_blocker_allowed_sites[
                profile->content_blocker_allowed_site_count++],
            CONTENT_BLOCKER_HOST_LIMIT, "%s", site);
        return true;
    }
    if (found == profile->content_blocker_allowed_site_count) return true;
    size_t remaining = profile->content_blocker_allowed_site_count
                       - found - 1u;
    if (remaining != 0) {
        memmove(
            &profile->content_blocker_allowed_sites[found],
            &profile->content_blocker_allowed_sites[found + 1u],
            remaining * sizeof(profile->content_blocker_allowed_sites[0]));
    }
    profile->content_blocker_allowed_site_count--;
    memset(
        profile->content_blocker_allowed_sites[
            profile->content_blocker_allowed_site_count],
        0, CONTENT_BLOCKER_HOST_LIMIT);
    return true;
}

size_t browser_profile_content_blocker_allowed_site_count(
    const BrowserProfile *profile)
{
    return profile == NULL ? 0
        : profile->content_blocker_allowed_site_count;
}

const char *browser_profile_content_blocker_allowed_site(
    const BrowserProfile *profile, size_t index)
{
    return profile == NULL
        || index >= profile->content_blocker_allowed_site_count
        ? NULL : profile->content_blocker_allowed_sites[index];
}

bool browser_profile_import_content_blocker_allowed_sites(
    BrowserProfile *profile, const char *path,
    BrowserProfileAllowlistImport *result)
{
    BrowserProfileAllowlistImport local = {0};
    if (result != NULL) *result = local;
    if (profile == NULL || path == NULL || path[0] == '\0') return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char line[PROFILE_ALLOWLIST_LINE_LIMIT];
    size_t bytes = 0;
    bool profile_records = false;
    bool first_line = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t physical = strlen(line);
        bytes += physical;
        if (bytes > PROFILE_ALLOWLIST_FILE_LIMIT) {
            local.truncated = true;
            break;
        }
        bool complete = physical != 0
            && (line[physical - 1u] == '\n' || feof(file));
        if (!complete) {
            int byte;
            while ((byte = fgetc(file)) != EOF && byte != '\n') {
                if (++bytes > PROFILE_ALLOWLIST_FILE_LIMIT) break;
            }
            local.ignored++;
            if (bytes > PROFILE_ALLOWLIST_FILE_LIMIT) {
                local.truncated = true;
                break;
            }
            first_line = false;
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (first_line && strcmp(line, "TILEFINCH_PROFILE\t1") == 0) {
            profile_records = true;
            first_line = false;
            continue;
        }
        first_line = false;
        char decoded[CONTENT_BLOCKER_HOST_LIMIT];
        const char *candidate = line;
        if (profile_records) {
            if (strncmp(line, "A\t", 2) != 0) continue;
            if (!profile_decode(
                    line + 2, decoded, sizeof(decoded))) {
                local.ignored++;
                continue;
            }
            candidate = decoded;
        } else {
            while (*candidate == ' ' || *candidate == '\t') candidate++;
            char *end = (char *) candidate + strlen(candidate);
            while (end > candidate
                   && isspace((unsigned char) end[-1])) *--end = '\0';
            if (*candidate == '\0' || *candidate == '!' || *candidate == '#')
                continue;
        }
        char url[CONTENT_BLOCKER_HOST_LIMIT + 16u];
        const char *site_url = candidate;
        if (strncmp(candidate, "https://", 8) == 0
            || strncmp(candidate, "http://", 7) == 0) {
        } else {
            if (!profile_valid_block_site(candidate)
                || snprintf(url, sizeof(url), "https://%s/", candidate)
                       >= (int) sizeof(url)) {
                local.ignored++;
                continue;
            }
            site_url = url;
        }
        if (browser_profile_content_blocker_site_allowed(profile, site_url)) {
            local.duplicate++;
            continue;
        }
        if (profile->content_blocker_allowed_site_count
                >= CONTENT_BLOCKER_ALLOW_SITE_LIMIT) {
            local.resident_full = true;
            break;
        }
        if (!browser_profile_set_content_blocker_site_allowed(
                profile, site_url, true)) {
            local.ignored++;
            continue;
        }
        local.added++;
    }
    bool okay = fclose(file) == 0;
    if (result != NULL) *result = local;
    return okay;
}

size_t browser_profile_bookmark_count(const BrowserProfile *profile)
{
    return profile == NULL ? 0 : profile->bookmark_count;
}

const BrowserProfilePage *browser_profile_bookmark(
    const BrowserProfile *profile, size_t index)
{
    return profile == NULL || index >= profile->bookmark_count
        ? NULL : &profile->bookmarks[index];
}

size_t browser_profile_history_count(const BrowserProfile *profile)
{
    return profile == NULL ? 0 : profile->history_count;
}

const BrowserProfilePage *browser_profile_history(
    const BrowserProfile *profile, size_t index)
{
    return profile == NULL || index >= profile->history_count
        ? NULL : &profile->history[index];
}

/*
 * Forgetting one address. History is a recency-ordered list, so an entry is
 * removed by closing the gap rather than tombstoning it; the file is rewritten
 * whole on save, so nothing survives the removal on disk either.
 */
bool browser_profile_forget_history(BrowserProfile *profile, const char *url)
{
    if (profile == NULL || url == NULL || url[0] == '\0') return false;
    for (size_t i = 0; i < profile->history_count; i++) {
        if (strcmp(profile->history[i].url, url) != 0) continue;
        size_t remaining = profile->history_count - i - 1u;
        if (remaining != 0) {
            memmove(
                &profile->history[i], &profile->history[i + 1u],
                remaining * sizeof(profile->history[0]));
        }
        profile->history_count--;
        memset(
            &profile->history[profile->history_count], 0,
            sizeof(profile->history[0]));
        return true;
    }
    return false;
}

static unsigned char profile_ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char) (value + ('a' - 'A')) : value;
}

static bool profile_ascii_prefix(const char *text, const char *query)
{
    if (text == NULL || query == NULL) return false;
    while (*query != '\0') {
        if (*text == '\0'
            || profile_ascii_lower((unsigned char) *text++)
                   != profile_ascii_lower((unsigned char) *query++))
            return false;
    }
    return true;
}

static bool profile_ascii_contains(
    const char *text, const char *query, bool word_prefix)
{
    if (text == NULL || query == NULL || query[0] == '\0') return false;
    for (size_t at = 0; text[at] != '\0'; at++) {
        if (word_prefix && at != 0
            && (isalnum((unsigned char) text[at - 1u])
                || text[at - 1u] == '_')) continue;
        if (profile_ascii_prefix(text + at, query)) return true;
    }
    return false;
}

static const char *profile_searchable_url(const char *url)
{
    if (url == NULL) return "";
    if (strncasecmp(url, "https://", 8) == 0) url += 8;
    else if (strncasecmp(url, "http://", 7) == 0) url += 7;
    if (strncasecmp(url, "www.", 4) == 0) url += 4;
    return url;
}

static unsigned profile_suggestion_rank(
    const BrowserProfilePage *page, const char *query)
{
    const char *url = profile_searchable_url(page->url);
    if (profile_ascii_prefix(page->title, query)) return 0;
    if (profile_ascii_prefix(url, query)) return 1;
    if (profile_ascii_contains(page->title, query, true)) return 2;
    if (profile_ascii_contains(page->title, query, false)) return 3;
    if (profile_ascii_contains(url, query, false)) return 4;
    return UINT_MAX;
}

static void profile_suggestion_consider(
    const BrowserProfilePage *page, bool bookmark, size_t recency,
    const char *query, BrowserProfileSuggestion *suggestions,
    unsigned scores[BROWSER_PROFILE_SUGGESTION_LIMIT], size_t *count,
    size_t capacity)
{
    unsigned rank = profile_suggestion_rank(page, query);
    if (rank == UINT_MAX) return;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(suggestions[i].url, page->url) == 0) return;
    }
    unsigned recency_score =
        (unsigned) (recency > 99u ? 99u : recency) * 10u;
    unsigned visit_credit = page->visits > 99u ? 495u
                                               : page->visits * 5u;
    unsigned score = rank * 10000u + (bookmark ? 0u : 1000u)
        + (recency_score > visit_credit
               ? recency_score - visit_credit : 0u);
    size_t insert = 0;
    while (insert < *count && scores[insert] <= score) insert++;
    if (insert >= capacity) return;
    size_t move = *count < capacity ? *count - insert
                                    : capacity - insert - 1u;
    if (move != 0) {
        memmove(suggestions + insert + 1u, suggestions + insert,
                move * sizeof(*suggestions));
        memmove(scores + insert + 1u, scores + insert,
                move * sizeof(*scores));
    }
    suggestions[insert] = (BrowserProfileSuggestion) {
        .url = page->url,
        .title = page->title,
        .bookmark = bookmark
    };
    scores[insert] = score;
    if (*count < capacity) (*count)++;
}

size_t browser_profile_suggest(
    const BrowserProfile *profile, const char *query,
    BrowserProfileSuggestion *suggestions, size_t capacity)
{
    if (profile == NULL || query == NULL || query[0] == '\0'
        || suggestions == NULL || capacity == 0) return 0;
    query = profile_searchable_url(query);
    if (query[0] == '\0') return 0;
    if (capacity > BROWSER_PROFILE_SUGGESTION_LIMIT)
        capacity = BROWSER_PROFILE_SUGGESTION_LIMIT;
    memset(suggestions, 0, capacity * sizeof(*suggestions));
    unsigned scores[BROWSER_PROFILE_SUGGESTION_LIMIT] = {0};
    size_t count = 0;
    for (size_t i = 0; i < profile->bookmark_count; i++) {
        profile_suggestion_consider(
            &profile->bookmarks[i], true, i, query,
            suggestions, scores, &count, capacity);
    }
    if (profile->history_enabled) {
        for (size_t i = 0; i < profile->history_count; i++) {
            profile_suggestion_consider(
                &profile->history[i], false, i, query,
                suggestions, scores, &count, capacity);
        }
    }
    return count;
}

bool browser_profile_record_resume(
    BrowserProfile *profile, const char *video_id,
    uint64_t position_us, uint64_t duration_us)
{
    if (profile == NULL || video_id == NULL || video_id[0] == '\0'
        || strlen(video_id) >= sizeof(profile->resumes[0].video_id))
        return false;
    size_t found = profile->resume_count;
    for (size_t i = 0; i < profile->resume_count; i++) {
        if (strcmp(profile->resumes[i].video_id, video_id) == 0) {
            found = i;
            break;
        }
    }
    if (found < profile->resume_count) {
        BrowserProfileResume existing = profile->resumes[found];
        if (found == 0
            && existing.position_us == position_us
            && existing.duration_us == duration_us) return false;
        memmove(
            &profile->resumes[1], &profile->resumes[0],
            found * sizeof(*profile->resumes));
        profile->resumes[0] = existing;
    } else {
        size_t move = profile->resume_count < BROWSER_PROFILE_RESUME_LIMIT
            ? profile->resume_count : BROWSER_PROFILE_RESUME_LIMIT - 1u;
        memmove(
            &profile->resumes[1], &profile->resumes[0],
            move * sizeof(*profile->resumes));
        if (profile->resume_count < BROWSER_PROFILE_RESUME_LIMIT)
            profile->resume_count++;
        memset(&profile->resumes[0], 0, sizeof(profile->resumes[0]));
        profile_copy(
            profile->resumes[0].video_id,
            sizeof(profile->resumes[0].video_id), video_id);
    }
    profile->resumes[0].position_us = position_us;
    profile->resumes[0].duration_us = duration_us;
    return true;
}

bool browser_profile_resume(
    const BrowserProfile *profile, const char *video_id,
    BrowserProfileResume *resume)
{
    if (profile == NULL || video_id == NULL || resume == NULL) return false;
    for (size_t i = 0; i < profile->resume_count; i++) {
        if (strcmp(profile->resumes[i].video_id, video_id) == 0) {
            *resume = profile->resumes[i];
            return true;
        }
    }
    return false;
}

static uint32_t recovery_checksum(const char *url, int scroll_y)
{
    uint32_t hash = UINT32_C(2166136261);
    const unsigned char *at = (const unsigned char *) url;
    while (at != NULL && *at != '\0') {
        hash ^= *at++;
        hash *= UINT32_C(16777619);
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (uint32_t) scroll_y >> shift;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

bool browser_recovery_save(
    const char *path, const char *url, int scroll_y)
{
    if (path == NULL || path[0] == '\0'
        || !recovery_url_supported(url)) return false;
    if (scroll_y < 0) scroll_y = 0;
    char temporary[1200];
    char encoded[BROWSER_PROFILE_URL_LIMIT * 3u];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t) length >= sizeof(temporary)
        || !profile_encode(url, encoded, sizeof(encoded))) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    uint32_t checksum = recovery_checksum(url, scroll_y);
    bool ok = fprintf(
        file, "TILEFINCH_RECOVERY\t1\nN\t%s\t%d\t%08x\n",
        encoded, scroll_y, (unsigned) checksum) > 0;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove(temporary);
        return false;
    }
    if (rename(temporary, path) == 0) return true;
    if (remove(path) == 0 && rename(temporary, path) == 0) return true;
    remove(temporary);
    return false;
}

bool browser_recovery_load(
    const char *path, BrowserRecoveryCheckpoint *checkpoint)
{
    if (path == NULL || path[0] == '\0' || checkpoint == NULL)
        return false;
    memset(checkpoint, 0, sizeof(*checkpoint));
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char header[64] = {0};
    char line[BROWSER_PROFILE_URL_LIMIT * 3u + 64u];
    bool ok = fgets(header, sizeof(header), file) != NULL;
    header[strcspn(header, "\r\n")] = '\0';
    ok = ok && strcmp(header, "TILEFINCH_RECOVERY\t1") == 0
        && fgets(line, sizeof(line), file) != NULL;
    ok = fclose(file) == 0 && ok;
    if (!ok) return false;
    line[strcspn(line, "\r\n")] = '\0';
    char *encoded = strchr(line, '\t');
    if (encoded == NULL || strncmp(line, "N\t", 2) != 0) return false;
    encoded++;
    char *scroll_text = strchr(encoded, '\t');
    if (scroll_text == NULL) return false;
    *scroll_text++ = '\0';
    char *checksum_text = strchr(scroll_text, '\t');
    if (checksum_text == NULL) return false;
    *checksum_text++ = '\0';
    char *scroll_end = NULL;
    long parsed_scroll = strtol(scroll_text, &scroll_end, 10);
    char *checksum_end = NULL;
    unsigned long parsed_checksum =
        strtoul(checksum_text, &checksum_end, 16);
    if (scroll_end == scroll_text || *scroll_end != '\0'
        || parsed_scroll < 0 || parsed_scroll > INT_MAX
        || checksum_end == checksum_text || *checksum_end != '\0'
        || parsed_checksum > UINT32_MAX
        || !profile_decode(
               encoded, checkpoint->url, sizeof(checkpoint->url))
        || !recovery_url_supported(checkpoint->url)) {
        memset(checkpoint, 0, sizeof(*checkpoint));
        return false;
    }
    checkpoint->scroll_y = (int) parsed_scroll;
    if ((uint32_t) parsed_checksum
            != recovery_checksum(checkpoint->url, checkpoint->scroll_y)) {
        memset(checkpoint, 0, sizeof(*checkpoint));
        return false;
    }
    return true;
}

bool browser_recovery_clear(const char *path)
{
    if (path == NULL || path[0] == '\0') return false;
    return remove(path) == 0;
}

static bool profile_html_reserve(ProfileHtml *html, size_t addition)
{
    if (addition > PROFILE_HTML_LIMIT
        || html->length > PROFILE_HTML_LIMIT - addition) return false;
    size_t needed = html->length + addition + 1u;
    if (needed <= html->capacity) return true;
    size_t capacity = html->capacity == 0 ? 4096u : html->capacity;
    while (capacity < needed && capacity < PROFILE_HTML_LIMIT + 1u)
        capacity *= 2u;
    if (capacity > PROFILE_HTML_LIMIT + 1u)
        capacity = PROFILE_HTML_LIMIT + 1u;
    char *grown = budget_realloc_category(
        html->budget, BUDGET_CATEGORY_RESOURCE, html->data, capacity);
    if (grown == NULL) return false;
    html->data = grown;
    html->capacity = capacity;
    return true;
}

static bool profile_html_text(ProfileHtml *html, const char *text)
{
    size_t length = strlen(text);
    if (!profile_html_reserve(html, length)) return false;
    memcpy(html->data + html->length, text, length + 1u);
    html->length += length;
    return true;
}

static bool profile_html_escape(ProfileHtml *html, const char *text)
{
    for (const unsigned char *at = (const unsigned char *)
             (text == NULL ? "" : text); *at != '\0'; at++) {
        const char *escaped = NULL;
        if (*at == '&') escaped = "&amp;";
        else if (*at == '<') escaped = "&lt;";
        else if (*at == '>') escaped = "&gt;";
        else if (*at == '"') escaped = "&quot;";
        if (escaped != NULL) {
            if (!profile_html_text(html, escaped)) return false;
        } else {
            char byte[2] = {(char) *at, '\0'};
            if (!profile_html_text(html, byte)) return false;
        }
    }
    return true;
}

bool browser_profile_build_page(
    BrowserProfile *profile, bool history,
    char **html, size_t *length)
{
    if (profile == NULL || html == NULL || length == NULL) return false;
    *html = NULL;
    *length = 0;
    ProfileHtml out = {.budget = profile->budget};
    const BrowserProfilePage *pages =
        history ? profile->history : profile->bookmarks;
    size_t count = history ? profile->history_count : profile->bookmark_count;
    bool ok = profile_html_text(
        &out, "<!doctype html><meta name=viewport content=\"width=device-width"
              ",initial-scale=1\"><style>*{box-sizing:border-box}body{margin:"
              "0;padding:14px;background:#f7f8f8;color:#17212b;font-family:"
              "sans-serif}h1{font-size:22px}.item{display:block;padding:12px;"
              "margin:8px 0;background:white;border:1px solid #ccd4d8;border-"
              "radius:8px;color:#17212b;text-decoration:none}.url{display:"
              "block;color:#60727e;font-size:12px;margin-top:4px;overflow-wrap:"
              "anywhere}.empty{padding:20px;background:#fff;border-radius:8px}"
              "</style><title>");
    ok = ok && profile_html_text(
        &out, history ? "History" : "Bookmarks")
        && profile_html_text(&out, "</title><h1>")
        && profile_html_text(&out, history ? "URL history" : "Bookmarks")
        && profile_html_text(&out, "</h1>");
    if (ok && history && !profile->history_enabled)
        ok = profile_html_text(
            &out, "<p class=empty>URL history is off. Enable it in Options."
                  "</p>");
    else if (ok && count == 0)
        ok = profile_html_text(
            &out, "<p class=empty>No saved pages yet.</p>");
    for (size_t i = 0; ok && i < count; i++) {
        ok = profile_html_text(&out, "<a class=item href=\"")
            && profile_html_escape(&out, pages[i].url)
            && profile_html_text(&out, "\">")
            && profile_html_escape(&out, pages[i].title)
            && profile_html_text(&out, "<span class=url>")
            && profile_html_escape(&out, pages[i].url)
            && profile_html_text(&out, "</span></a>");
    }
    if (!ok) {
        budget_free(profile->budget, out.data);
        return false;
    }
    *html = out.data;
    *length = out.length;
    return true;
}

bool browser_profile_build_homepage(
    BrowserProfile *profile, char **html, size_t *length)
{
    if (profile == NULL || html == NULL || length == NULL) return false;
    *html = NULL;
    *length = 0;
    ProfileHtml out = {.budget = profile->budget};
    bool ok = profile_html_text(
        &out, "<!doctype html><meta name=viewport content=\"width=device-width"
              ",initial-scale=1\"><title>My homepage</title><style>"
              "*{box-sizing:border-box}body{margin:0;padding:14px;background:"
              "#edf3f4;color:#17212b;font-family:sans-serif}h1{font-size:"
              "23px;margin:0 0 4px}.intro{color:#52646d;margin:0 0 12px;"
              "font-size:13px}.grid{display:grid;grid-template-columns:"
              "repeat(2,minmax(0,1fr));gap:9px}.item{display:block;min-height:"
              "68px;padding:11px;background:#fff;border:1px solid #c8d4d8;"
              "border-radius:9px;color:#17212b;text-decoration:none;font-"
              "weight:bold;overflow-wrap:anywhere}.url{display:block;color:"
              "#60727e;font-size:10px;font-weight:normal;margin-top:5px;"
              "overflow:hidden}.empty{padding:18px;background:#fff;border:"
              "1px solid #c8d4d8;border-radius:9px;line-height:1.35}</style>"
              "<h1>My homepage</h1><p class=intro>Your saved pages</p>");
    if (ok && profile->bookmark_count == 0) {
        ok = profile_html_text(
            &out, "<p class=empty>No homepage links yet. Open Library on a "
                  "page and choose Add or remove bookmark.</p>");
    } else if (ok) {
        ok = profile_html_text(&out, "<div class=grid>");
        for (size_t i = 0; ok && i < profile->bookmark_count; i++) {
            const BrowserProfilePage *page = &profile->bookmarks[i];
            ok = profile_html_text(&out, "<a class=item href=\"")
                && profile_html_escape(&out, page->url)
                && profile_html_text(&out, "\">")
                && profile_html_escape(&out, page->title)
                && profile_html_text(&out, "<span class=url>")
                && profile_html_escape(&out, page->url)
                && profile_html_text(&out, "</span></a>");
        }
        ok = ok && profile_html_text(&out, "</div>");
    }
    if (!ok) {
        budget_free(profile->budget, out.data);
        return false;
    }
    *html = out.data;
    *length = out.length;
    return true;
}
