#include "tilefinch/browser_profile.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static bool file_present(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || needle == NULL) return false;
    char buffer[512];
    bool found = false;
    while (fgets(buffer, sizeof(buffer), file) != NULL)
        if (strstr(buffer, needle) != NULL) { found = true; break; }
    fclose(file);
    return found;
}

static bool seal_profile(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint32_t checksum = UINT32_C(2166136261);
    size_t length = 0;
    unsigned char buffer[512];
    bool ok = true;
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), file);
        for (size_t i = 0; i < got; i++) {
            checksum ^= buffer[i];
            checksum *= UINT32_C(16777619);
        }
        length += got;
        if (got != sizeof(buffer)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    file = fopen(path, "ab");
    if (file == NULL) return false;
    ok = fprintf(file, "END\t%zu\t%08X\n", length,
                 (unsigned) checksum) > 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    CHECK(browser_profile_ui_scale(profile) == 1
          && browser_profile_page_font_percent(profile) == 100
          && !browser_profile_history_enabled(profile)
          && !browser_profile_restore_last_page(profile)
          && !browser_profile_tab_hibernation_enabled(profile)
          && !browser_profile_custom_homepage_enabled(profile)
          && !browser_profile_experimental_voice_input(profile)
          && !browser_profile_adaptive_voice_memory(profile)
          && browser_profile_analog_cursor_enabled(profile)
          && browser_profile_persistent_cache_mb(profile) == 0
          && browser_profile_live_cache_kib(profile) == 512
          && !browser_profile_persist_local_storage(profile)
          && browser_profile_tls_session_persistence(profile)
          && browser_profile_javascript_enabled(profile)
          && browser_profile_site_javascript_enabled(
                 profile, "https://problem.example/page")
          && browser_profile_javascript_allowed_for_url(
                 profile, "https://problem.example/page")
          && browser_profile_site_data_allowed(profile)
          && !browser_profile_third_party_cookie_site_allowed(
                 profile, "https://compat.example/")
          && browser_profile_cookie_banner_hidden(
                 profile, "https://news.example/")
          && browser_profile_search_engine(profile)
                 == BROWSER_SEARCH_GOOGLE
          && browser_profile_color_mode(profile)
                 == BROWSER_COLOR_MODE_AUTO
          && browser_profile_chrome_theme(profile)
                 == BROWSER_CHROME_THEME_FINCH
          && browser_profile_youtube_quality(profile)
                 == BROWSER_YOUTUBE_QUALITY_360P
          && !browser_profile_youtube_compact_results(profile)
          && browser_profile_video_scaling(profile)
                 == BROWSER_VIDEO_SCALING_SMOOTH
          && browser_profile_video_startup_buffering(profile)
          && !browser_profile_resume_offline_downloads(profile)
          && browser_profile_text_entry_mode(profile)
                 == BROWSER_TEXT_ENTRY_OSK
          && browser_profile_content_blocker_mode(profile)
                 == CONTENT_BLOCKER_BASIC
          && browser_profile_content_blocker_cosmetic_hiding(profile)
          && browser_profile_content_blocker_total_blocked(profile) == 0
          && browser_profile_reader_font(profile)
                 == BROWSER_READER_FONT_SANS
          && !browser_profile_remember_reader_site_scale(profile)
          && browser_profile_reader_site_count(profile) == 0
          && browser_profile_update_check_enabled(profile)
          && browser_profile_update_channel(profile)
                 == BROWSER_UPDATE_CHANNEL_STABLE
          && browser_profile_glyph_language(profile)
                 == BROWSER_GLYPH_LANGUAGE_EMBEDDED
          && !browser_profile_color_emoji(profile)
          && browser_profile_update_check_last_unix(profile) == 0
          && browser_profile_update_check_available_sequence(profile) == 0
          && browser_profile_content_blocker_allowed_site_count(profile)
                 == 0);
    CHECK(!browser_profile_record_history(
        profile, "https://off.test/", "off"));
    browser_profile_set_ui_scale(profile, 2);
    browser_profile_set_page_font_percent(profile, 125);
    browser_profile_set_history_enabled(profile, true);
    browser_profile_set_restore_last_page(profile, true);
    browser_profile_set_tab_hibernation_enabled(profile, true);
    browser_profile_set_custom_homepage_enabled(profile, true);
    browser_profile_set_experimental_voice_input(profile, true);
    browser_profile_set_adaptive_voice_memory(profile, true);
    browser_profile_set_analog_cursor_enabled(profile, false);
    browser_profile_set_persistent_cache_mb(profile, 4);
    browser_profile_set_persistent_cache_mb(profile, 3);
    browser_profile_set_live_cache_kib(profile, 1024);
    browser_profile_set_live_cache_kib(profile, 768);
    browser_profile_set_live_cache_kib(profile, 4096);
    browser_profile_set_persist_local_storage(profile, true);
    browser_profile_set_tls_session_persistence(profile, false);
    CHECK(browser_profile_set_site_javascript_enabled(
              profile, "https://problem.example/page", false)
          && !browser_profile_site_javascript_enabled(
                 profile, "https://problem.example/elsewhere")
          && !browser_profile_javascript_allowed_for_url(
                 profile, "https://problem.example/elsewhere")
          && browser_profile_javascript_allowed_for_url(
                 profile, "https://other.example/")
          && !browser_profile_set_site_javascript_enabled(
                 profile, "https://problem.example/again", false)
          && !browser_profile_set_site_javascript_enabled(
                 profile, "https://tilefinch.local/home", false));
    browser_profile_set_javascript_enabled(profile, false);
    browser_profile_set_site_data_allowed(profile, false);
    CHECK(browser_profile_persistent_cache_mb(profile) == 4
          && browser_profile_live_cache_kib(profile) == 4096);
    browser_profile_set_search_engine(profile, BROWSER_SEARCH_DUCKDUCKGO);
    browser_profile_set_color_mode(profile, BROWSER_COLOR_MODE_DARK);
    browser_profile_set_chrome_theme(profile, BROWSER_CHROME_THEME_PLUM);
    CHECK(browser_profile_wave_background(profile));
    browser_profile_set_wave_background(profile, false);
    browser_profile_set_youtube_quality(
        profile, BROWSER_YOUTUBE_QUALITY_240P);
    browser_profile_set_youtube_compact_results(profile, true);
    browser_profile_set_video_scaling(profile, BROWSER_VIDEO_SCALING_SHARP);
    browser_profile_set_video_startup_buffering(profile, false);
    browser_profile_set_resume_offline_downloads(profile, true);
    browser_profile_set_text_entry_mode(
        profile, BROWSER_TEXT_ENTRY_DANZEFF);
    browser_profile_set_content_blocker_mode(
        profile, CONTENT_BLOCKER_CUSTOM);
    browser_profile_set_content_blocker_cosmetic_hiding(profile, false);
    browser_profile_record_content_blocked(profile, 37);
    browser_profile_set_reader_font(profile, BROWSER_READER_FONT_SERIF);
    browser_profile_set_update_check_enabled(profile, false);
    browser_profile_set_update_check_last_unix(
        profile, UINT64_C(1750000000));
    browser_profile_set_update_check_available_sequence(profile, 43);
    browser_profile_set_update_channel(
        profile, BROWSER_UPDATE_CHANNEL_BETA);
    browser_profile_set_glyph_language(
        profile, BROWSER_GLYPH_LANGUAGE_JAPANESE);
    browser_profile_set_color_emoji(profile, true);
    CHECK(!browser_profile_record_reader_site_font_percent(
        profile, "https://en.wikipedia.org/", 125));
    browser_profile_set_remember_reader_site_scale(profile, true);
    for (unsigned i = 0; i < BROWSER_PROFILE_READER_SITE_LIMIT + 1u; i++) {
        char reader_url[96];
        snprintf(reader_url, sizeof(reader_url),
                 "https://reader%u.test/article", i);
        CHECK(browser_profile_record_reader_site_font_percent(
            profile, reader_url, i % 2u == 0 ? 100u : 125u));
    }
    CHECK(browser_profile_record_reader_site_font_percent(
              profile, "https://en.wikipedia.org/wiki/PSP", 150)
          && browser_profile_reader_site_count(profile)
                 == BROWSER_PROFILE_READER_SITE_LIMIT);
    CHECK(browser_profile_set_reader_site_always(
              profile, "https://en.wikipedia.org/wiki/PSP", true)
          && browser_profile_reader_site_always(
              profile, "https://m.wikipedia.org/wiki/PSP"));
    unsigned reader_percent = 0;
    CHECK(browser_profile_reader_site_font_percent(
              profile, "https://m.wikipedia.org/", &reader_percent)
          && reader_percent == 150);
    CHECK(browser_profile_set_content_blocker_site_allowed(
              profile, "https://en.wikipedia.org/wiki/PSP", true)
          && browser_profile_content_blocker_site_allowed(
              profile, "https://m.wikipedia.org/")
          && browser_profile_content_blocker_allowed_site_count(profile) == 1
          && strcmp(
                 browser_profile_content_blocker_allowed_site(profile, 0),
                 "wikipedia.org") == 0);
    CHECK(browser_profile_set_third_party_cookie_site_allowed(
              profile, "https://cookies.example/page", true)
          && browser_profile_third_party_cookie_site_allowed(
                 profile, "https://sub.cookies.example/other"));
    CHECK(browser_profile_set_cookie_banner_hidden(
              profile, "https://news.example/article", false)
          && !browser_profile_cookie_banner_hidden(
                 profile, "https://m.news.example/other")
          && browser_profile_cookie_banner_hidden(
                 profile, "https://other.example/"));
    CHECK(browser_profile_add_bookmark(
        profile, "https://example.test/a?x=1&y=2", "A <page>"));
    CHECK(browser_profile_has_bookmark(
              profile, "https://example.test/a?x=1&y=2")
          && browser_profile_remove_bookmark(
              profile, "https://example.test/a?x=1&y=2")
          && !browser_profile_has_bookmark(
              profile, "https://example.test/a?x=1&y=2")
          && browser_profile_bookmark_count(profile) == 0
          && browser_profile_add_bookmark(
              profile, "https://example.test/a?x=1&y=2", "A <page>"));
    for (unsigned i = 0; i < 105; i++) {
        char url[96], title[32];
        snprintf(url, sizeof(url), "https://history.test/%u", i);
        snprintf(title, sizeof(title), "Page %u", i);
        CHECK(browser_profile_record_history(profile, url, title));
    }
    CHECK(browser_profile_history_count(profile)
          == BROWSER_PROFILE_HISTORY_LIMIT);
    const BrowserProfilePage *newest =
        browser_profile_history(profile, 0);
    CHECK(newest != NULL
          && strcmp(newest->url, "https://history.test/104") == 0);
    /* Forgetting one address closes the gap and leaves recency intact; an
       address that is not there is not an error, it is just not there. */
    CHECK(browser_profile_forget_history(
              profile, "https://history.test/104")
          && browser_profile_history_count(profile)
                 == BROWSER_PROFILE_HISTORY_LIMIT - 1u);
    newest = browser_profile_history(profile, 0);
    CHECK(newest != NULL
          && strcmp(newest->url, "https://history.test/103") == 0);
    CHECK(!browser_profile_forget_history(
              profile, "https://history.test/104")
          && !browser_profile_forget_history(profile, "")
          && !browser_profile_forget_history(profile, NULL)
          && browser_profile_history_count(profile)
                 == BROWSER_PROFILE_HISTORY_LIMIT - 1u);
    CHECK(browser_profile_record_history(
              profile, "https://history.test/104", "Page 104")
          && browser_profile_history_count(profile)
                 == BROWSER_PROFILE_HISTORY_LIMIT);
    CHECK(browser_profile_add_bookmark(
        profile, "https://en.wikipedia.org/wiki/PSP",
        "PlayStation Portable - Wikipedia"));
    CHECK(browser_profile_record_history(
        profile, "https://old.reddit.com/r/PSP/", "PSP on Reddit"));
    CHECK(browser_profile_record_history(
              profile, "https://old.reddit.com/r/PSP/", "PSP on Reddit")
          && browser_profile_record_history(
              profile, "https://old.reddit.com/r/PSP/", "PSP on Reddit")
          && browser_profile_history(profile, 0)->visits == 3);
    BrowserProfileSuggestion suggestions[BROWSER_PROFILE_SUGGESTION_LIMIT];
    size_t suggestion_count = browser_profile_suggest(
        profile, "wiki", suggestions, BROWSER_PROFILE_SUGGESTION_LIMIT);
    CHECK(suggestion_count >= 1
          && strcmp(suggestions[0].url,
                    "https://en.wikipedia.org/wiki/PSP") == 0
          && suggestions[0].bookmark);
    suggestion_count = browser_profile_suggest(
        profile, "reddit", suggestions, BROWSER_PROFILE_SUGGESTION_LIMIT);
    CHECK(suggestion_count >= 1
          && strcmp(suggestions[0].url,
                    "https://old.reddit.com/r/PSP/") == 0
          && !suggestions[0].bookmark);
    suggestion_count = browser_profile_suggest(
        profile, "HTTPS://old.reddit", suggestions,
        BROWSER_PROFILE_SUGGESTION_LIMIT);
    CHECK(suggestion_count >= 1
          && strcmp(suggestions[0].url,
                    "https://old.reddit.com/r/PSP/") == 0);
    browser_profile_set_history_enabled(profile, false);
    CHECK(browser_profile_suggest(
              profile, "reddit", suggestions,
              BROWSER_PROFILE_SUGGESTION_LIMIT) == 0);
    browser_profile_set_history_enabled(profile, true);
    CHECK(browser_profile_record_resume(
        profile, "TFTEST00001", UINT64_C(8000000), UINT64_C(19000000)));
    CHECK(!browser_profile_record_resume(
        profile, "TFTEST00001", UINT64_C(8000000), UINT64_C(19000000)));
    BrowserProfileResume resume;
    CHECK(browser_profile_resume(profile, "TFTEST00001", &resume)
          && resume.position_us == UINT64_C(8000000));

    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-profile-%ld.cfg",
             (long) getpid());
    CHECK(browser_profile_save(profile, path));
    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL && browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 2
          && browser_profile_page_font_percent(loaded) == 125
          && browser_profile_history_enabled(loaded)
          && browser_profile_restore_last_page(loaded)
          && browser_profile_tab_hibernation_enabled(loaded)
          && browser_profile_custom_homepage_enabled(loaded)
          && browser_profile_experimental_voice_input(loaded)
          && browser_profile_adaptive_voice_memory(loaded)
          && !browser_profile_analog_cursor_enabled(loaded)
          && browser_profile_persistent_cache_mb(loaded) == 4
          && browser_profile_live_cache_kib(loaded) == 4096
          && browser_profile_persist_local_storage(loaded)
          && !browser_profile_tls_session_persistence(loaded)
          && !browser_profile_javascript_enabled(loaded)
          && !browser_profile_site_javascript_enabled(
                 loaded, "https://problem.example/reloaded")
          && !browser_profile_javascript_allowed_for_url(
                 loaded, "https://other.example/")
          && !browser_profile_site_data_allowed(loaded)
          && browser_profile_search_engine(loaded)
                 == BROWSER_SEARCH_DUCKDUCKGO
          && browser_profile_color_mode(loaded)
                 == BROWSER_COLOR_MODE_DARK
          && browser_profile_chrome_theme(loaded)
                 == BROWSER_CHROME_THEME_PLUM
          && !browser_profile_wave_background(loaded)
          && browser_profile_youtube_quality(loaded)
                 == BROWSER_YOUTUBE_QUALITY_240P
          && browser_profile_youtube_compact_results(loaded)
          && browser_profile_video_scaling(loaded)
                 == BROWSER_VIDEO_SCALING_SHARP
          && !browser_profile_video_startup_buffering(loaded)
          && browser_profile_resume_offline_downloads(loaded)
          && browser_profile_text_entry_mode(loaded)
                 == BROWSER_TEXT_ENTRY_DANZEFF
          && browser_profile_content_blocker_mode(loaded)
                 == CONTENT_BLOCKER_CUSTOM
          && !browser_profile_content_blocker_cosmetic_hiding(loaded)
          && browser_profile_content_blocker_total_blocked(loaded) == 37
          && browser_profile_reader_font(loaded)
                 == BROWSER_READER_FONT_SERIF
          && !browser_profile_update_check_enabled(loaded)
          && browser_profile_update_channel(loaded)
                 == BROWSER_UPDATE_CHANNEL_BETA
          && browser_profile_glyph_language(loaded)
                 == BROWSER_GLYPH_LANGUAGE_JAPANESE
          && browser_profile_color_emoji(loaded)
          && browser_profile_update_check_last_unix(loaded)
                 == UINT64_C(1750000000)
          && browser_profile_update_check_available_sequence(loaded) == 43
          && browser_profile_remember_reader_site_scale(loaded)
          && browser_profile_reader_site_count(loaded)
                 == BROWSER_PROFILE_READER_SITE_LIMIT
          && browser_profile_reader_site_font_percent(
                 loaded, "https://de.wikipedia.org/", &reader_percent)
          && reader_percent == 150
          && browser_profile_reader_site_always(
                 loaded, "https://de.wikipedia.org/")
          && browser_profile_content_blocker_site_allowed(
                 loaded, "https://de.wikipedia.org/")
          && browser_profile_third_party_cookie_site_allowed(
                 loaded, "https://cookies.example/")
          && !browser_profile_cookie_banner_hidden(
                 loaded, "https://m.news.example/")
          && browser_profile_cookie_banner_hidden(
                 loaded, "https://other.example/")
          && browser_profile_bookmark_count(loaded) == 2
          && browser_profile_history_count(loaded) == 100
          && browser_profile_history(loaded, 0)->visits == 3
          && browser_profile_resume(loaded, "TFTEST00001", &resume));
    BrowserProfile *startup_loaded = browser_profile_create(&budget);
    BrowserProfileAllowlistImport deferred_import = {0};
    CHECK(startup_loaded != NULL
          && browser_profile_load_without_content_blocker_sites(
                 startup_loaded, path)
          && browser_profile_content_blocker_allowed_site_count(
                 startup_loaded) == 0
          && !browser_profile_cookie_banner_hidden(
                 startup_loaded, "https://m.news.example/")
          && browser_profile_import_content_blocker_allowed_sites(
                 startup_loaded, path, &deferred_import)
          && deferred_import.added == 1
          && browser_profile_content_blocker_site_allowed(
                 startup_loaded, "https://de.wikipedia.org/"));
    browser_profile_destroy(startup_loaded);
    char *html = NULL;
    size_t length = 0;
    CHECK(browser_profile_build_page(loaded, false, &html, &length)
          && html != NULL && length != 0
          && strstr(html, "A &lt;page&gt;") != NULL
          && strstr(html, "x=1&amp;y=2") != NULL);
    budget_free(&budget, html);
    html = NULL;
    length = 0;
    CHECK(browser_profile_build_homepage(loaded, &html, &length)
          && html != NULL && length != 0
          && strstr(html, "<title>My homepage</title>") != NULL
          && strstr(html, "A &lt;page&gt;") != NULL
          && strstr(html, "x=1&amp;y=2") != NULL);
    budget_free(&budget, html);
    FILE *legacy = fopen(path, "wb");
    CHECK(legacy != NULL);
    CHECK(fputs(
        "TILEFINCH_PROFILE\t1\n"
        "UI\t2\t150\t1\n"
        "MIXED\tcompat.example\n"
        "TPC\tcookies.example\n",
        legacy) >= 0);
    CHECK(fclose(legacy) == 0);
    CHECK(seal_profile(path));
    BrowserProfile *legacy_loaded = browser_profile_create(&budget);
    CHECK(legacy_loaded != NULL
          && browser_profile_load(legacy_loaded, path)
          && browser_profile_ui_scale(legacy_loaded) == 2
          && browser_profile_page_font_percent(legacy_loaded) == 150
          && browser_profile_history_enabled(legacy_loaded)
          && !browser_profile_restore_last_page(legacy_loaded)
          && !browser_profile_tab_hibernation_enabled(legacy_loaded)
          && !browser_profile_custom_homepage_enabled(legacy_loaded)
          && !browser_profile_experimental_voice_input(legacy_loaded)
          && !browser_profile_adaptive_voice_memory(legacy_loaded)
          && browser_profile_analog_cursor_enabled(legacy_loaded)
          && browser_profile_persistent_cache_mb(legacy_loaded) == 0
          && browser_profile_live_cache_kib(legacy_loaded) == 512
          && !browser_profile_persist_local_storage(legacy_loaded)
          && browser_profile_tls_session_persistence(legacy_loaded)
          && browser_profile_javascript_enabled(legacy_loaded)
          && browser_profile_site_javascript_enabled(
                 legacy_loaded, "https://problem.example/")
          && browser_profile_site_data_allowed(legacy_loaded)
          && browser_profile_search_engine(legacy_loaded)
                 == BROWSER_SEARCH_GOOGLE
          && browser_profile_color_mode(legacy_loaded)
                 == BROWSER_COLOR_MODE_AUTO
          && browser_profile_chrome_theme(legacy_loaded)
                 == BROWSER_CHROME_THEME_FINCH
          /* A file written before the WAVE record keeps the default. */
          && browser_profile_wave_background(legacy_loaded)
          && browser_profile_youtube_quality(legacy_loaded)
                 == BROWSER_YOUTUBE_QUALITY_360P
          && !browser_profile_youtube_compact_results(legacy_loaded)
          && browser_profile_video_scaling(legacy_loaded)
                 == BROWSER_VIDEO_SCALING_SMOOTH
          && browser_profile_video_startup_buffering(legacy_loaded)
          && !browser_profile_resume_offline_downloads(legacy_loaded)
          && browser_profile_text_entry_mode(legacy_loaded)
                 == BROWSER_TEXT_ENTRY_OSK
          && browser_profile_content_blocker_mode(legacy_loaded)
                 == CONTENT_BLOCKER_BASIC
          && browser_profile_content_blocker_cosmetic_hiding(legacy_loaded)
          && browser_profile_content_blocker_total_blocked(legacy_loaded) == 0
          && browser_profile_reader_font(legacy_loaded)
                 == BROWSER_READER_FONT_SANS
          && !browser_profile_remember_reader_site_scale(legacy_loaded)
          && browser_profile_update_check_enabled(legacy_loaded)
          && browser_profile_update_channel(legacy_loaded)
                 == BROWSER_UPDATE_CHANNEL_STABLE
          && browser_profile_glyph_language(legacy_loaded)
                 == BROWSER_GLYPH_LANGUAGE_EMBEDDED
          && !browser_profile_color_emoji(legacy_loaded)
          && browser_profile_update_check_last_unix(legacy_loaded) == 0
          && browser_profile_update_check_available_sequence(
                 legacy_loaded) == 0
          && browser_profile_content_blocker_allowed_site_count(legacy_loaded)
                 == 0
          && browser_profile_cookie_banner_hidden(
                 legacy_loaded, "https://news.example/")
          && browser_profile_third_party_cookie_site_allowed(
                 legacy_loaded, "https://cookies.example/"));
    /* MIXED was durable in older releases. Loading and re-saving an old
       profile must retire that authority while preserving unrelated durable
       compatibility settings such as the third-party-cookie exception. */
    CHECK(browser_profile_save(legacy_loaded, path)
          && !file_contains(path, "MIXED\t")
          && file_contains(path, "TPC\tcookies.example"));
    browser_profile_destroy(legacy_loaded);

    /*
     * A MEDIA record written by the build before video scaling existed: two
     * fields, not three. Both of its settings must survive, and the absent
     * one must read as Smooth, which is the default rather than a zero that
     * happens to look like one.
     */
    FILE *two_field_media = fopen(path, "wb");
    CHECK(two_field_media != NULL);
    CHECK(fputs(
        "TILEFINCH_PROFILE\t1\nMEDIA\t240\t1\n",
        two_field_media) >= 0);
    CHECK(fclose(two_field_media) == 0);
    CHECK(seal_profile(path));
    BrowserProfile *two_field_loaded = browser_profile_create(&budget);
    CHECK(two_field_loaded != NULL
          && browser_profile_load(two_field_loaded, path)
          && browser_profile_youtube_quality(two_field_loaded)
                 == BROWSER_YOUTUBE_QUALITY_240P
          && !browser_profile_youtube_compact_results(two_field_loaded)
          && browser_profile_resume_offline_downloads(two_field_loaded)
          && browser_profile_video_scaling(two_field_loaded)
                 == BROWSER_VIDEO_SCALING_SMOOTH
          && browser_profile_video_startup_buffering(two_field_loaded));
    browser_profile_destroy(two_field_loaded);

    FILE *forward = fopen(path, "wb");
    CHECK(forward != NULL);
    CHECK(fputs(
        "TILEFINCH_PROFILE\t1\n"
        "UI\t2\t125\t1\t2\t1\t1\t0\t2\t1\t1\t77\tfuture\n"
        "DATA\t4\t1\t1024\tfuture\n"
        "BLOCK\t0\n"
        "UPDCHK\t0\t123\t7\tfuture\n"
        "FUTURE\tignored\twithout\tshifting\n",
        forward) >= 0);
    CHECK(fclose(forward) == 0);
    CHECK(seal_profile(path));
    BrowserProfile *forward_loaded = browser_profile_create(&budget);
    CHECK(forward_loaded != NULL
          && browser_profile_load(forward_loaded, path)
          && browser_profile_ui_scale(forward_loaded) == 2
          && browser_profile_page_font_percent(forward_loaded) == 125
          && browser_profile_history_enabled(forward_loaded)
          && browser_profile_search_engine(forward_loaded)
                 == BROWSER_SEARCH_DUCKDUCKGO
          && browser_profile_restore_last_page(forward_loaded)
          && !browser_profile_tab_hibernation_enabled(forward_loaded)
          && browser_profile_adaptive_voice_memory(forward_loaded)
          && !browser_profile_analog_cursor_enabled(forward_loaded)
          && browser_profile_color_mode(forward_loaded)
                 == BROWSER_COLOR_MODE_DARK
          && browser_profile_custom_homepage_enabled(forward_loaded)
          && browser_profile_experimental_voice_input(forward_loaded)
          && browser_profile_persistent_cache_mb(forward_loaded) == 4
          && browser_profile_live_cache_kib(forward_loaded) == 1024
          && browser_profile_persist_local_storage(forward_loaded));
    CHECK(browser_profile_text_entry_mode(forward_loaded)
              == BROWSER_TEXT_ENTRY_OSK
          && browser_profile_content_blocker_mode(forward_loaded)
                 == CONTENT_BLOCKER_OFF
          && browser_profile_content_blocker_cosmetic_hiding(
                 forward_loaded));
    CHECK(!browser_profile_update_check_enabled(forward_loaded)
          && browser_profile_update_channel(forward_loaded)
                 == BROWSER_UPDATE_CHANNEL_STABLE
          && browser_profile_update_check_last_unix(forward_loaded) == 123
          && browser_profile_update_check_available_sequence(
                 forward_loaded) == 7);
    browser_profile_destroy(forward_loaded);

    /* Background update-check admission: enabled + valid clock + at
       least 3.5 days since the last completed check; a future-recorded
       check resets the cadence instead of blocking it. */
    BrowserProfile *cadence = browser_profile_create(&budget);
    CHECK(cadence != NULL);
    CHECK(!browser_profile_update_check_due(cadence, 0));
    CHECK(browser_profile_update_check_due(cadence, UINT64_C(1000)));
    browser_profile_set_update_check_last_unix(
        cadence, UINT64_C(1000000));
    CHECK(!browser_profile_update_check_due(cadence, UINT64_C(1000000)));
    CHECK(!browser_profile_update_check_due(
        cadence, UINT64_C(1000000) + UINT64_C(302399)));
    CHECK(browser_profile_update_check_due(
        cadence, UINT64_C(1000000)
                     + BROWSER_PROFILE_UPDATE_CHECK_INTERVAL_SECONDS));
    CHECK(browser_profile_update_check_due(cadence, UINT64_C(999999)));
    browser_profile_set_update_check_enabled(cadence, false);
    CHECK(!browser_profile_update_check_due(
        cadence, UINT64_C(1000000) + UINT64_C(604800)));
    browser_profile_destroy(cadence);
    CHECK(browser_profile_update_check_due(NULL, UINT64_C(1000)));
    CHECK(!browser_profile_update_check_due(NULL, 0));

    FILE *allowlist = fopen(path, "wb");
    CHECK(allowlist != NULL);
    CHECK(fputs("! import test\nhttps://one.example/path\n", allowlist)
          >= 0);
    for (unsigned i = 0; i < CONTENT_BLOCKER_ALLOW_SITE_LIMIT + 2u; i++)
        CHECK(fprintf(allowlist, "allow%u.example\n", i) > 0);
    CHECK(fclose(allowlist) == 0);
    BrowserProfile *imported = browser_profile_create(&budget);
    BrowserProfileAllowlistImport import_result = {0};
    CHECK(imported != NULL
          && browser_profile_import_content_blocker_allowed_sites(
                 imported, path, &import_result)
          && browser_profile_content_blocker_allowed_site_count(imported)
                 == CONTENT_BLOCKER_ALLOW_SITE_LIMIT
          && import_result.added == CONTENT_BLOCKER_ALLOW_SITE_LIMIT
          && import_result.resident_full
          && browser_profile_content_blocker_site_allowed(
                 imported, "https://one.example/elsewhere"));
    browser_profile_destroy(imported);

    /* Backup rotation: the profile store must never leave the stick with no
       readable generation, because on FAT the publish is remove + rename. */
    char torn_path[160], torn_backup[192];
    snprintf(torn_path, sizeof(torn_path),
             "/tmp/tilefinch-profile-torn-%ld.cfg", (long) getpid());
    snprintf(torn_backup, sizeof(torn_backup), "%s.bak", torn_path);
    remove(torn_path);
    remove(torn_backup);
    /* The first publish has nothing to rotate. */
    CHECK(browser_profile_save(profile, torn_path));
    CHECK(file_present(torn_path) && !file_present(torn_backup));
    /* Every later publish rotates the live file aside first. */
    CHECK(browser_profile_save(profile, torn_path));
    CHECK(file_present(torn_path) && file_present(torn_backup));
    /* Power cut inside the publish window: the primary is gone. */
    CHECK(remove(torn_path) == 0);
    BrowserProfile *torn = browser_profile_create(&budget);
    CHECK(torn != NULL && browser_profile_load(torn, torn_path)
          && browser_profile_ui_scale(torn) == 2
          && browser_profile_page_font_percent(torn) == 125
          && browser_profile_history_enabled(torn)
          && browser_profile_restore_last_page(torn)
          && browser_profile_search_engine(torn)
                 == BROWSER_SEARCH_DUCKDUCKGO
          && browser_profile_chrome_theme(torn)
                 == BROWSER_CHROME_THEME_PLUM
          && browser_profile_bookmark_count(torn) == 2
          && browser_profile_history_count(torn) == 100);
    browser_profile_destroy(torn);
    /* A primary that survives but does not parse falls back too, and is
       dropped so the next rotation cannot promote it over the backup. */
    FILE *corrupt_primary = fopen(torn_path, "wb");
    CHECK(corrupt_primary != NULL
          && fputs("\x01\x02 not a profile\n", corrupt_primary) >= 0
          && fclose(corrupt_primary) == 0);
    BrowserProfile *recovered = browser_profile_create(&budget);
    CHECK(recovered != NULL && browser_profile_load(recovered, torn_path)
          && browser_profile_ui_scale(recovered) == 2
          && browser_profile_page_font_percent(recovered) == 125
          && browser_profile_bookmark_count(recovered) == 2
          && browser_profile_history_count(recovered) == 100);
    CHECK(!file_present(torn_path));
    browser_profile_destroy(recovered);
    /* A syntactically valid prefix without its terminating newline is a torn
       primary too. It must not be accepted with defaulted-away tail records
       while a complete backup is available. */
    FILE *truncated_primary = fopen(torn_path, "wb");
    CHECK(truncated_primary != NULL
          && fputs("TILEFINCH_PROFILE\t1\nUI\t1\t100", truncated_primary)
                 >= 0
          && fclose(truncated_primary) == 0);
    BrowserProfile *truncated = browser_profile_create(&budget);
    CHECK(truncated != NULL && browser_profile_load(truncated, torn_path)
          && browser_profile_ui_scale(truncated) == 2
          && browser_profile_page_font_percent(truncated) == 125
          && browser_profile_history_count(truncated) == 100
          && !file_present(torn_path));
    browser_profile_destroy(truncated);
    /* Even a newline-aligned prefix is incomplete without the authenticated
       end marker; fall back instead of re-enabling omitted tail policies. */
    FILE *aligned_primary = fopen(torn_path, "wb");
    CHECK(aligned_primary != NULL
          && fputs("TILEFINCH_PROFILE\t1\nUI\t1\t100\t0\n",
                   aligned_primary) >= 0
          && fclose(aligned_primary) == 0);
    BrowserProfile *aligned = browser_profile_create(&budget);
    CHECK(aligned != NULL && browser_profile_load(aligned, torn_path)
          && browser_profile_ui_scale(aligned) == 2
          && browser_profile_page_font_percent(aligned) == 125
          && browser_profile_javascript_enabled(aligned) == false
          && !file_present(torn_path));
    browser_profile_destroy(aligned);
    /* With both generations gone the load fails instead of inventing one. */
    CHECK(remove(torn_backup) == 0);
    BrowserProfile *absent = browser_profile_create(&budget);
    CHECK(absent != NULL && !browser_profile_load(absent, torn_path)
          && !browser_profile_load_without_content_blocker_sites(
                 absent, torn_path));
    browser_profile_destroy(absent);

    char recovery_path[160];
    snprintf(recovery_path, sizeof(recovery_path), "%s.recovery", path);
    CHECK(!browser_recovery_save(
              recovery_path, "file:/not-a-page", 4));
    CHECK(browser_recovery_save(
              recovery_path, "https://example.test/path?q=1", 1234));
    BrowserRecoveryCheckpoint checkpoint = {0};
    CHECK(browser_recovery_load(recovery_path, &checkpoint)
          && strcmp(
                 checkpoint.url,
                 "https://example.test/path?q=1") == 0
          && checkpoint.scroll_y == 1234);
    FILE *corrupt = fopen(recovery_path, "wb");
    CHECK(corrupt != NULL
          && fputs(
                 "TILEFINCH_RECOVERY\t1\n"
                 "N\thttps://example.test/path\t1234\t00000000\n",
                 corrupt) >= 0
          && fclose(corrupt) == 0);
    CHECK(!browser_recovery_load(recovery_path, &checkpoint)
          && checkpoint.url[0] == '\0');
    CHECK(browser_recovery_clear(recovery_path));

    remove(path);
    CHECK(browser_profile_set_reader_site_always(
        loaded, "https://wikipedia.org/", false));
    browser_profile_set_remember_reader_site_scale(loaded, false);
    CHECK(!browser_profile_remember_reader_site_scale(loaded)
          && browser_profile_reader_site_count(loaded) == 0
          && !browser_profile_reader_site_font_percent(
                 loaded, "https://wikipedia.org/", &reader_percent));
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0);
    puts("browser-profile-tests: ok");
    return 0;
}
