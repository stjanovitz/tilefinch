/* Exercise the real painter, not a duplicate string-length approximation.
   This host-only observer compiles out of both device executables. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "tilefinch/glyph_component.h"

static void observe_text(const char *, int, int, int, int,
                         int, int, int, int, bool, bool);
#define TILEFINCH_UI_TEXT_OBSERVER observe_text
#include "../src/psp_ui.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return false; } } while (0)
enum { WIDTH = 480, HEIGHT = 272, STRIDE = 496, RUN_LIMIT = 96 };
typedef struct {
    char text[512];
    int left, top, right, bottom;
    bool truncated, missing_glyph;
} TextRun;
static TextRun runs[RUN_LIMIT];
static size_t run_count;
static bool run_overflow;
static uint16_t storage[STRIDE * HEIGHT + 32];
static char catalog[TILEFINCH_UI_TRANSLATION_BYTES + 1];
static char *keys[TILEFINCH_UI_TRANSLATION_ROWS], *values[TILEFINCH_UI_TRANSLATION_ROWS];
static size_t key_count, checked_frames, checked_labels;
static const char *language_code;
static const char *capture_directory;
static const char *real_hindi_pack, *real_arabic_pack;
#include "ui_navigation_keys.inc"
static size_t checked_navigation_labels;

static void observe_text(const char *text, int x, int y, int end, int maximum,
                         int left, int top, int right, int bottom, bool truncated,
                         bool missing_glyph)
{
    (void)x; (void)y; (void)end; (void)maximum;
    if (!text[0]) return;
    if (run_count == RUN_LIMIT) { run_overflow = true; return; }
    TextRun *run = &runs[run_count++];
    snprintf(run->text, sizeof(run->text), "%s", text);
    run->left = left; run->top = top; run->right = right; run->bottom = bottom;
    run->truncated = truncated;
    run->missing_glyph = missing_glyph;
}

static bool catalog_text(const char *text)
{
    for (size_t i = 0; i < key_count; i++) {
        if (!strcasecmp(keys[i], text) || !strcmp(values[i], text)) return true;
        /* Installation progress is a translated prefix plus native digits. */
        if (!strcmp(keys[i], "Downloading") || !strcmp(keys[i], "Installing")) {
            const char *prefix = !strcmp(language_code, "en") ? keys[i] : values[i];
            size_t length = strlen(prefix);
            if (!strncmp(text, prefix, length) && text[length] == ' ') {
                const char *tail = text + length + 1;
                while (*tail >= '0' && *tail <= '9') tail++;
                if (tail > text + length + 1 && !strcmp(tail, "%")) return true;
            }
        }
    }
    return false;
}

static bool load_catalog(Budget *budget, unsigned language, TilefinchUiTranslation **translation)
{
    unsigned source_language = language == 0 ? 1 : language;
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(source_language);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s",
        TILEFINCH_UI_RESOURCE_ROOT, spec->resource_key);
    FILE *file = fopen(path, "rb");
    CHECK(file);
    size_t length = fread(catalog, 1, sizeof(catalog), file);
    CHECK(!ferror(file) && fclose(file) == 0 && length < sizeof(catalog));
    *translation = language == 0 ? NULL
        : tilefinch_ui_translation_create(budget, language, catalog, length);
    CHECK(language == 0 || *translation);
    catalog[length] = 0;
    char *cursor = strchr(catalog, '\n');
    CHECK(cursor);
    cursor++;
    key_count = 0;
    while (*cursor) {
        CHECK(key_count < TILEFINCH_UI_TRANSLATION_ROWS);
        keys[key_count] = cursor;
        char *tab = strchr(cursor, '\t'), *end = strchr(cursor, '\n');
        CHECK(tab && end && tab < end);
        *tab = 0; *end = 0;
        values[key_count] = language == 0 ? tab + 1
            : (char *)tilefinch_ui_translation_text(*translation, keys[key_count]);
        key_count++;
        cursor = end + 1;
    }
    return true;
}

static void put16(unsigned char *out, unsigned value)
{ out[0] = value >> 8; out[1] = value; }
static void put32(unsigned char *out, unsigned value)
{ out[0] = value >> 24; out[1] = value >> 16; out[2] = value >> 8; out[3] = value; }

/* A conservative pack uses the production 16x16 mono metrics and fills the
   bitmap edges. It checks the widest possible ink, not substitute '?' tiles.
   All codepoints come from public catalogs; no downloaded fonts or network. */
static bool make_metric_pack(char path[64])
{
    unsigned sequences[4096][8] = {{0}};
    unsigned lengths[4096], sequence_count = 0;
    char sequence_path[1024];
    snprintf(sequence_path, sizeof(sequence_path),
        "%s/translations/ui/v%u/hi.sequences", TILEFINCH_TEST_ROOT,
        tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_HINDI)->version);
    FILE *sequence_file = fopen(sequence_path, "r");
    CHECK(sequence_file);
    char line[128];
    while (fgets(line, sizeof(line), sequence_file)) {
        if (line[0] == '#') continue;
        CHECK(sequence_count < 4096);
        char *cursor = line, *end;
        unsigned count = 0;
        while (count < 8) {
            unsigned long cp = strtoul(cursor, &end, 16);
            if (end == cursor) break;
            CHECK(cp >= 0x0900 && cp <= 0x097f);
            sequences[sequence_count][count++] = (unsigned)cp;
            cursor = end;
        }
        CHECK(count >= 2 && count <= 8);
        lengths[sequence_count++] = count;
    }
    CHECK(!ferror(sequence_file) && fclose(sequence_file) == 0 && sequence_count > 0);
    unsigned char *present = calloc(0x110000u, 1);
    CHECK(present);
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    for (unsigned language = 1; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        TilefinchUiTranslation *t;
        CHECK(load_catalog(&budget, language, &t));
        for (size_t row = 0; row < key_count; row++) {
            const char *text = values[row];
            size_t bytes = strlen(text);
            for (size_t at = 0; at < bytes;) {
                unsigned cp;
                size_t used = font_utf8_next(text + at, bytes - at, &cp);
                CHECK(used && cp < 0x110000u);
                at += used;
                if (cp >= 128) present[cp] = 1;
            }
        }
        tilefinch_ui_translation_destroy(t);
    }
    unsigned pages = 0, glyphs = 0;
    for (unsigned page = 0; page < TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT; page++) {
        unsigned count = 0;
        for (unsigned bit = 0; bit < 256; bit++) count += present[page * 256 + bit];
        pages += count != 0; glyphs += count;
    }
    size_t pages_offset = 80 + TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2;
    size_t sequences_offset = pages_offset + pages * 36;
    size_t payload_offset = sequences_offset + sequence_count * 40;
    size_t bytes_count = payload_offset + (glyphs + sequence_count) * 32;
    unsigned char *bytes = calloc(bytes_count, 1);
    CHECK(bytes);
    memcpy(bytes, "TFGFv1\0\0", 8);
    put16(bytes + 8, 1);
    bytes[10] = 1; bytes[11] = 16; bytes[12] = 16; bytes[13] = 64;
    memcpy(bytes + 44, "glyph-ui-test", 13); bytes[14] = 13;
    put32(bytes + 16, glyphs + sequence_count); put16(bytes + 20, pages);
    put16(bytes + 22, sequence_count);
    put32(bytes + 24, 80); put32(bytes + 28, pages_offset);
    put32(bytes + 32, sequences_offset); put32(bytes + 36, payload_offset);
    put32(bytes + 40, bytes_count);
    memset(bytes + 80, 0xff, TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2);
    unsigned index = 0, glyph = 0;
    for (unsigned page = 0; page < TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT; page++) {
        bool populated = false;
        for (unsigned bit = 0; bit < 256; bit++) populated |= present[page * 256 + bit] != 0;
        if (!populated) continue;
        put16(bytes + 80 + page * 2, index);
        unsigned char *record = bytes + pages_offset + index++ * 36;
        put32(record, glyph);
        for (unsigned bit = 0; bit < 256; bit++) {
            if (!present[page * 256 + bit]) continue;
            record[4 + bit / 8] |= 1u << (bit & 7u);
            /* Hollow square leaves all four extents visible. */
            unsigned char *bitmap = bytes + payload_offset + glyph++ * 32;
            for (unsigned y = 0; y < 16; y++) {
                bitmap[y * 2] = y == 0 || y == 15 ? 255 : 128;
                bitmap[y * 2 + 1] = y == 0 || y == 15 ? 255 : 1;
            }
        }
    }
    for (unsigned at = 0; at < sequence_count; at++) {
        unsigned char *record = bytes + sequences_offset + at * 40;
        record[0] = lengths[at];
        put32(record + 4, glyphs + at);
        for (unsigned cp = 0; cp < lengths[at]; cp++)
            put32(record + 8 + cp * 4, sequences[at][cp]);
        unsigned char *bitmap = bytes + payload_offset + (glyphs + at) * 32;
        for (unsigned y = 0; y < 16; y++) {
            bitmap[y * 2] = y == 0 || y == 15 ? 255 : 128;
            bitmap[y * 2 + 1] = y == 0 || y == 15 ? 255 : 1;
        }
    }
    snprintf(path, 64, "/tmp/tilefinch-ui-metrics-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    CHECK(file && fwrite(bytes, 1, bytes_count, file) == bytes_count && fclose(file) == 0);
    free(bytes); free(present);
    CHECK(budget.current == 0);
    return true;
}

static bool validate_frame(const PspUiState *ui)
{
    CHECK(!run_overflow && run_count > 0);
    for (size_t i = 0; i < run_count; i++) {
        const TextRun *a = &runs[i];
        for (size_t key = 0; strcmp(language_code, "en") != 0
             && key < sizeof(navigation_keys) / sizeof(navigation_keys[0]); key++) {
            if (!strcasecmp(a->text, navigation_keys[key])) {
                fprintf(stderr, "untranslated navigation instruction: locale=%s '%s'\n",
                        language_code, a->text);
                return false;
            }
            if (!strcmp(a->text, tilefinch_ui_text(navigation_keys[key])))
                checked_navigation_labels++;
        }
        bool known = catalog_text(a->text);
        if (known) {
            checked_labels++;
            if (a->truncated || a->missing_glyph || a->left < 0 || a->right > WIDTH || a->top < 0 || a->bottom > HEIGHT) {
                fprintf(stderr, "locale=%s screen=%u option=%u glyph=%u choice=%u clipped '%s' ink=%d,%d..%d,%d\n",
                    language_code, ui->screen, ui->options_selection, ui->glyph_language,
                    ui->ui_language, a->text,
                    a->left, a->top, a->right, a->bottom);
                for (size_t r = 0; r < run_count; r++)
                    fprintf(stderr, "  '%s' %d,%d..%d,%d\n", runs[r].text,
                        runs[r].left, runs[r].top, runs[r].right, runs[r].bottom);
                return false;
            }
        }
        for (size_t j = i + 1; j < run_count; j++) {
            const TextRun *b = &runs[j];
            if (!known && !catalog_text(b->text)) continue;
            bool y_overlap = a->top < b->bottom && b->top < a->bottom;
            if (y_overlap && a->left < b->right && b->left < a->right) {
                fprintf(stderr, "locale=%s screen=%u overlapping '%s' and '%s'\n",
                    language_code, ui->screen, a->text, b->text);
                return false;
            }
            /* Same-row label/value pairs and breadcrumbs retain a readable gap. */
            if (y_overlap && a->right <= b->left) CHECK(b->left - a->right >= 5);
            if (y_overlap && b->right <= a->left) CHECK(a->left - b->right >= 5);
        }
    }
    for (size_t at = 0; at < 16; at++) CHECK(storage[at] == 0xa55a && storage[16 + STRIDE * HEIGHT + at] == 0xa55a);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = WIDTH; x < STRIDE; x++) CHECK(storage[16 + y * STRIDE + x] == 0xa55a);
    checked_frames++;
    return true;
}

static bool paint(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    /* The shipping pack pump is incremental. Finish pending bitmap pages
       before evaluating geometry, so a temporary fallback cannot certify fit. */
    for (unsigned pass = 0; pass < 64; pass++) {
        run_count = 0; run_overflow = false;
        for (size_t at = 0; at < sizeof(storage) / sizeof(storage[0]); at++) storage[at] = 0xa55a;
        if (ui->screen == PSP_UI_SCREEN_PAGE && ui->page_gamepad_capture) {
            /* The normal chrome cache paints separate local-coordinate
               surfaces. Observe the hint at final framebuffer coordinates. */
            draw_bottom_bar(ui, storage + 16, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        } else {
            psp_ui_composite(ui, storage + 16, WIDTH, HEIGHT, STRIDE);
        }
        bool changed = false;
        size_t read = 0;
        CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
        if (!read) return validate_frame(ui);
    }
    fprintf(stderr, "glyph pump did not settle: locale=%s screen=%u option=%u group=%u\n",
            language_code, ui->screen, ui->options_selection, ui->options_group_selection);
    CHECK(false);
}

static bool paint_failed_media(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    PspUiMediaState media = {.visible = true, .failed = true, .controls_visible = true};
    for (unsigned unavailable = 0; unavailable < 2; unavailable++) {
        media.retry_unavailable = unavailable != 0;
        bool settled = false;
        for (unsigned pass = 0; pass < 64; pass++) {
            run_count = 0; run_overflow = false;
            for (size_t at = 0; at < sizeof(storage) / sizeof(storage[0]); at++)
                storage[at] = 0xa55a;
            psp_ui_media_composite(&media, storage + 16, WIDTH, HEIGHT, STRIDE);
            bool changed = false;
            size_t read = 0;
            CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
            if (read) continue;
            CHECK(validate_frame(ui));
            UiRect back = media_failed_back_rect(
                media_failed_panel_rect(WIDTH, HEIGHT), media.retry_unavailable);
            bool found = false;
            for (size_t at = 0; at < run_count; at++) {
                if (strcmp(runs[at].text, tilefinch_ui_text("O Back"))) continue;
                CHECK(runs[at].left >= back.x && runs[at].right <= back.x + back.width);
                found = true;
            }
            CHECK(found);
            settled = true;
            break;
        }
        CHECK(settled);
    }
    return true;
}

static bool snapshot(const char *name)
{
    if (!capture_directory) return true;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s.ppm", capture_directory, language_code, name);
    FILE *file = fopen(path, "wb");
    CHECK(file && fprintf(file, "P6\n480 272\n255\n") > 0);
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) {
        uint16_t p = storage[16 + y * STRIDE + x];
        unsigned char rgb[3] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        CHECK(fwrite(rgb, 1, 3, file) == 3);
    }
    CHECK(fclose(file) == 0);
    return true;
}

static bool test_square_button_symbol(void)
{
    for (int scale = 1; scale <= 2; scale++) {
        memset(storage, 0, sizeof(storage));
        run_count = 0;
        run_overflow = false;
        int side = scale == 1 ? 7 : 11;
        int advance = side + 2;
        int end = draw_text(storage, WIDTH, HEIGHT, STRIDE, 10, 10,
                            "□", 1, 0xffff, scale);
        CHECK(end == 10 + advance);
        CHECK(chrome_text_width_bytes("□", strlen("□"), scale, false) == advance);
        CHECK(run_count == 1 && !runs[0].missing_glyph && !runs[0].truncated);
        for (int y = 0; y < 30; y++) {
            for (int x = 0; x < 30; x++) {
                bool edge = x >= 10 && x < 10 + side && y >= 12 && y < 12 + side
                    && (x == 10 || x == 9 + side || y == 12 || y == 11 + side);
                CHECK(storage[y * STRIDE + x] == (edge ? 0xffff : 0));
            }
        }
        PspUiState ui;
        psp_ui_init(&ui);
        ui.browser_ui_scale = scale;
        for (unsigned editable = 0; editable < 2; editable++) {
            ui.focus_editable = editable;
            run_count = 0;
            draw_bottom_bar(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
            bool found = false;
            for (size_t i = 0; i < run_count; i++) {
                CHECK(strstr(runs[i].text, "SQ Reload") == NULL);
                if (strstr(runs[i].text, "□")) {
                    CHECK(!runs[i].missing_glyph && !runs[i].truncated);
                    found = true;
                }
            }
            CHECK(found && !run_overflow);
        }
    }
    return true;
}

static bool test_all_languages(void)
{
    char path[64];
    CHECK(make_metric_pack(path));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider);
    if (real_hindi_pack)
        CHECK(tilefinch_glyph_provider_attach(provider, real_hindi_pack, "glyph-devanagari"));
    if (real_arabic_pack)
        CHECK(tilefinch_glyph_provider_attach(provider, real_arabic_pack, "glyph-arabic"));
    CHECK(tilefinch_glyph_provider_attach(provider, path, "glyph-ui-test")
        && font_optional_glyph_provider_install(provider));
    unsigned cluster_key = 0;
    size_t cluster_characters = 0;
    CHECK(ui_text_next_glyph("कि", strlen("कि"), &cluster_key, &cluster_characters)
              == strlen("कि")
          && cluster_characters == 2 && cluster_key > 0x10ffffu);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget,
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Oblique-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Regular.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Bold.ttf", 2u * 1024u * 1024u));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS),
        font_set_face_variant(&fonts, FONT_SANS, false, true), 1);
    CHECK(test_square_button_symbol());
    for (unsigned language = 0; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        TilefinchUiTranslation *translation;
        CHECK(load_catalog(&budget, language, &translation));
        tilefinch_ui_translation_bind(translation);
        language_code = tilefinch_ui_language_spec(language)->code;
        if (language != TILEFINCH_UI_LANGUAGE_ENGLISH) {
            for (size_t key = 0; key < sizeof(navigation_keys) / sizeof(navigation_keys[0]); key++) {
                CHECK(strcmp(tilefinch_ui_text(navigation_keys[key]), navigation_keys[key]) != 0);
            }
        }
        for (unsigned theme = 0; theme < 2; theme++) {
            PspUiState ui;
            psp_ui_init(&ui);
            ui.chrome_visible = false;
            ui.overlay_motion = 0;
            ui.chrome_theme = theme ? BROWSER_CHROME_THEME_LIGHT : BROWSER_CHROME_THEME_FINCH;
            ui.ui_language = language;
            ui.screen = PSP_UI_SCREEN_OPTIONS;
            for (unsigned group = 0; group < UI_SETTINGS_GROUP_COUNT; group++) {
                ui.options_group_selection = group;
                CHECK(paint(&ui, provider));
            }
            CHECK(snapshot(theme ? "settings-light" : "settings-dark"));
            ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
            for (unsigned option = 0; option < UI_OPTIONS_ITEM_COUNT; option++) {
                ui.options_selection = option;
                CHECK(paint(&ui, provider));
            }
            ui.screen = PSP_UI_SCREEN_GLYPH_OPTIONS;
            for (unsigned selected = 0; selected < TILEFINCH_UI_LANGUAGE_COUNT; selected++) {
                ui.ui_language = selected;
                for (unsigned glyph = 0; glyph < BROWSER_GLYPH_LANGUAGE_COUNT; glyph++) {
                    ui.glyph_language = glyph;
                    ui.glyph_options_selection = 6;
                    CHECK(paint(&ui, provider));
                }
            }
            CHECK(snapshot(theme ? "languages-light" : "languages-dark"));
            ui.glyph_language = BROWSER_GLYPH_LANGUAGE_JAPANESE;
            for (unsigned selected = 0; selected < UI_GLYPH_OPTION_ROWS; selected++) {
                ui.glyph_options_selection = selected;
                for (unsigned phase = 0; phase <= PSP_UI_GLYPH_COMPONENT_ERROR; phase++) {
                    ui.glyph_component_phase = phase;
                    ui.glyph_component_progress_plus_one = 1001;
                    ui.glyph_operation_pack = selected == 3
                        ? TILEFINCH_GLYPH_PACK_COLOR_EMOJI : TILEFINCH_GLYPH_PACK_JAPANESE;
                    ui.glyph_installed_mask = phase == PSP_UI_GLYPH_COMPONENT_READY
                        ? (1u << TILEFINCH_GLYPH_PACK_JAPANESE)
                            | (1u << TILEFINCH_GLYPH_PACK_COLOR_EMOJI) : 0;
                    ui.glyph_component_remove_confirmation = phase == PSP_UI_GLYPH_COMPONENT_READY;
                    CHECK(paint(&ui, provider));
                    ui.glyph_component_progress_plus_one = 0;
                    CHECK(paint(&ui, provider));
                }
            }
            ui.screen = PSP_UI_SCREEN_THEME_OPTIONS;
            for (unsigned selected = 0; selected < BROWSER_CHROME_THEME_CUSTOM; selected++) {
                ui.data_options_selection = selected;
                CHECK(paint(&ui, provider));
            }
            ui.screen = PSP_UI_SCREEN_TABS;
            CHECK(paint(&ui, provider));
            PspUiTabsView tabs = {.count = 2};
            ui.tabs = &tabs;
            CHECK(paint(&ui, provider));
            ui.tabs = NULL;
            ui.screen = PSP_UI_SCREEN_VIDEO_LANGUAGE_OPTIONS;
            for (unsigned audio = 0; audio < BROWSER_VIDEO_LANGUAGE_COUNT; audio++) {
                ui.video_language = audio;
                CHECK(paint(&ui, provider));
            }
            ui.screen = PSP_UI_SCREEN_MENU;
            for (unsigned scale = 1; scale <= 2; scale++) {
                ui.browser_ui_scale = scale;
                for (unsigned row = 0; row < PSP_UI_MENU_ITEM_COUNT; row++) {
                    ui.menu_selection = row;
                    CHECK(paint(&ui, provider));
                }
            }
            ui.screen = PSP_UI_SCREEN_PAGE_TOOLS;
            CHECK(paint(&ui, provider));
            ui.screen = PSP_UI_SCREEN_SITE_CONTROLS;
            CHECK(paint(&ui, provider));
            ui.screen = PSP_UI_SCREEN_HELP;
            CHECK(paint(&ui, provider));
            ui.screen = PSP_UI_SCREEN_HELP_DETAIL;
            ui.menu_selection = 3;
            CHECK(paint(&ui, provider));
            CHECK(snapshot(theme ? "controls-light" : "controls-dark"));
            ui.screen = PSP_UI_SCREEN_UPDATE_VERSIONS;
            for (unsigned phase = 0; phase <= TILEFINCH_UPDATE_HISTORY_ERROR; phase++) {
                ui.update_history_phase = phase;
                CHECK(paint(&ui, provider));
            }
            ui.update_history_count = 1;
            snprintf(ui.update_history_versions[0], sizeof(ui.update_history_versions[0]), "0.1.30");
            ui.update_history_phase = TILEFINCH_UPDATE_HISTORY_READY;
            CHECK(paint(&ui, provider));
            ui.update_history_count = 0;
            PspUiSiteStorageView sites = {.count = 1};
            snprintf(sites.rows[0].origin, sizeof(sites.rows[0].origin), "https://example.org");
            snprintf(sites.rows[0].detail, sizeof(sites.rows[0].detail), "RAM");
            ui.site_storage = &sites;
            ui.screen = PSP_UI_SCREEN_DATA_OPTIONS;
            for (unsigned row = 0; row < 1 + UI_SITE_DATA_FIXED_ROWS; row++) {
                ui.data_options_selection = row;
                CHECK(paint(&ui, provider));
            }
            ui.data_clear_confirmation = 1;
            CHECK(paint(&ui, provider));
            ui.data_options_selection = 0;
            ui.screen = PSP_UI_SCREEN_STORAGE_SITE;
            for (unsigned state = 0; state < 4; state++) {
                sites.rows[0].state = state;
                for (unsigned focus = 0; focus < 2; focus++) {
                    ui.data_clear_confirmation = focus;
                    CHECK(paint(&ui, provider));
                }
            }
            ui.data_clear_confirmation = 0;
            ui.site_storage = NULL;
            ui.screen = PSP_UI_SCREEN_PAGE;
            ui.chrome_visible = true;
            ui.page_gamepad_capture = true;
            for (unsigned scale = 1; scale <= 2; scale++) {
                ui.browser_ui_scale = scale;
                CHECK(paint(&ui, provider));
            }
            ui.page_gamepad_capture = false;
            CHECK(paint_failed_media(&ui, provider));
            ui.chrome_visible = false;
            PspUiHomeView home = {.tile_count = 1, .engine_ready = true};
            snprintf(home.tiles[0].label, sizeof(home.tiles[0].label), "Home");
            ui.home = &home;
            ui.screen = PSP_UI_SCREEN_HOME;
            CHECK(paint(&ui, provider));
            home.engine_ready = false;
            CHECK(paint(&ui, provider));
            ui.home = NULL;
        }
        tilefinch_ui_translation_bind(NULL);
        tilefinch_ui_translation_destroy(translation);
    }
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    CHECK(unlink(path) == 0 && budget.current == 0);
    CHECK(checked_frames > 2000 && checked_labels > 20000
          && checked_navigation_labels > 1000);
    printf("locale layout: %zu frames, %zu complete labels; no overlap or clipping\n", checked_frames, checked_labels);
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1) capture_directory = argv[1];
    if (argc > 2) real_hindi_pack = argv[2];
    if (argc > 3) real_arabic_pack = argv[3];
    return test_square_button_symbol() && test_all_languages() ? 0 : 1;
}
