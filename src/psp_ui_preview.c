#include "tilefinch/psp_ui.h"
#include "tilefinch/budget.h"
#include "tilefinch/build_version.h"
#include "tilefinch/glyph_component_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_WIDTH 480
#define PREVIEW_HEIGHT 272

static uint16_t preview_rgb565(unsigned red, unsigned green, unsigned blue)
{
    return (uint16_t) (((red * 31u / 255u) << 11)
                       | ((green * 63u / 255u) << 5)
                       | (blue * 31u / 255u));
}

static void preview_rect(uint16_t *frame, int x, int y, int width, int height,
                         uint16_t color)
{
    for (int row = y; row < y + height && row < PREVIEW_HEIGHT; row++) {
        if (row < 0) continue;
        for (int column = x; column < x + width && column < PREVIEW_WIDTH;
             column++) {
            if (column >= 0) {
                frame[(size_t) row * PREVIEW_WIDTH + (size_t) column] = color;
            }
        }
    }
}

/* Every mode that ends by compositing the media overlay over the page. */
static bool preview_media_mode(const char *mode)
{
    return strcmp(mode, "media") == 0
        || strcmp(mode, "media-playing") == 0
        || strcmp(mode, "media-playing-bbb") == 0
        || strcmp(mode, "media-loading") == 0
        || strcmp(mode, "seek") == 0
        || strcmp(mode, "media-error") == 0
        || strcmp(mode, "media-retry") == 0;
}

static bool write_ppm(const char *path, const uint16_t *frame)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file, "P6\n%d %d\n255\n", PREVIEW_WIDTH, PREVIEW_HEIGHT);
    for (size_t at = 0; at < PREVIEW_WIDTH * PREVIEW_HEIGHT; at++) {
        uint16_t pixel = frame[at];
        unsigned char rgb[3] = {
            (unsigned char) (((pixel >> 11) & 31u) * 255u / 31u),
            (unsigned char) (((pixel >> 5) & 63u) * 255u / 63u),
            (unsigned char) ((pixel & 31u) * 255u / 31u)
        };
        if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}

static bool read_ppm(const char *path, uint16_t *frame)
{
    if (path == NULL || frame == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    int width = 0;
    int height = 0;
    int maximum = 0;
    bool valid = fscanf(file, "P6\n%d %d\n%d\n", &width, &height, &maximum)
                     == 3
        && width == PREVIEW_WIDTH && height == PREVIEW_HEIGHT
        && maximum == 255;
    for (size_t at = 0; valid && at < PREVIEW_WIDTH * PREVIEW_HEIGHT; at++) {
        unsigned char rgb[3];
        valid = fread(rgb, 1, sizeof(rgb), file) == sizeof(rgb);
        if (valid) {
            frame[at] = preview_rgb565(rgb[0], rgb[1], rgb[2]);
        }
    }
    if (valid) valid = fgetc(file) == EOF;
    return fclose(file) == 0 && valid;
}

int main(int argc, char **argv)
{
    const char *output = argc > 1 ? argv[1] : "psp-ui-preview.ppm";
    const char *mode = argc > 2 ? argv[2] : "";
    const char *page_frame = argc > 3 ? argv[3] : NULL;
    uint16_t *frame = calloc(PREVIEW_WIDTH * PREVIEW_HEIGHT, sizeof(*frame));
    if (frame == NULL) return 1;

    Budget font_budget;
    FontSet fonts;
    budget_init(&font_budget, 2u * 1024u * 1024u);
    memset(&fonts, 0, sizeof(fonts));
    bool fonts_ready = font_set_load(
        &fonts, &font_budget,
        "fonts/DejaVuSans-Latin.ttf", "fonts/DejaVuSerif-Latin.ttf",
        "fonts/DejaVuSans-Oblique-Latin.ttf",
        "fonts/DejaVuSans-Bold-Latin.ttf",
        "fonts/DejaVuSerif-Bold-Latin.ttf",
        "fonts/TilefinchSans-Regular.ttf",
        "fonts/TilefinchSans-Bold.ttf", 1536u * 1024u);
    /*
     * The boot entrance's keyframes, so the choreography is reviewable as
     * stills: mark-centred (frame one), wave-up, mid-glide, and the
     * slow-branch state that holds the mark and shows one muted line.
     * Fonts are loaded first because the glide keyframe draws real HOME.
     */
    if (strncmp(mode, "boot-entrance", 13) == 0) {
        if (fonts_ready)
            psp_ui_set_chrome_fonts(
                font_set_face(&fonts, FONT_SANS),
                font_set_face_variant(&fonts, FONT_SANS, false, true));
        PspUiHomeView entrance_home = {
            .tile_count = 6,
            .continue_count = 3,
            .engine_ready = true,
            .tiles = {
                {"WIKIPEDIA", "Articles and search"},
                {"YOUTUBE", "Browse and play video"},
                {"ARS TECHNICA", "arstechnica.com"},
                {"HACKER NEWS", "news.ycombinator.com"},
                {"LOBSTERS", "lobste.rs"},
                {"MDN", "developer.mozilla.org"}
            },
            .continues = {
                {"PlayStation Portable", "en.wikipedia.org"},
                {"Fixture video", "youtube.com"},
                {"Show HN: a PSP browser", "news.ycombinator.com"}
            }
        };
        PspUiState entrance_ui;
        psp_ui_init(&entrance_ui);
        psp_ui_set_home(&entrance_ui, &entrance_home);
        psp_ui_show_home(&entrance_ui);
        /* At boot the link is not yet READY, so the status line shows no
           wifi bars -- the absent state, captured alongside the rest. */
        psp_ui_set_device_status(14, 8, 62, false, false, -1);
        PspUiBootEntranceView view = {
            .frame = 0, .wave = true, .branch_status = NULL
        };
        if (strncmp(mode, "boot-entrance-", 14) == 0) {
            char *end = NULL;
            unsigned long numbered = strtoul(mode + 14, &end, 10);
            if (end != mode + 14 && *end == '\0'
                && numbered <= PSP_UI_BOOT_ENTRANCE_FRAMES)
                view.frame = (unsigned) numbered;
        }
        if (strcmp(mode, "boot-entrance-wave") == 0) view.frame = 7;
        else if (strcmp(mode, "boot-entrance-glide") == 0) view.frame = 13;
        else if (strcmp(mode, "boot-entrance-settled") == 0)
            view.frame = PSP_UI_BOOT_ENTRANCE_FRAMES;
        else if (strcmp(mode, "boot-entrance-slow") == 0)
            view.branch_status = "READING SETTINGS";
        psp_ui_boot_entrance_composite(
            &view, &entrance_ui, frame, PREVIEW_WIDTH, PREVIEW_HEIGHT,
            PREVIEW_WIDTH);
        bool written = write_ppm(output, frame);
        printf("psp-ui-preview: output=%s state-bytes=%zu\n",
               output, psp_ui_state_bytes());
        psp_ui_clear_chrome_font();
        if (fonts_ready) font_set_destroy(&fonts);
        free(frame);
        return written ? 0 : 1;
    }

    if (strcmp(mode, "startup") == 0 || strcmp(mode, "skeleton") == 0) {
        psp_ui_startup_composite(
            strcmp(mode, "skeleton") == 0
                ? PSP_UI_STARTUP_HOMEPAGE : PSP_UI_STARTUP_SPLASH,
            strcmp(mode, "skeleton") == 0
                ? "PREPARING HOME" : "STARTING BROWSER",
            strcmp(mode, "skeleton") == 0 ? 620 : 80,
            frame, PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_WIDTH);
        bool written = write_ppm(output, frame);
        printf("psp-ui-preview: output=%s state-bytes=%zu\n",
               output, psp_ui_state_bytes());
        psp_ui_clear_chrome_font();
        if (fonts_ready) font_set_destroy(&fonts);
        free(frame);
        return written ? 0 : 1;
    }

    if (fonts_ready)
        psp_ui_set_chrome_fonts(
            font_set_face(&fonts, FONT_SANS),
            font_set_face_variant(&fonts, FONT_SANS, false, true));

    for (int y = 0; y < PREVIEW_HEIGHT; y++) {
        uint16_t background = preview_rgb565(
            247u - (unsigned) y / 20u,
            249u - (unsigned) y / 25u,
            250u - (unsigned) y / 30u);
        for (int x = 0; x < PREVIEW_WIDTH; x++) {
            frame[(size_t) y * PREVIEW_WIDTH + (size_t) x] = background;
        }
    }
    preview_rect(frame, 18, 54, 310, 14, preview_rgb565(24, 49, 83));
    preview_rect(frame, 18, 77, 425, 5, preview_rgb565(164, 174, 183));
    preview_rect(frame, 18, 89, 390, 5, preview_rgb565(183, 191, 198));
    preview_rect(frame, 18, 101, 418, 5, preview_rgb565(183, 191, 198));
    preview_rect(frame, 18, 122, 164, 86, preview_rgb565(211, 222, 227));
    preview_rect(frame, 198, 122, 245, 6, preview_rgb565(24, 49, 83));
    for (int y = 139; y < 210; y += 13) {
        preview_rect(frame, 198, y, 230 - (y % 3) * 17, 4,
                     preview_rgb565(176, 185, 192));
    }
    if (page_frame != NULL && !read_ppm(page_frame, frame)) {
        fprintf(stderr, "could not read 480x272 P6 page frame: %s\n",
                page_frame);
        psp_ui_clear_chrome_font();
        if (fonts_ready) font_set_destroy(&fonts);
        free(frame);
        return 1;
    }

    PspUiState ui;
    psp_ui_init(&ui);
    TilefinchDiagnosticQrReport *diagnostic_report = NULL;
    psp_ui_set_page(&ui, "PlayStation Portable - Wikipedia",
                    "https://en.wikipedia.org/wiki/PlayStation_Portable",
                    true);
    psp_ui_set_history(&ui, true, false);
    psp_ui_set_scroll(&ui, 684, 4217);
    psp_ui_set_focus(&ui, true, 196, 119, 128, 14);
    psp_ui_show_status(&ui, "PAGE READY - 42 LINKS", 180);
    PspUiTabsView tabs = {
        .count = 5,
        .active_index = 0
    };
    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "PlayStation Portable");
    snprintf(tabs.titles[1], sizeof(tabs.titles[1]), "YouTube");
    snprintf(tabs.titles[2], sizeof(tabs.titles[2]), "Hacker News");
    snprintf(tabs.titles[3], sizeof(tabs.titles[3]), "Reddit");
    snprintf(tabs.titles[4], sizeof(tabs.titles[4]), "NYTimes");
    snprintf(tabs.domains[0], sizeof(tabs.domains[0]), "en.wikipedia.org");
    snprintf(tabs.domains[1], sizeof(tabs.domains[1]), "youtube.com");
    snprintf(tabs.domains[2], sizeof(tabs.domains[2]), "news.ycombinator.com");
    snprintf(tabs.domains[3], sizeof(tabs.domains[3]), "reddit.com");
    snprintf(tabs.domains[4], sizeof(tabs.domains[4]), "nytimes.com");
    psp_ui_set_tabs(&ui, &tabs);
    BrowserProfileSuggestion text_suggestions[3] = {
        {.url = "https://en.wikipedia.org/",
         .title = "Wikipedia, the free encyclopedia", .bookmark = true},
        {.url = "https://en.wikipedia.org/wiki/PSP",
         .title = "PlayStation Portable - Wikipedia", .bookmark = false},
        {.url = "https://www.wikipedia.org/",
         .title = "Wikipedia language portal", .bookmark = false}
    };
    PspUiTextEntryView text_entry = {
        .description = "SEARCH OR ADDRESS",
        .text = "wiki",
        .suggestions = text_suggestions,
        .cursor = 4,
        .suggestion_count = 3,
        .suggestion_selection = 0,
        .cell = 0,
        .allow_submit = true
    };
    PspUiFindView find_view = {
        .query = "portable",
        .match_count = 12,
        .selected = 3
    };
    /*
     * Fixture only. The shipped empty-profile default is the two builtin
     * tiles (see psp_home_sync_ui); the rest exist here to fill the 3x2
     * grid so truncation, gutters and focus can be reviewed. There is no
     * SEARCH tile -- Start opens the omnibox and the hint line says so.
     */
    PspUiHomeView home_view = {
        .tile_count = 6,
        .continue_count = 3,
        .engine_ready = true,
        .tiles = {
            {"WIKIPEDIA", "Articles and search"},
            {"YOUTUBE", "Browse and play video"},
            {"ARS TECHNICA", "arstechnica.com"},
            {"HACKER NEWS", "news.ycombinator.com"},
            {"LOBSTERS", "lobste.rs"},
            {"MDN", "developer.mozilla.org"}
        },
        .continues = {
            {"PlayStation Portable", "en.wikipedia.org"},
            {"Fixture video", "youtube.com"},
            {"Show HN: a PSP browser", "news.ycombinator.com"}
        }
    };
    PspUiCollectionsView collections_view = {
        .section = PSP_UI_COLLECTION_OFFLINE,
        .count = 4,
        .empty_message = "NOTHING SAVED FOR OFFLINE YET",
        .rows = {
            {"PlayStation Portable", "en.wikipedia.org", "184 KB", true},
            {"Fixture video", "youtube.com", "6.2 MB", true},
            {"Ambient computing", "lobste.rs", "QUEUED", true},
            {"The lost art of C", "news.ycombinator.com", "92 KB", true}
        }
    };
    /*
     * The three sections do not share a row shape, so they cannot share a
     * fixture: reusing the offline rows put byte sizes on bookmarks and
     * offered SQUARE DELETE on a section where deletion is not legal.
     * These mirror psp_collections_sync_ui: bookmarks carry no trailing
     * detail and are not deletable (add/remove is a page verb in the
     * menu); history carries a visit count only when there is more than
     * one visit, and is deletable.
     */
    PspUiCollectionsView bookmarks_view = {
        .section = PSP_UI_COLLECTION_BOOKMARKS,
        .count = 4,
        .empty_message = "NO BOOKMARKS YET",
        .rows = {
            {"Wikipedia, the free encyclopedia",
             "https://en.wikipedia.org/", "", false},
            {"Hacker News", "https://news.ycombinator.com/", "", false},
            {"MDN Web Docs", "https://developer.mozilla.org/", "", false},
            {"Lobsters", "https://lobste.rs/", "", false}
        }
    };
    PspUiCollectionsView history_view = {
        .section = PSP_UI_COLLECTION_HISTORY,
        .count = 4,
        .empty_message = "NO HISTORY YET",
        .rows = {
            {"PlayStation Portable - Wikipedia",
             "https://en.wikipedia.org/wiki/PlayStation_Portable",
             "7 VISITS", true},
            {"Fixture video",
             "https://www.youtube.com/watch?v=TFTEST00001", "", true},
            {"Show HN: a PSP browser",
             "https://news.ycombinator.com/item?id=1", "2 VISITS", true},
            {"The lost art of C", "https://lobste.rs/s/c", "", true}
        }
    };
    if (strcmp(mode, "menu") == 0) {
        ui.screen = PSP_UI_SCREEN_MENU;
        ui.menu_selection = 3;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "page-tools") == 0) {
        ui.screen = PSP_UI_SCREEN_PAGE_TOOLS;
        ui.menu_selection = 5;
        ui.reader_mode = true;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "site-controls") == 0) {
        ui.screen = PSP_UI_SCREEN_SITE_CONTROLS;
        ui.menu_selection = 1;
        ui.site_javascript_enabled = true;
        ui.reader_site_always = true;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "help") == 0) {
        ui.screen = PSP_UI_SCREEN_HELP;
        ui.menu_selection = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "options") == 0) {
        ui.screen = PSP_UI_SCREEN_OPTIONS;
        ui.options_group_selection = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "browsing") == 0) {
        ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
        ui.options_selection = 9;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "appearance") == 0
               || strcmp(mode, "options-ocean") == 0
               || strcmp(mode, "options-plum") == 0
               || strcmp(mode, "options-ember") == 0) {
        ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
        ui.options_selection = strcmp(mode, "appearance") == 0 ? 0 : 15;
        if (strcmp(mode, "options-ocean") == 0)
            ui.chrome_theme = BROWSER_CHROME_THEME_OCEAN;
        else if (strcmp(mode, "options-plum") == 0)
            ui.chrome_theme = BROWSER_CHROME_THEME_PLUM;
        else if (strcmp(mode, "options-ember") == 0)
            ui.chrome_theme = BROWSER_CHROME_THEME_EMBER;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "glyph-options") == 0) {
        ui.screen = PSP_UI_SCREEN_GLYPH_OPTIONS;
        ui.glyph_language = BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED;
        ui.glyph_options_selection = 0;
        ui.glyph_installed_mask =
            (uint8_t) (1u << TILEFINCH_GLYPH_PACK_LATIN_EXTENDED);
        ui.toast_frames = 0;
    } else if (strcmp(mode, "adblock") == 0) {
        ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
        ui.options_selection = 22;
        ui.content_blocker_mode = CONTENT_BLOCKER_BASIC;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "data") == 0) {
        ui.screen = PSP_UI_SCREEN_DATA_OPTIONS;
        ui.data_options_selection = 6;
        ui.live_cache_kib = 1024;
        ui.persistent_cache_mb = 4;
        ui.persist_local_storage = true;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "experimental") == 0) {
        ui.screen = PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS;
        ui.experimental_voice_input = false;
        ui.experimental_options_selection = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "tabs") == 0) {
        ui.screen = PSP_UI_SCREEN_TABS;
        ui.tab_selection = 1;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "home") == 0
               || strcmp(mode, "home-12h") == 0) {
        /*
         * A fixed wall clock so the render is deterministic. On device the
         * 12/24-hour choice comes from
         * PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT; "home-12h" pins the 12-hour
         * branch so the wider "2:08 PM" string can be reviewed against the
         * battery block.
         */
        /* A deterministic three-of-four bars so the wifi cluster renders
           the same every capture. */
        psp_ui_set_device_status(14, 8, 62, false,
                                 strcmp(mode, "home-12h") == 0, 3);
        psp_ui_set_home(&ui, &home_view);
        psp_ui_show_home(&ui);
        ui.home_selection = 1;
        ui.overlay_animation_frames = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "collections") == 0
               || strcmp(mode, "collections-bookmarks") == 0
               || strcmp(mode, "collections-history") == 0) {
        psp_ui_set_device_status(14, 8, 62, false, false, 3);
        PspUiCollectionsView *section_view =
            strcmp(mode, "collections-bookmarks") == 0 ? &bookmarks_view
            : (strcmp(mode, "collections-history") == 0 ? &history_view
                                                        : &collections_view);
        psp_ui_show_collections(&ui, section_view->section);
        psp_ui_set_collections(&ui, section_view);
        ui.collections_selection = 1;
        ui.overlay_animation_frames = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "text-entry") == 0) {
        psp_ui_set_text_entry(&ui, &text_entry);
        /* Composite the at-rest layout. The preview draws exactly one
           frame and never calls psp_ui_update, so without this the panel
           is captured at its full slide-in offset and the legend lines
           fall off the bottom of the frame. */
        ui.overlay_animation_frames = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "find") == 0) {
        psp_ui_set_find(&ui, &find_view);
        ui.overlay_animation_frames = 0;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "update") == 0) {
        /* The update panel had no preview mode, so its progress track and
           its primary button had never been looked at. */
        ui.screen = PSP_UI_SCREEN_UPDATE;
        psp_ui_set_update(
            &ui, "1.4.2", "A new version is ready",
            "Faster page loads, fewer stalls", 620, "Install and restart",
            true, true);
        ui.toast_frames = 0;
    } else if (strcmp(mode, "update-versions") == 0) {
        ui.screen = PSP_UI_SCREEN_UPDATE_VERSIONS;
        TilefinchUpdateHistorySnapshot history = {
            .phase = TILEFINCH_UPDATE_HISTORY_READY,
            .count = 8u,
            .versions = {
                "0.1.7", "0.1.6", "0.1.5", "0.1.4",
                "0.1.3", "0.1.2", "0.1.1", "0.1.0"
            }
        };
        psp_ui_set_update_history(&ui, &history);
        ui.data_options_selection = 7u;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "diagnostic-qr") == 0) {
        char source_path[1024];
        int path_length = snprintf(
            source_path, sizeof(source_path), "%s.diagnostic-input", output);
        static const char log[] =
            "tilefinch-device-error-v2\n"
            "version=" TILEFINCH_VERSION_STRING "\n"
            "stage=navigation\n"
            "http=0\n"
            "native=0x80110601\n"
            "detail=network-profile\n";
        FILE *source = path_length > 0
                && (size_t) path_length < sizeof(source_path)
            ? fopen(source_path, "wb") : NULL;
        bool source_ready = false;
        if (source != NULL) {
            bool wrote = fwrite(log, 1u, sizeof(log) - 1u, source)
                == sizeof(log) - 1u;
            source_ready = fclose(source) == 0 && wrote;
        }
        if (!source_ready) {
            fprintf(stderr, "could not stage diagnostic preview input\n");
            psp_ui_clear_chrome_font();
            if (fonts_ready) font_set_destroy(&fonts);
            free(frame);
            return 1;
        }
        TilefinchDiagnosticSource diagnostic_source = {
            "tilefinch-last-error.txt", source_path
        };
        TilefinchDiagnosticMetadata diagnostic_metadata = {
            .app_version = TILEFINCH_VERSION_STRING,
            .release_sequence = 1u,
            .created_unix_time = 1787006559u,
            .psp_model = 2u,
            .psp_firmware = UINT32_C(0x06060110)
        };
        char diagnostic_error[96];
        diagnostic_report = tilefinch_diagnostic_qr_build(
            &diagnostic_metadata, &diagnostic_source, 1u,
            diagnostic_error, sizeof(diagnostic_error));
        (void) remove(source_path);
        if (diagnostic_report == NULL) {
            fprintf(stderr, "could not build diagnostic preview: %s\n",
                    diagnostic_error);
            psp_ui_clear_chrome_font();
            if (fonts_ready) font_set_destroy(&fonts);
            free(frame);
            return 1;
        }
        ui.screen = PSP_UI_SCREEN_DIAGNOSTIC_QR;
        psp_ui_set_diagnostic_qr(
            &ui, tilefinch_diagnostic_qr_view(diagnostic_report));
        ui.toast_frames = 0;
    } else if (strcmp(mode, "loading") == 0) {
        /* The chrome bar's edge and its progress band, over the light page
           skeleton -- the case the user actually stares at. */
        psp_ui_set_loading(&ui, true, 380);
        ui.loading_phase = 46u;
        ui.toast_frames = 0;
    } else if (strcmp(mode, "page-capture") == 0
               || strcmp(mode, "reader-capture") == 0) {
        /* README/documentation captures feed a real engine frame through
           this harness. Keep the ordinary native chrome, but suppress
           fixture-only focus and toast state that is not part of the page. */
        ui.reader_mode = strcmp(mode, "reader-capture") == 0;
        ui.toast_frames = 0;
        psp_ui_set_focus(&ui, false, 0, 0, 0, 0);
    } else if (strcmp(mode, "large") == 0) {
        ui.browser_ui_scale = 2;
    } else if (preview_media_mode(mode)) {
        ui.chrome_visible = false;
        ui.toast_frames = 0;
    }
    if (strcmp(mode, "night") == 0) {
        ui.color_mode = BROWSER_COLOR_MODE_DARK;
        ui.page_dark = true;
        psp_ui_apply_page_dark_rgb565(
            frame, PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_WIDTH);
    }
    /* Match psp_present_internal: fullscreen media owns the panel, so none
       of the page compositor's focus, cursor, scrollbar, or chrome may be
       baked underneath the player. */
    if (!preview_media_mode(mode)) {
        psp_ui_composite(&ui, frame, PREVIEW_WIDTH, PREVIEW_HEIGHT,
                         PREVIEW_WIDTH);
    }
    if (preview_media_mode(mode)) {
        PspUiMediaState media;
        psp_ui_media_init(&media);
        bool big_buck_bunny = strcmp(mode, "media-playing-bbb") == 0;
        /* `media` is the paused control (the pointer), `media-playing` the
           running one (the two bars): both halves of the same overlay have
           to be reviewable as stills. */
        psp_ui_media_set(
            &media, true,
            strcmp(mode, "media-playing") == 0 || big_buck_bunny, false,
            big_buck_bunny ? UINT64_C(120000000) : UINT64_C(10000000),
            big_buck_bunny ? UINT64_C(596000000) : UINT64_C(18950000),
            big_buck_bunny ? "Big Buck Bunny" : "Fixture video");
        if (strcmp(mode, "media-loading") == 0) {
            psp_ui_media_set_resolving(&media, "Fixture video");
            psp_ui_media_set_resolving_progress(
                &media, "Loading...", 620u);
        } else if (strcmp(mode, "seek") == 0)
            psp_ui_media_set_seek_preview(&media, UINT64_C(15000000));
        else if (strcmp(mode, "media-error") == 0)
            psp_ui_media_set_error(&media, "AV WP 800200D2");
        else if (strcmp(mode, "media-retry") == 0) {
            psp_ui_media_set_resolving(&media, "Fixture video");
            psp_ui_media_set_resolving_progress(
                &media, "Retrying at 240p", 400u);
        }
        psp_ui_media_composite(
            &media, frame, PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_WIDTH);
    }

    bool written = write_ppm(output, frame);
    printf("psp-ui-preview: output=%s state-bytes=%zu\n",
           output, psp_ui_state_bytes());
    psp_ui_clear_chrome_font();
    tilefinch_diagnostic_qr_destroy(diagnostic_report);
    if (fonts_ready) font_set_destroy(&fonts);
    free(frame);
    return written ? 0 : 1;
}
