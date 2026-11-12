/* Exercise the real painter, not a duplicate string-length approximation.
   This host-only observer compiles out of both device executables. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "tilefinch/glyph_component.h"
#include "tilefinch/glyph_component_store.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/youtube_lite.h"

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
/* The rotated player paints in a 272x480 logical viewport. */
static uint16_t storage[STRIDE * WIDTH + 32];
static char catalog[TILEFINCH_UI_TRANSLATION_BYTES + 1];
/* The authored source holds every language's column. */
static char source_text[1024 * 1024];
static char *keys[TILEFINCH_UI_TRANSLATION_ROWS], *values[TILEFINCH_UI_TRANSLATION_ROWS];
static size_t key_count, checked_frames, checked_labels;
static const char *language_code;
static const char *capture_directory;
/* Real packs from the command line, "<component id>=<path>", ahead of the
   synthetic fallbacks in a provider that holds four. */
typedef struct {
    const char *id, *path;
} RealPack;
static RealPack real_packs[TILEFINCH_GLYPH_COMPONENT_PACK_LIMIT - 1u];
static size_t real_pack_count;
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

/* The downloaded catalog holds no English keys, so they come from the
   authored source's English column. Every key must find its row: the
   generator's hash and the runtime's agree. */
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
    snprintf(path, sizeof(path), "%s/translations/ui/v%u/source.tsv",
        TILEFINCH_TEST_ROOT, spec->version);
    file = fopen(path, "rb");
    CHECK(file);
    length = fread(source_text, 1, sizeof(source_text), file);
    CHECK(!ferror(file) && fclose(file) == 0 && length < sizeof(source_text));
    source_text[length] = 0;
    char *cursor = strchr(source_text, '\n');
    CHECK(cursor);
    cursor++;
    key_count = 0;
    while (*cursor) {
        CHECK(key_count < TILEFINCH_UI_TRANSLATION_ROWS);
        keys[key_count] = cursor;
        char *tab = strchr(cursor, '\t'), *end = strchr(cursor, '\n');
        CHECK(tab && end && tab < end);
        *tab = 0; *end = 0;
        values[key_count] = language == 0 ? keys[key_count]
            : (char *)tilefinch_ui_translation_text(*translation, keys[key_count]);
        if (values[key_count] == keys[key_count] && language != 0) {
            fprintf(stderr, "locale=%s key has no row: '%s'\n", spec->code, keys[key_count]);
            CHECK(false);
        }
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
static bool make_metric_pack(char path[64], bool bengali)
{
    unsigned sequences[4096][8] = {{0}};
    unsigned lengths[4096], sequence_count = 0;
    char sequence_path[1024];
    unsigned locale = bengali ? TILEFINCH_UI_LANGUAGE_BENGALI : TILEFINCH_UI_LANGUAGE_HINDI;
    snprintf(sequence_path, sizeof(sequence_path),
        "%s/translations/ui/v%u/%s.sequences", TILEFINCH_TEST_ROOT,
        tilefinch_ui_language_spec(locale)->version, bengali ? "bengali-assamese" : "hi");
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
            CHECK((cp >= (bengali ? 0x0980u : 0x0900u)
                   && cp <= (bengali ? 0x09ffu : 0x097fu)) || cp == 0x200cu || cp == 0x200du);
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
    const char *id = bengali ? "glyph-ui-bn-test" : "glyph-ui-test";
    memcpy(bytes + 44, id, strlen(id)); bytes[14] = (unsigned char)strlen(id);
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

static bool validate_frame_geometry(const PspUiState *ui, int width, int height,
                                    int stride)
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
            if (a->truncated || a->missing_glyph || a->left < 0 || a->right > width || a->top < 0 || a->bottom > height) {
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
                fprintf(stderr, "locale=%s screen=%u overlapping '%s' (%d,%d..%d,%d) and '%s' (%d,%d..%d,%d)\n",
                    language_code, ui->screen, a->text, a->left, a->top, a->right, a->bottom,
                    b->text, b->left, b->top, b->right, b->bottom);
                return false;
            }
            /* Same-row label/value pairs and breadcrumbs retain a readable gap. */
            if (y_overlap && ((a->right <= b->left && b->left - a->right < 5)
                              || (b->right <= a->left && a->left - b->right < 5))) {
                fprintf(stderr, "locale=%s screen=%u too close '%s' (%d..%d) and '%s' (%d..%d)\n",
                    language_code, ui->screen, a->text, a->left, a->right,
                    b->text, b->left, b->right);
                return false;
            }
        }
    }
    for (size_t at = 0; at < 16; at++) CHECK(storage[at] == 0xa55a && storage[16 + stride * height + at] == 0xa55a);
    for (int y = 0; y < height; y++)
        for (int x = width; x < stride; x++) CHECK(storage[16 + y * stride + x] == 0xa55a);
    checked_frames++;
    return true;
}

static bool validate_frame(const PspUiState *ui)
{
    return validate_frame_geometry(ui, WIDTH, HEIGHT, STRIDE);
}

static bool paint(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    /* The shipping pack pump is incremental. Finish pending bitmap pages
       before evaluating geometry, so a temporary fallback cannot certify fit. */
    for (unsigned pass = 0; pass < 64; pass++) {
        run_count = 0; run_overflow = false;
        for (size_t at = 0; at < sizeof(storage) / sizeof(storage[0]); at++) storage[at] = 0xa55a;
        if (ui->screen == PSP_UI_SCREEN_PAGE
            && (ui->page_gamepad_capture || ui->focus_label_valid)) {
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
    PspUiMediaPresentation presentation = {0};
    PspUiMediaState media = {.visible = true, .failed = true, .controls_visible = true};
    psp_ui_media_set_error_reason(&media, "PLAYLIST COULD NOT LOAD", "Connection timed out");
    for (unsigned unavailable = 0; unavailable < 3; unavailable++) {
        media.retry_unavailable = unavailable != 0;
        media.presentation = unavailable == 2 ? &presentation : NULL;
        presentation.queue_active = presentation.queue_failed = presentation.queue_terminal = true;
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
                media_failed_panel_rect(&media, WIDTH, HEIGHT), media.retry_unavailable);
            bool found = false, headline = false;
            for (size_t at = 0; at < run_count; at++) {
                if (!strcmp(runs[at].text, tilefinch_ui_text("PLAYLIST COULD NOT LOAD")))
                    headline = true;
                if (strcmp(runs[at].text, tilefinch_ui_text(unavailable == 2 ? "O Cancel" : "O Back"))) continue;
                CHECK(runs[at].left >= back.x && runs[at].right <= back.x + back.width);
                found = true;
            }
            CHECK(found && headline);
            settled = true;
            break;
        }
        CHECK(settled);
    }
    return true;
}

static bool player_has_text(const char *key)
{
    const char *expected = tilefinch_ui_text(key);
    for (size_t at = 0; at < run_count; at++)
        if (!strcmp(runs[at].text, expected)) return true;
    fprintf(stderr, "missing translated player label: locale=%s key='%s'\n",
        language_code, key);
    return false;
}

static bool snapshot_geometry(const char *name, int width, int height, int stride);

static bool queue_cache_matches_full(PspUiMediaState *media)
{
    static uint16_t scratch[WIDTH * HEIGHT], menu[352 * 208];
    static uint32_t cached[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    PspUiMediaPresentation *p = media->presentation;
    unsigned old_selection = p->track_menu_selection;
    bool old_auto = p->automatic_next;
    PspUiMediaTrackMenuCache cache = {0};
    /* Writing a slot directly bypasses the paths that announce it, so each
       write here announces itself. */
    uint8_t saved_glyph_state = chrome_font_cache.loaded[0][0][0];
    chrome_font_cache.loaded[0][0][0] = UI_CHROME_GLYPH_PENDING;
    chrome_glyph_state_changed();
    CHECK(!psp_ui_media_raster_queue_update(media, old_selection, old_auto,
        menu, 352, 208, 352));
    chrome_font_cache.loaded[0][0][0] = saved_glyph_state;
    chrome_glyph_state_changed();
    saved_glyph_state = chrome_font_cache.unicode[0].loaded;
    chrome_font_cache.unicode[0].loaded = UI_CHROME_GLYPH_REFUSED_FIRST;
    chrome_glyph_state_changed();
    CHECK(!psp_ui_media_raster_queue_update(media, old_selection, old_auto,
        menu, 352, 208, 352));
    chrome_font_cache.unicode[0].loaded = saved_glyph_state;
    chrome_glyph_state_changed();
    for (unsigned step = 0; step < 16; step++) {
        for (unsigned at = 0; at < WIDTH * HEIGHT; at++)
            cached[at] = expected[at] = UINT32_C(0xff6284a6);
        p->track_menu_selection = 2 + step % 5;
        p->automatic_next = (step & 1) != 0;
        run_count = 0; run_overflow = false;
        psp_ui_media_composite_8888_cached(media, NULL, cached,
            WIDTH, HEIGHT, WIDTH, scratch, menu, 352 * 208, 1, &cache, NULL, NULL);
        psp_ui_media_composite_8888(media, NULL, expected, WIDTH, HEIGHT, WIDTH, scratch);
        if (memcmp(cached, expected, sizeof(cached))) {
            for (unsigned at = 0; at < WIDTH * HEIGHT; at++) if (cached[at] != expected[at]) {
                fprintf(stderr, "queue-cache language=%s step=%u first-diff=%u,%u got=%08x expected=%08x\n",
                    language_code, step, at % WIDTH, at / WIDTH, cached[at], expected[at]); break;
            }
        }
        CHECK(!memcmp(cached, expected, sizeof(cached)) && !run_overflow);
    }
    p->track_menu_selection = (uint8_t) old_selection;
    p->automatic_next = old_auto;
    return true;
}

static bool test_player_language(const PspUiState *ui,
                                 TilefinchGlyphProvider *provider)
{
    PspUiMediaPresentation presentation = {0};
    PspUiMediaState media = {
        .visible = true, .playing = true, .controls_visible = true,
        .seek_enabled = true, .duration_us = UINT64_C(9000000000),
        .current_time_us = UINT64_C(1800000000),
        .presentation = &presentation
    };
    snprintf(media.title, sizeof(media.title),
        "A long video title that must stay inside the player title bar in either orientation");
    presentation.audio_track_count = 2;
    presentation.subtitle_track_count = 2;
    presentation.chapter_count = MEDIA_CHAPTER_LIMIT;
    for (unsigned at = 0; at < MEDIA_CHAPTER_LIMIT; at++) {
        presentation.chapters[at].seconds = at * 300u;
        snprintf(presentation.chapters[at].title,
            sizeof(presentation.chapters[at].title),
            "Chapter %u: a deliberately long title that is safely ellipsized, not clipped", at + 1u);
    }
    snprintf(presentation.audio_tracks[0].label,
        sizeof(presentation.audio_tracks[0].label), "Original language - descriptive audio");
    snprintf(presentation.audio_tracks[1].label,
        sizeof(presentation.audio_tracks[1].label), "Alternative language - commentary");
    snprintf(presentation.subtitle_tracks[0].label,
        sizeof(presentation.subtitle_tracks[0].label), "Original language - auto-generated");
    snprintf(presentation.subtitle_tracks[1].label,
        sizeof(presentation.subtitle_tracks[1].label), "Alternative language - translated");
    for (unsigned portrait = 0; portrait < 2; portrait++) {
        int width = portrait ? HEIGHT : WIDTH;
        int height = portrait ? WIDTH : HEIGHT;
        int stride = portrait ? HEIGHT + 16 : STRIDE;
        presentation.clockwise = portrait != 0;
        presentation.rotation_available = true;
        presentation.rotation_suggested = portrait != 0;
        /* Open every category, including the first and last chapter pages;
           then paint playing, paused, seek-preview and live controls. */
        for (unsigned lane = 0; lane < 21; lane++) {
            presentation.queue_active = lane >= 13;
            presentation.queue_count = lane >= 13 ? 6 : 0;
            presentation.automatic_next = lane == 14;
            presentation.queue_failed = lane == 15;
            presentation.queue_retry = lane == 15;
            presentation.queue_loading = lane == 16;
            presentation.queue_position = lane == 20 ? 0 : 5;
            media.ended = lane >= 19;
            snprintf(presentation.queue_error, sizeof(presentation.queue_error), "Connection timed out");
            for (unsigned at = 0; at < 6; at++)
                snprintf(presentation.queue_titles[at], 128, "Upcoming video %u with a safely shortened title", at + 1);
            presentation.track_menu_open = lane < 5 || (lane >= 13 && lane < 17) || lane >= 19;
            presentation.track_menu_tab = lane >= 13 ? 3u : lane < 3 ? lane : 2u;
            presentation.track_menu_selection = lane == 3 ? 6u
                : lane == 4 ? MEDIA_CHAPTER_LIMIT - 1u : lane >= 13 ? 1u : 0u;
            media.playing = lane != 6 && lane != 10 && lane != 12 && lane != 18;
            media.seek_preview_active = lane == 7;
            media.seek_preview_time_us = UINT64_C(8765000000);
            media.live = lane == 8 || lane == 11 || lane == 12;
            presentation.audio_track_count = lane < 9 || lane >= 13 ? 2u : 0u;
            presentation.subtitle_track_count = lane < 9 || lane >= 13 ? 2u : 0u;
            presentation.chapter_count = lane < 9 || lane >= 13 ? MEDIA_CHAPTER_LIMIT : 0u;
            bool settled = false;
            for (unsigned pass = 0; pass < 64; pass++) {
                run_count = 0;
                run_overflow = false;
                for (size_t at = 0; at < sizeof(storage) / sizeof(storage[0]); at++)
                    storage[at] = 0xa55a;
                psp_ui_media_composite(&media, storage + 16,
                    width, height, stride);
                bool changed = false;
                size_t read = 0;
                CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
                if (read) continue;
                CHECK(validate_frame_geometry(ui, width, height, stride));
                if (presentation.track_menu_open) {
                    int edges[5];
                    bool folded = media_menu_tab_edges(media_track_menu_rect(&media, width, height), edges, lane >= 13 ? 4u : 3u);
                    CHECK((folded || (player_has_text("Audio") && player_has_text("Subtitles")
                        && player_has_text("Chapters")))
                        && player_has_text(lane == 15 ? "Connection timed out"
                            : lane == 16 ? "Loading..."
                            : ui_text_or_legacy("L/R TAB  X SELECT  O BACK",
                                                "L/R  X SELECT  O BACK")));
                    if (lane >= 19)
                        CHECK(player_has_text("Queue") && player_has_text("X PLAY")
                            && player_has_text("O Back")
                            && (lane == 20 || player_has_text("Video")));
                    else if (lane >= 13)
                        CHECK(player_has_text("Queue")
                            && player_has_text(lane == 15 ? "Retry queue"
                                : lane == 14 ? "Automatic next: On" : "Automatic next: Off"));
                } else {
                    CHECK(player_has_text(lane == 17 ? "X PAUSE  TRI QUEUE  L/R VIDEO"
                        : lane == 18 ? "X PLAY  TRI QUEUE  L/R VIDEO"
                        : lane == 7 ? "X GO   O CANCEL"
                        : lane == 12 ? "X PLAY"
                        : lane == 11 ? "X PAUSE"
                        : lane == 10 ? "STICK/L/R SEEK  X PLAY"
                        : lane == 9 ? "STICK/L/R SEEK  X PAUSE"
                        : lane == 8 ? "X PAUSE  TRI TRACKS"
                        : lane == 6 ? "X PLAY  TRI MENU  L/R SEEK"
                        : "X PAUSE  TRI MENU  L/R SEEK"));
                    if (portrait) CHECK(player_has_text("SELECT ROTATE"));
                    if (media.live) CHECK(player_has_text("LIVE"));
                }
                for (size_t at = 0; at < run_count; at++) {
                    const TextRun *run = &runs[at];
                    /* Author-provided titles may shorten, but their ink must
                       remain bounded just like native translated labels. */
                    CHECK(!run->missing_glyph && run->left >= 0
                        && run->right <= width && run->top >= 0
                        && run->bottom <= height);
                }
                if (presentation.track_menu_open) {
                    unsigned saved_tab = presentation.track_menu_tab;
                    int edges[5];
                    UiRect box = media_track_menu_rect(&media, width, height);
                    bool folded = media_menu_tab_edges(box, edges, lane >= 13 ? 4u : 3u);
                    if (folded) {
                        int x = box.x + 4, y = box.y + 18;
                        PspUiMediaIntent intent = portrait
                            ? psp_ui_media_activate_at(&media, WIDTH - 1 - y, x, WIDTH, HEIGHT)
                            : psp_ui_media_activate_at(&media, x, y, WIDTH, HEIGHT);
                        CHECK(intent.visual_changed && presentation.track_menu_tab == 2);
                        presentation.track_menu_tab = (uint8_t)saved_tab;
                    }
                    static const char *const categories[] = {"Chapters", "Audio", "Subtitles", "Queue"};
                    for (unsigned column = 0; !folded && column < (lane >= 13 ? 4u : 3u); column++) {
                        const TextRun *label = NULL;
                        for (size_t at = 0; at < run_count; at++)
                            if (!strcmp(runs[at].text, tilefinch_ui_text(categories[column])))
                                label = &runs[at];
                        CHECK(label);
                        int x = label->left + 1;
                        int y = media_track_menu_rect(&media, width, height).y + 18;
                        PspUiMediaIntent intent = portrait
                            ? psp_ui_media_activate_at(&media, WIDTH - 1 - y, x, WIDTH, HEIGHT)
                            : psp_ui_media_activate_at(&media, x, y, WIDTH, HEIGHT);
                        CHECK(intent.visual_changed
                            && presentation.track_menu_tab == (column == 3 ? 3u : (column + 2u) % 3u));
                    }
                    presentation.track_menu_tab = (uint8_t) saved_tab;
                }
                if (lane == 2 || lane == 7 || lane == 13 || lane == 15 || lane == 19) {
                    char name[48];
                    snprintf(name, sizeof(name), "player-%s-%s",
                        portrait ? "portrait" : "landscape",
                        lane == 2 ? "chapters" : lane == 7 ? "seek"
                            : lane == 13 ? "queue" : lane == 19 ? "queue-ended" : "queue-failed");
                    CHECK(snapshot_geometry(name, width, height, stride));
                }
                if (!portrait && (lane == 13 || lane == 14 || lane == 16))
                    CHECK(queue_cache_matches_full(&media));
                settled = true;
                break;
            }
            CHECK(settled);
        }
    }
    return true;
}

static bool snapshot_geometry(const char *name, int width, int height, int stride)
{
    if (!capture_directory) return true;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s.ppm", capture_directory, language_code, name);
    FILE *file = fopen(path, "wb");
    CHECK(file && fprintf(file, "P6\n%d %d\n255\n", width, height) > 0);
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        uint16_t p = storage[16 + y * stride + x];
        unsigned char rgb[3] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        CHECK(fwrite(rgb, 1, 3, file) == 3);
    }
    CHECK(fclose(file) == 0);
    return true;
}

static bool snapshot(const char *name)
{
    return snapshot_geometry(name, WIDTH, HEIGHT, STRIDE);
}

static bool test_download_language(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    PspUiFileDownloadPreview file = {.name="Settings.zip", .origin="https://downloads.example.test"};
    psp_ui_show_file_download(ui, &file);
    ui->overlay_animation_frames = 0;
    CHECK(paint(ui, provider));
    CHECK(player_has_text("Save file?") && player_has_text("X Save   O Cancel"));
    /* Author filenames must never be translated as UI keys. */
    bool name = false;
    for (size_t i = 0; i < run_count; i++) if (!strcmp(runs[i].text, "Settings.zip")) name = true;
    CHECK(name);
    CHECK(snapshot("download-save"));
    ZipInstallPreview zip = {.name="Homebrew", .destination="ms0:/PSP/GAME/Homebrew",
        .files=512, .skipped_files=511, .unpacked_bytes=ZIP_INSTALL_UNPACKED_LIMIT};
    for (unsigned removing=0; removing<2; removing++) {
        zip.removing = removing != 0;
        psp_ui_show_zip_install(ui, &zip);
        ui->overlay_animation_frames = 0;
        CHECK(paint(ui, provider));
        CHECK(player_has_text(removing ? "Remove homebrew?" : "Install homebrew?"));
        CHECK(player_has_text(removing ? "X Remove   O Cancel" : "X Install   O Cancel"));
        CHECK(player_has_text(removing ? "Saved data inside it will also be deleted." : "Only install software you trust."));
        CHECK(snapshot(removing ? "homebrew-remove" : "homebrew-install"));
    }
    PspUiCollectionsView view = {.section=PSP_UI_COLLECTION_DOWNLOADS, .count=2};
    view.rows[0] = (PspUiCollectionsRow){.title="Settings.zip", .detail=tilefinch_ui_text("Memory Stick file"),
        .deletable=true, .is_file=true, .offline_state=OFFLINE_ITEM_READY};
    view.rows[1] = (PspUiCollectionsRow){.title=tilefinch_ui_text("Installed homebrew"),
        .detail=tilefinch_ui_text("Manage apps installed by Tilefinch"), .offline_state=UINT8_MAX};
    psp_ui_show_collections(ui, PSP_UI_COLLECTION_DOWNLOADS);
    ui->overlay_animation_frames = 0;
    psp_ui_set_collections(ui, &view);
    for (unsigned row=0; row<2; row++) {
        ui->collections_selection = row;
        CHECK(paint(ui, provider));
        CHECK(player_has_text("Downloads"));
        CHECK(player_has_text(row ? "X OPEN   L/R SECTION" : "X DETAILS   SQUARE DELETE   L/R SECTION"));
        CHECK(snapshot(row ? "homebrew-manager-entry" : "downloads"));
    }
    ui->collections_selection = 0;
    ui->collections_delete_confirmation = 1;
    CHECK(paint(ui, provider));
    CHECK(player_has_text("PRESS SQUARE AGAIN TO DELETE") && player_has_text("SQUARE DELETE   O CANCEL"));
    CHECK(snapshot("download-delete"));
    ui->collections_delete_confirmation = 0;
    ui->collections = NULL;
    ui->file_download_preview = NULL;
    ui->zip_install_preview = NULL;
    ui->screen = PSP_UI_SCREEN_PAGE;
    return true;
}

/* A page's alert/confirm/prompt: every hint whole in every language and
   theme, the page's own text wrapped inside the box, and a long host cut
   from the left. */
static bool test_page_dialog_language(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    static const char *const messages[] = {
        "Are you sure you want to leave this page? Changes you made may not be saved.",
        "Line one\nLine two\n\nAfter a blank line, a much longer paragraph that has to wrap "
        "across several rows of the dialog before it runs out of room and ends with an "
        "ellipsis because there is more text than the dialog can show at once.\nmore\nmore",
        "Unbreakable:" "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ"
    };
    static const char *const hosts[] = {
        "shop.example.test",
        "accounts.a-very-long-subdomain-name.payments-example-service.test:8443",
        ""
    };
    for (unsigned kind = PAGE_DIALOG_ALERT; kind <= PAGE_DIALOG_PROMPT; kind++) {
        for (unsigned offer = 0; offer < 2; offer++) {
            PspUiPageDialogView view = {
                .kind = (PageDialogKind) kind, .offer_suppress = offer != 0,
                .host = hosts[(kind + offer) % 3], .message = messages[(kind + offer) % 3],
                .value = kind == PAGE_DIALOG_PROMPT ? "Ada Lovelace" : NULL
            };
            psp_ui_show_page_dialog(ui, &view);
            CHECK(ui->screen == PSP_UI_SCREEN_PAGE_DIALOG && ui->overlay_animation_frames == 0);
            CHECK(paint(ui, provider));
            CHECK(player_has_text("Page message"));
            CHECK(player_has_text(kind == PAGE_DIALOG_ALERT ? "X OK"
                : kind == PAGE_DIALOG_PROMPT ? "X Edit   START OK   O Cancel" : "X OK   O Cancel"));
            CHECK(player_has_text("Triangle Leave page"));
            if (offer) CHECK(player_has_text("Square Don't let this page show dialogs"));
            /* Page text stays inside the dialog box and never under a hint. */
            int hint_top = HEIGHT;
            for (size_t r = 0; r < run_count; r++)
                if (!strcmp(runs[r].text, tilefinch_ui_text("Triangle Leave page")))
                    hint_top = runs[r].top;
            CHECK(hint_top < HEIGHT);
            for (size_t r = 0; r < run_count; r++) {
                if (catalog_text(runs[r].text)) continue;
                CHECK(runs[r].left >= 32 && runs[r].right <= WIDTH - 32
                      && runs[r].top >= UI_TOP_HEIGHT && runs[r].bottom <= hint_top);
            }
            if (view.host[0] != '\0') {
                bool host_shown = false;
                for (size_t r = 0; r < run_count; r++) {
                    size_t length = strlen(runs[r].text);
                    /* The registrable end of the host is always shown. */
                    if (length >= 5 && strcmp(runs[r].text + length - 5, ".test") == 0)
                        host_shown = true;
                    if (strstr(view.host, ":8443") && strstr(runs[r].text, ":8443"))
                        host_shown = true;
                }
                CHECK(host_shown);
            }
            static const char *const names[] = {
                "page-dialog-alert", "page-dialog-confirm", "page-dialog-prompt"
            };
            if (offer) CHECK(snapshot(names[kind]));
        }
    }
    {
        /* Page text in a right-to-left script reads in visual order and
           starts at the box's right side; plain text keeps its own bytes. */
        static Budget dialog_budget;
        budget_init(&dialog_budget, 256u * 1024u);
        static char shown[PSP_UI_PAGE_DIALOG_SHOWN_MESSAGE_BYTES];
        static char value[PSP_UI_PAGE_DIALOG_SHOWN_VALUE_BYTES];
        PspUiPageDialogView view = {
            .kind = PAGE_DIALOG_PROMPT, .host = "shop.example.test",
            .message = "\xd7\x90\xd7\x91 \xd7\x92\xd7\x93\nabc",
            .value = "\xd7\x90\xd7\x91"
        };
        psp_ui_page_dialog_prepare(&dialog_budget, &view, shown, value);
        CHECK(view.right_to_left && view.message == shown);
        CHECK(strcmp(shown, "\xd7\x93\xd7\x92 \xd7\x91\xd7\x90\nabc") == 0);
        CHECK(view.value == value && strcmp(value, "\xd7\x91\xd7\x90") == 0);
        psp_ui_show_page_dialog(ui, &view);
        CHECK(paint(ui, provider));
        bool aligned = false;
        for (size_t r = 0; r < run_count; r++)
            if (strcmp(runs[r].text, "abc") == 0)
                aligned = runs[r].left > WIDTH / 2;
        CHECK(aligned);
        PspUiPageDialogView lam = {
            .kind = PAGE_DIALOG_ALERT, .host = "",
            .message = "\xd9\x84\xd8\xa7"
        };
        psp_ui_page_dialog_prepare(&dialog_budget, &lam, shown, value);
        CHECK(lam.right_to_left && strcmp(shown, "\xef\xbb\xbb") == 0);
        {
            /* A right-to-left row that fits ends at the box's right edge and
               keeps its last glyphs, the start of the sentence; a narrow
               first letter (yod) used to be cut for the painter's ellipsis. */
            PspUiPageDialogView yod = {
                .kind = PAGE_DIALOG_ALERT, .host = "tilefinch.test",
                .message = "\xd7\x99\xd7\x95\xd7\x9d \xd7\x98\xd7\x95\xd7\x91"
            };
            psp_ui_page_dialog_prepare(&dialog_budget, &yod, shown, value);
            CHECK(yod.right_to_left && yod.message == shown);
            psp_ui_show_page_dialog(ui, &yod);
            CHECK(paint(ui, provider));
            bool whole = false;
            for (size_t r = 0; r < run_count; r++)
                if (strcmp(runs[r].text, shown) == 0)
                    whole = !runs[r].truncated && runs[r].left > WIDTH / 2;
            CHECK(whole);
        }
        {
            /* F12: rows the prepare step already cut to the box are drawn
               whole, whatever the first paragraph's direction; the painter's
               own ellipsis reserve used to cut a later right-to-left
               paragraph's rows again, at their start. */
            static char message[600];
            snprintf(message, sizeof(message), "abc\n");
            for (int letter = 0; letter < 90; letter++)
                strcat(message, letter % 2 ? "\xd7\x91" : "\xd7\x90");
            PspUiPageDialogView later = {
                .kind = PAGE_DIALOG_ALERT, .host = "tilefinch.test",
                .message = message
            };
            psp_ui_page_dialog_prepare(&dialog_budget, &later, shown, value);
            CHECK(!later.right_to_left && later.message == shown);
            psp_ui_show_page_dialog(ui, &later);
            CHECK(paint(ui, provider));
            size_t rows = 0;
            for (size_t r = 0; r < run_count; r++) {
                if (strstr(runs[r].text, "\xd7\x90") == NULL) continue;
                rows++;
                if (runs[r].truncated)
                    fprintf(stderr, "cut again: '%s'\n", runs[r].text);
                CHECK(!runs[r].truncated);
            }
            CHECK(rows >= 2u);
            /* The prompt's answer, cut to its field, likewise. */
            static char answer[300];
            answer[0] = '\0';
            for (int letter = 0; letter < 80; letter++)
                strcat(answer, letter % 2 ? "\xd7\x92" : "\xd7\x93");
            PspUiPageDialogView asked = {
                .kind = PAGE_DIALOG_PROMPT, .host = "tilefinch.test",
                .message = "abc", .value = answer
            };
            psp_ui_page_dialog_prepare(&dialog_budget, &asked, shown, value);
            CHECK(asked.value == value && !strncmp(value, "...", 3));
            psp_ui_show_page_dialog(ui, &asked);
            CHECK(paint(ui, provider));
            bool answered = false;
            for (size_t r = 0; r < run_count; r++) {
                if (strcmp(runs[r].text, value) != 0) continue;
                answered = true;
                CHECK(!runs[r].truncated);
            }
            CHECK(answered);
        }
        PspUiPageDialogView plain = {
            .kind = PAGE_DIALOG_ALERT, .host = "", .message = "Plain text\nmore"
        };
        const char *original = plain.message;
        psp_ui_page_dialog_prepare(&dialog_budget, &plain, shown, value);
        CHECK(!plain.right_to_left && plain.message == original);
    }
    psp_ui_clear_page_dialog(ui);
    CHECK(ui->screen == PSP_UI_SCREEN_PAGE && ui->page_dialog == NULL);
    return true;
}

/* Every status toast stores an English key and is translated at paint time.
   The longest translations (Bengali, Hindi, Assamese, then French and
   Portuguese) must arrive whole at both chrome scales: wrapped at a space
   onto the second line when needed, never cut mid-codepoint, ellipsized,
   or garbled by sentence casing. */
static bool test_status_toasts(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    static const char *const long_literals[] = {
        "SELECT ENGLISH UI AND RESTART BEFORE REMOVING THIS PACK",
        "YOUTUBE SIGN-IN NOT CONFIRMED - TRY SIGN IN AGAIN",
        "WI-FI DETAILS IN RAM - RETRY, THEN DIAGNOSTIC QR",
        "SIGN-IN COUNTS IN RAM - RETRY, THEN DIAGNOSTIC QR",
        "COOKIES CLEARED - SAVED LOGIN NOT REMOVED",
        "SIGNED OUT - SAVED LOGIN COULD NOT BE REMOVED",
    };
    size_t literal_count = sizeof(long_literals) / sizeof(long_literals[0]);
    ui->screen = PSP_UI_SCREEN_PAGE;
    ui->chrome_visible = false;
    for (unsigned scale = 1; scale <= 2; scale++) {
        ui->browser_ui_scale = scale;
        for (size_t row = 0; row < key_count + literal_count; row++) {
            const char *key = row < key_count ? keys[row] : long_literals[row - key_count];
            bool status_key = strlen(key) < PSP_UI_STATUS_CAPACITY;
            for (const char *c = key; *c && status_key; c++)
                if (*c >= 'a' && *c <= 'z') status_key = false;
            if (!status_key) continue;
            psp_ui_show_status(ui, key, 60);
            CHECK(!strcmp(ui->status, key));
            ui->toast_entry_frames = 0;
            char expected[257];
            snprintf(expected, sizeof(expected), "%s", tilefinch_ui_text(key));
            psp_ui_status_sentence_case(expected);
            CHECK(paint(ui, provider));
            char joined[600];
            if (run_count == 1)
                snprintf(joined, sizeof(joined), "%s", runs[0].text);
            /* Arabic catalog text is visual order: its first line is the
               right part of the string. */
            else if (run_count == 2 && !strcmp(language_code, "ar")
                     && tilefinch_ui_text(key) != key)
                snprintf(joined, sizeof(joined), "%s %s", runs[1].text, runs[0].text);
            else if (run_count == 2)
                snprintf(joined, sizeof(joined), "%s %s", runs[0].text, runs[1].text);
            else joined[0] = '\0';
            bool whole = !strcmp(joined, expected);
            for (size_t r = 0; r < run_count; r++) {
                if (runs[r].truncated || runs[r].missing_glyph
                    || runs[r].left < 12 || runs[r].right > WIDTH - 12) whole = false;
                for (const unsigned char *c = (const unsigned char *) runs[r].text; *c; c++)
                    if (*c == 0xc3u && c[1] >= 0x80u && c[1] <= 0x9eu && c > (const unsigned char *) runs[r].text
                        && (c[-1] & 0xc0u) != 0x80u && c[-1] >= 'a' && c[-1] <= 'z') whole = false;
            }
            if (!whole) {
                fprintf(stderr, "toast locale=%s scale=%u key='%s' expected '%s'\n",
                        language_code, scale, key, expected);
                for (size_t r = 0; r < run_count; r++)
                    fprintf(stderr, "  '%s' %d..%d truncated=%u missing=%u\n", runs[r].text,
                            runs[r].left, runs[r].right, runs[r].truncated, runs[r].missing_glyph);
                CHECK(false);
            }
        }
    }
    ui->toast_frames = 0;
    ui->status[0] = '\0';
    ui->browser_ui_scale = 1;
    return true;
}

/* A toast line's expected text, written independently of the painter: its
   catalog value, or the value of the text before a trailing " NN%" with
   that count kept. True when the line came from the catalog. */
static bool expected_toast_line(const char *line, char *output, size_t size)
{
    const char *value = tilefinch_ui_text(line);
    const char *space = strrchr(line, ' ');
    if (value == line && space != NULL && space[1] >= '0' && space[1] <= '9'
        && line[strlen(line) - 1] == '%') {
        char head[128];
        snprintf(head, sizeof(head), "%.*s", (int) (space - line), line);
        if (tilefinch_ui_text(head) != head) {
            snprintf(output, size, "%s%s", tilefinch_ui_text(head), space);
            return true;
        }
    }
    snprintf(output, size, "%s", value);
    return value != line;
}

/* A run showing exactly TEXT, whole. */
static bool shown_whole(const char *text)
{
    for (size_t at = 0; at < run_count; at++)
        if (!strcmp(runs[at].text, text) && !runs[at].truncated
            && !runs[at].missing_glyph && runs[at].left >= 0
            && runs[at].right <= WIDTH) return true;
    fprintf(stderr, "missing or clipped: locale=%s '%s'\n", language_code, text);
    for (size_t r = 0; r < run_count; r++)
        fprintf(stderr, "  '%s' %d..%d truncated=%u\n", runs[r].text,
                runs[r].left, runs[r].right, runs[r].truncated);
    return false;
}

static bool paint_media(PspUiMediaState *media, TilefinchGlyphProvider *provider)
{
    for (unsigned pass = 0; pass < 64; pass++) {
        run_count = 0; run_overflow = false;
        for (size_t at = 0; at < sizeof(storage) / sizeof(storage[0]); at++)
            storage[at] = 0xa55a;
        psp_ui_media_composite(media, storage + 16, WIDTH, HEIGHT, STRIDE);
        bool changed = false;
        size_t read = 0;
        CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
        if (!read) return true;
    }
    CHECK(false);
}

/* A run whose text contains NEEDLE. */
static bool run_contains(const char *needle)
{
    for (size_t at = 0; at < run_count; at++)
        if (strstr(runs[at].text, needle) != NULL) return true;
    return false;
}

/* A drawn, unclipped run ends with SUFFIX: a host's end is on screen. */
static bool run_ends_with(const char *suffix)
{
    size_t length = strlen(suffix);
    for (size_t at = 0; at < run_count; at++) {
        size_t text = strlen(runs[at].text);
        if (text >= length && !runs[at].truncated
            && !strcmp(runs[at].text + text - length, suffix)) return true;
    }
    return false;
}

/* Audio-only media's listening view in every state: a live station playing,
   paused, reconnecting and buffering, a seekable file, the open and the
   failure. Every catalog label arrives whole and unclipped beside long
   station text; a live stream never shows a "0:00 / 0:00" timeline. */
static bool test_audio_player_language(const PspUiState *ui,
                                       TilefinchGlyphProvider *provider,
                                       const char *theme_name)
{
    psp_ui_theme_select(ui->chrome_theme);
    static PspUiMediaPresentation presentation;
    PspUiMediaState media;
    psp_ui_media_init(&media);
    psp_ui_media_bind_presentation(&media, &presentation);
    static const char *const names[] = {
        "live", "paused", "reconnecting", "buffering", "file", "opening",
        "failed"
    };
    for (unsigned lane = 0; lane < 7; lane++) {
        bool live = lane <= 3;
        psp_ui_media_set(&media, true, lane != 1, false,
                         UINT64_C(754000000),
                         lane == 4 ? UINT64_C(3215000000) : 0,
                         "Radio Manele Romania - a station name long enough "
                         "to need shortening in the title bar");
        media.live = live;
        psp_ui_media_set_audio(
            &media, true,
            "Manele, Romanian pop and dance hits around the clock, all day",
            "a-very-long-stream-host-name.radio.example.net:8888",
            lane != 4, lane == 2);
        if (lane == 3) psp_ui_media_set_buffering(&media, true, 0);
        if (lane == 5) {
            psp_ui_media_set_resolving(&media, "Radio Manele Romania");
            psp_ui_media_set_resolving_progress(&media, "Loading...", 400u);
        }
        if (lane == 6)
            psp_ui_media_set_error_reason(&media, "AUDIO DIDN'T START",
                                          "DECODER STALLED AT 0 PACKETS");
        CHECK(media.audio_only && media.controls_visible);
        CHECK(paint_media(&media, provider));
        CHECK(validate_frame(ui));
        if (lane <= 4) {
            static const char *const states[] = {
                "Playing", "Playback paused", "Reconnecting...",
                "Buffering...", "Playing"
            };
            CHECK(shown_whole(tilefinch_ui_text(states[lane])));
            CHECK(shown_whole(tilefinch_ui_text("Not encrypted")) == (lane != 4));
            /* A long host gives way at its start; its end is drawn. */
            CHECK(run_ends_with("example.net:8888"));
        }
        if (lane == 0) {
            /* A host and description that fit arrive whole. */
            psp_ui_media_set_audio(&media, true, "Dance", "141.94.96.97:8888",
                                   true, false);
            CHECK(paint_media(&media, provider));
            CHECK(validate_frame(ui));
            CHECK(shown_whole("141.94.96.97:8888") && shown_whole("Dance"));
            psp_ui_media_set_audio(
                &media, true,
                "Manele, Romanian pop and dance hits around the clock, all day",
                "a-very-long-stream-host-name.radio.example.net:8888",
                true, false);
            CHECK(paint_media(&media, provider));
        }
        if (live) {
            CHECK(shown_whole(tilefinch_ui_text("LIVE")));
            CHECK(shown_whole("12:34"));
            CHECK(!run_contains(" / "));
            CHECK(shown_whole(tilefinch_ui_text(
                lane == 1 ? "X PLAY" : "X PAUSE")));
        } else if (lane == 4) {
            CHECK(shown_whole("12:34 / 53:35"));
            CHECK(shown_whole(tilefinch_ui_text("STICK/L/R SEEK  X PAUSE")));
        } else if (lane == 5) {
            CHECK(shown_whole(tilefinch_ui_text("Opening audio")));
            psp_ui_media_set_resolving_progress(&media, "RESTARTING VIDEO",
                                                400u);
            CHECK(paint_media(&media, provider));
            CHECK(validate_frame(ui));
            CHECK(shown_whole(tilefinch_ui_text("RESTARTING AUDIO")));
        } else {
            CHECK(player_has_text("AUDIO DIDN'T START"));
            static const char *const failures[] = {
                "AUDIO DECODER COULDN'T START", "AUDIO SOURCE COULD NOT OPEN",
                "NOT ENOUGH MEMORY FOR AUDIO"
            };
            for (size_t at = 0; at < sizeof(failures) / sizeof(failures[0]);
                 at++) {
                psp_ui_media_set_error_reason(&media, failures[at],
                                              "AV ML 80010002");
                CHECK(paint_media(&media, provider));
                CHECK(validate_frame(ui));
                CHECK(player_has_text(failures[at]));
            }
            psp_ui_media_set_error_reason(&media, "AUDIO DIDN'T START",
                                          "DECODER STALLED AT 0 PACKETS");
            CHECK(paint_media(&media, provider));
        }
        char name[64];
        snprintf(name, sizeof(name), "audio-%s-%s", names[lane], theme_name);
        CHECK(snapshot(name));
    }
    return true;
}

/* The Update screen, the video player's headlines and the font-pack
   offer's confirmation box show catalog text from state: every status,
   button and headline arrives translated and whole at both chrome scales. */
static bool test_state_text(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    static const char *const statuses[] = {
        "PRESS X TO CHECK FOR UPDATES", "CONNECTING...", "CHECKING...",
        "UP TO DATE", "UPDATE AVAILABLE", "UPDATE CHECK FAILED",
        "DOWNLOADING...", "STOPPING...", "CANCELLED", "DOWNLOAD VERIFIED",
        "UNSIGNED DOWNLOAD HASHED", "DOWNLOAD FAILED",
        "DOWNLOADED FILE DID NOT VERIFY", "NOT ENOUGH FREE SPACE FOR UPDATE",
        "DEVELOPER UPDATE URL HAS NO DIRECTORY", "UPDATE URL IS TOO LONG",
        "UPDATE FILE COULD NOT BE CREATED", "NOT ENOUGH MEMORY FOR METADATA",
        "PREPARING UPDATE...", "VERIFYING DOWNLOAD...", "INSTALLING...",
        "HASHING FAILED", "PACKAGE VERIFICATION FAILED",
        "PACKAGE TABLE IS INVALID", "NOT ENOUGH FREE SPACE TO INSTALL",
        "STAGING DIRECTORY IS NOT CLEAN", "UPDATE EXTRACTION FAILED",
        "AN UPDATE FILE DID NOT VERIFY", "UPDATE METADATA COULD NOT BE SAVED",
        "ACTIVATING UPDATE...", "UPDATE COULD NOT BE ACTIVATED",
        "READY TO RESTART", "SIGNED UPDATES ARE NOT CONFIGURED",
        "BETA CHANNEL - SIGNED", "STABLE CHANNEL - SIGNED",
        "DEVELOPER URL - UNSIGNED TRIAL",
    };
    static const char *const labels[] = {
        "CHECK NOW", "CHECK AGAIN", "DOWNLOAD UPDATE", "INSTALL UPDATE",
        "RESTART TO UPDATE", "TRY INSTALL AGAIN",
    };
    static const char *const headlines[] = {
        "PLAYLIST VIDEO COULD NOT OPEN", "VIDEO DECODER NEEDS APP RESTART",
        "VIDEO DECODER MADE NO PROGRESS", "VIDEO DECODER RESET FAILED",
        "VIDEO ENDED AFTER DECODER RECOVERY", "VIDEO FIRST FRAME TIMED OUT",
        "AUDIO DIDN'T START",
        "VIDEO SEEK TIMED OUT", "VIDEO STAGING TRANSFER DID NOT COMPLETE",
        "VIDEO START CANCELLED", "VIDEO UNAVAILABLE", "VIDEO COULD NOT RESTART",
        "SEEK FRAME UNAVAILABLE", "SEEK SOURCE COULD NOT PRIME",
        "media open failed", "media seek failed",
        "could not produce bounded preview frame",
    };
    static const char *const progress[] = {
        "STARTING VIDEO", "RESTARTING VIDEO", "Restarting decoder",
        "Seeking...", "Buffering after seek",
    };
    bool english = !strcmp(language_code, "en");
    size_t status_count = sizeof(statuses) / sizeof(statuses[0]);
    size_t label_count = sizeof(labels) / sizeof(labels[0]);
    ui->screen = PSP_UI_SCREEN_UPDATE;
    for (unsigned scale = 1; scale <= 2; scale++) {
        ui->browser_ui_scale = scale;
        for (size_t at = 0; at < status_count; at++) {
            const char *label = labels[at % label_count];
            bool cancel = at % 3u == 1u;
            ui->update_channel = at % 2u ? BROWSER_UPDATE_CHANNEL_BETA
                                         : BROWSER_UPDATE_CHANNEL_STABLE;
            psp_ui_set_update(ui, at % 4u ? "0.1.30" : "", statuses[at],
                at % 5u ? "" : "Decoder rebuild needed", at % 2u ? 500 : -1,
                label, true, cancel);
            CHECK(paint(ui, provider));
            char current[160];
            snprintf(current, sizeof(current), "%s  %s",
                tilefinch_ui_text("Current version"),
                at % 4u ? "0.1.30" : tilefinch_ui_text("Unknown"));
            CHECK(shown_whole(tilefinch_ui_text("Tilefinch update"))
                  && shown_whole(current)
                  && shown_whole(tilefinch_ui_text(statuses[at]))
                  && shown_whole(tilefinch_ui_text(label)));
            if (at % 5u == 0u)
                CHECK(shown_whole(tilefinch_ui_text("Decoder rebuild needed")));
            CHECK(shown_whole(tilefinch_ui_text(
                cancel ? "O Stop"
                : ui->update_channel == BROWSER_UPDATE_CHANNEL_STABLE
                    ? "Square Versions   O Back" : "O Back")));
        }
        /* IS-10: the choice for a trial that did not finish starting. */
        psp_ui_set_update(ui, "0.1.30", "UPDATE DID NOT FINISH STARTING",
                          "", -1, "TRY AGAIN", true, false);
        psp_ui_set_update_discard(ui, true);
        CHECK(paint(ui, provider));
        CHECK(shown_whole(tilefinch_ui_text("UPDATE DID NOT FINISH STARTING"))
              && shown_whole(tilefinch_ui_text("TRY AGAIN"))
              && shown_whole(tilefinch_ui_text("Square Discard   O Back")));
    }
    ui->update_channel = BROWSER_UPDATE_CHANNEL_STABLE;
    ui->browser_ui_scale = 1;

    PspUiMediaState media = {.visible = true, .controls_visible = true};
    for (size_t at = 0; at < sizeof(headlines) / sizeof(headlines[0]); at++) {
        psp_ui_media_set_error(&media, headlines[at]);
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        /* English keeps folding a long headline onto the second row. */
        if (!english || strlen(headlines[at]) <= 32u)
            CHECK(shown_whole(tilefinch_ui_text(headlines[at])));
    }
    /* A video the PSP cannot play: each headline with its details line
       (media_failure_message.c), both translated and whole. */
    static const char *const failures[][2] = {
        {"OPTIONAL VIDEO DECODER NEEDED", "Install it for low res. See README"},
        {"VIDEO DECODER COULDN'T START", "Not enough memory"},
        {"VIDEO DECODER COULDN'T START", "Decoder rebuild needed"},
        {"VIDEO DECODER COULDN'T START", "Files missing. See README"},
        {"VIDEO DECODER COULDN'T START", "0x800200D9"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "Uses a B-frame pyramid"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "Uses over 3 reference frames"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "PSP decoder refused its pictures"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "High profile, over 432x240"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "Unsupported H.264 profile"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "Picture size too large"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "No supported codec"},
        {"PSP CAN'T PLAY THIS VIDEO FORMAT", "Video playlist too long"},
        {"VIDEO SOURCE COULD NOT OPEN", "HTTP 403"},
        {"STREAM OFFLINE OR NOT AVAILABLE", "HTTP 404"},
        {"PLAY UNENCRYPTED AUDIO?", "This station is not encrypted"},
        {"UNENCRYPTED AUDIO BLOCKED", "Change it in Site information"},
        {"PSP CAN'T PLAY THIS AUDIO", "MP3 0x807F00FD"},
    };
    for (size_t at = 0; at < sizeof(failures) / sizeof(failures[0]); at++) {
        media = (PspUiMediaState) {.visible = true, .controls_visible = true};
        psp_ui_media_set_error_reason(&media, failures[at][0], failures[at][1]);
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        CHECK(shown_whole(tilefinch_ui_text(failures[at][0]))
              && shown_whole(tilefinch_ui_text(failures[at][1])));
    }
    for (size_t at = 0; at < sizeof(progress) / sizeof(progress[0]); at++) {
        media = (PspUiMediaState) {.visible = true, .controls_visible = true,
                                   .resolving = true};
        psp_ui_media_set_resolving_progress(&media, progress[at], 450u);
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        CHECK(shown_whole(tilefinch_ui_text(progress[at])));
    }

    /* The font-pack offer: checking, the signed size, and failures. */
    ui->screen = PSP_UI_SCREEN_GLYPH_OFFER;
    for (unsigned pack = 0; pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        const char *label = tilefinch_ui_text(
            tilefinch_glyph_pack_spec((TilefinchGlyphPack) pack)->label);
        ui->glyph_confirm_pack = (uint8_t) pack;
        ui->glyph_confirm_phase = PSP_UI_GLYPH_CONFIRM_CHECKING;
        CHECK(paint(ui, provider));
        CHECK(shown_whole(label) && shown_whole(tilefinch_ui_text("Checking size..."))
              && shown_whole(tilefinch_ui_text("O Cancel")));
        psp_ui_set_glyph_offer_size(ui, (uint8_t) pack, 41u * 1024u * 1024u / 10u);
        CHECK(paint(ui, provider));
        char title[160];
        snprintf(title, sizeof(title), "%s  (4.1 MB)", label);
        CHECK(shown_whole(title)
              && shown_whole(tilefinch_ui_text("Install this font pack?"))
              && shown_whole(tilefinch_ui_text("Signed, verified, kept on the Memory Stick."))
              && shown_whole(tilefinch_ui_text("X Install   O Cancel")));
        static const char *const failures[] = {
            "SIGNED PACK DOWNLOAD UNAVAILABLE", "NETWORK NOT READY FOR FONT PACK"};
        psp_ui_set_glyph_offer_error(ui, (uint8_t) pack, failures[pack % 2u]);
        CHECK(paint(ui, provider));
        char failure[128];
        snprintf(failure, sizeof(failure), "%s", failures[pack % 2u]);
        psp_ui_status_sentence_case(failure);
        CHECK(shown_whole(english ? failure : tilefinch_ui_text(failures[pack % 2u]))
              && shown_whole(tilefinch_ui_text("O Close")));
    }

    /* The font-pack row's hints: no pack for the page language, an
       installed pack, and its removal confirmation. */
    ui->screen = PSP_UI_SCREEN_GLYPH_OPTIONS;
    ui->glyph_options_selection = 2;
    ui->glyph_component_phase = PSP_UI_GLYPH_COMPONENT_READY;
    ui->glyph_language = BROWSER_GLYPH_LANGUAGE_EMBEDDED;
    CHECK(paint(ui, provider)
          && shown_whole(tilefinch_ui_text("Choose a language above   O Back")));
    ui->glyph_language = BROWSER_GLYPH_LANGUAGE_JAPANESE;
    ui->glyph_installed_mask = 1u << TILEFINCH_GLYPH_PACK_JAPANESE;
    ui->glyph_component_remove_confirmation = false;
    CHECK(paint(ui, provider)
          && shown_whole(tilefinch_ui_text("X Check update   Square remove")));
    ui->glyph_component_remove_confirmation = true;
    CHECK(paint(ui, provider)
          && shown_whole(tilefinch_ui_text("Square remove   O Cancel")));
    ui->glyph_component_remove_confirmation = false;
    ui->glyph_installed_mask = 0;
    ui->glyph_options_selection = 0;
    ui->screen = PSP_UI_SCREEN_PAGE;
    return true;
}

/* A run showing exactly TEXT, whole, without reporting a miss. */
static bool has_whole_run(const char *text)
{
    for (size_t at = 0; at < run_count; at++)
        if (!strcmp(runs[at].text, text) && !runs[at].truncated
            && !runs[at].missing_glyph && runs[at].left >= 0
            && runs[at].right <= WIDTH) return true;
    return false;
}

/* The run showing exactly TEXT lies inside RECT. */
static bool run_inside(const char *text, UiRect rect)
{
    for (size_t at = 0; at < run_count; at++)
        if (!strcmp(runs[at].text, text))
            return runs[at].left >= rect.x
                && runs[at].right <= rect.x + rect.width
                && runs[at].top >= rect.y
                && runs[at].bottom <= rect.y + rect.height;
    return false;
}

/* Site information: the title, each connection state, the issuer
   placeholders, the cookies and storage line, every action and its armed
   confirmation, each place a site's storage can be (beside Site storage),
   and the hint.
   The site name, TLS version and a server's issuer stay as sent. */
static bool test_site_information(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    static const char *const states[4] = {
        "Secure", "TLS status unavailable", "Certificate warning", "Not secure"};
    static const char *const actions[UI_SITE_INFO_ACTION_COUNT] = {
        "Permissions & controls", "Site storage",
        "Clear data for this site", "Reset permissions"};
    bool arabic = tilefinch_ui_translation_bound_language() == TILEFINCH_UI_LANGUAGE_ARABIC;
    unsigned session_both = 0;
    snprintf(ui->url, sizeof(ui->url), "https://www.example.org/news/today");
    char domain[64], detail[128];
    ui_url_presentation(ui, domain, sizeof(domain), detail, sizeof(detail));
    CHECK(domain[0] != 0);
    ui->screen = PSP_UI_SCREEN_PAGE_INFORMATION;
    ui->site_cookie_count = 120;
    ui->site_storage_count = 2048;
    ui->site_data_bytes = 12u * 1024u * 1024u;
    for (unsigned state = 0; state < 4; state++) {
        ui->secure = state != 3;
        ui->site_tls_verify_result_available = state != 1;
        ui->site_tls_verify_result = state == 2 ? 9 : 0;
        snprintf(ui->site_tls_version, sizeof(ui->site_tls_version), "%s",
                 state == 3 ? "HTTP" : "TLS 1.3");
        /* As psp_app_input.c fills it when the server sent no issuer. */
        const char *issuer = tilefinch_ui_text(
            ui->secure ? "Certificate issuer unavailable" : "No TLS certificate");
        CHECK(strlen(issuer) < sizeof(ui->site_tls_issuer));
        snprintf(ui->site_tls_issuer, sizeof(ui->site_tls_issuer), "%s", issuer);
        char connection[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 24];
        snprintf(connection, sizeof(connection), "%s  |  %s",
                 tilefinch_ui_text(states[state]), ui->site_tls_version);
        for (unsigned place = 0; place < 4; place++) {
            ui->site_storage_state = (uint8_t) place;
            /* The Site data & storage list's words for the place; a
               this-session grant names the Memory Stick too where the
               row has room. */
            static const char *const places[4] = {
                "RAM", "Memory Stick", "RAM only", "This session"};
            char where[2 * TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 4], both[sizeof(where)];
            snprintf(where, sizeof(where), "%s",
                     place == 0 ? "RAM" : tilefinch_ui_text(places[place]));
            snprintf(both, sizeof(both), "%s (%s)", tilefinch_ui_text("Memory Stick"),
                     tilefinch_ui_text("This session"));
            char data[2 * TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 48];
            if (arabic)
                snprintf(data, sizeof(data), "120 %s  |  2048 %s  |  12288 KB",
                         tilefinch_ui_text("Cookies:"), tilefinch_ui_text("Storage:"));
            else
                snprintf(data, sizeof(data), "%s 120  |  %s 2048  |  12288 KB",
                         tilefinch_ui_text("Cookies:"), tilefinch_ui_text("Storage:"));
            for (unsigned row = 0; row < UI_SITE_INFO_ACTION_COUNT; row++) {
                ui->menu_selection = (uint8_t) row;
                for (unsigned armed = 0; armed < (row >= 2 ? 2u : 1u); armed++) {
                    ui->data_clear_confirmation = armed ? (uint8_t) (row + 1) : 0;
                    CHECK(paint(ui, provider));
                    CHECK(shown_whole(tilefinch_ui_text("Site information"))
                          && shown_whole(domain)
                          && shown_whole(connection) && shown_whole(issuer)
                          && shown_whole(data)
                          && (place == 3 && has_whole_run(both) ? (session_both++, true)
                              : shown_whole(where))
                          && shown_whole(tilefinch_ui_text("X Open / confirm   O Back")));
                    for (unsigned at = 0; at < UI_SITE_INFO_ACTION_COUNT; at++)
                        CHECK(shown_whole(tilefinch_ui_text(
                            armed && at == row ? "Press X again to confirm" : actions[at])));
                }
            }
        }
    }
    fprintf(stderr, "site information locale=%s scale=%u: Memory Stick (This session) "
            "in %u of %u session frames\n", language_code, ui->browser_ui_scale,
            session_both, 4u * (UI_SITE_INFO_ACTION_COUNT + 2u));
    if (!strcmp(language_code, "en")) CHECK(session_both > 0);
    /* A server's issuer is shown as sent, never looked up. */
    snprintf(ui->site_tls_issuer, sizeof(ui->site_tls_issuer), "Secure");
    ui->secure = true;
    ui->site_tls_verify_result_available = true;
    ui->site_tls_verify_result = 0;
    ui->data_clear_confirmation = 0;
    CHECK(paint(ui, provider) && shown_whole("Secure"));
    memset(ui->url, 0, sizeof(ui->url));
    ui->site_tls_issuer[0] = ui->site_tls_version[0] = 0;
    ui->site_cookie_count = ui->site_storage_count = 0;
    ui->site_data_bytes = 0;
    ui->site_storage_state = 0;
    ui->secure = false;
    ui->site_tls_verify_result_available = false;
    ui->menu_selection = 0;
    ui->screen = PSP_UI_SCREEN_PAGE;
    return true;
}

/* The failed-video panel, the Help details and the Site data & storage
   screens: every line arrives translated and whole at both chrome scales.
   The Retry chip shows the X button and the shared "Retry" word and grows
   to fit it; English keeps its 132 px chip in the 324 px panel. */
static bool test_remaining_screens(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    bool english = !strcmp(language_code, "en");
    char retry[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 4];
    snprintf(retry, sizeof(retry), "X %s", tilefinch_ui_text("Retry"));
    char audio[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 16], both[600];
    /* Named the way the player's other hints name the buttons. */
    snprintf(audio, sizeof(audio), "TRI %s", tilefinch_ui_text("Audio-only"));
    const char *lower = "\xe2\x96\xa1 240p";
    snprintf(both, sizeof(both), "%s   %s", audio, lower);
    for (unsigned scale = 1; scale <= 2; scale++) {
        ui->browser_ui_scale = scale;
        PspUiMediaState media = {.visible = true, .controls_visible = true};
        psp_ui_media_set_error_reason(&media, "VIDEO UNAVAILABLE",
                                      "Connection timed out");
        media.failed = true;
        for (unsigned recovery = 0; recovery < 3; recovery++) {
            media.audio_only_recovery_available = recovery != 1;
            media.lower_quality_recovery_available = recovery != 0;
            CHECK(paint_media(&media, provider) && validate_frame(ui));
            UiRect panel = media_failed_panel_rect(&media, WIDTH, HEIGHT);
            UiRect chip = media_failed_retry_rect(&media, panel);
            UiRect back = media_failed_back_rect(panel, false);
            CHECK(panel.x >= 16 && panel.x + panel.width <= WIDTH - 16
                  && chip.x + chip.width + 8 <= back.x);
            if (english)
                CHECK(panel.width == 324 && chip.width == 132);
            CHECK(shown_whole(retry) && run_inside(retry, chip)
                  && shown_whole(tilefinch_ui_text("O Back"))
                  && run_inside(tilefinch_ui_text("O Back"), back));
            CHECK(shown_whole(recovery == 0 ? audio
                              : recovery == 1 ? lower : both));
            /* The pointer finds both chips where they are drawn. */
            CHECK(psp_ui_media_activate_at(&media, chip.x + chip.width - 2,
                      chip.y + chip.height / 2, WIDTH, HEIGHT).action
                  == PSP_UI_MEDIA_ACTION_RETRY);
            CHECK(psp_ui_media_activate_at(&media, back.x + 2,
                      back.y + back.height / 2, WIDTH, HEIGHT).action
                  == PSP_UI_MEDIA_ACTION_CLOSE);
        }
        media.retry_unavailable = true;
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        CHECK(shown_whole(tilefinch_ui_text("Restart Tilefinch to use video"))
              && !has_whole_run(retry));

        /* The unencrypted-audio question: its Retry says "Play anyway",
           fits its chip and still answers through the Retry action. */
        char play[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 4];
        snprintf(play, sizeof(play), "X %s", tilefinch_ui_text("Play anyway"));
        psp_ui_media_set_error_reason(&media, "PLAY UNENCRYPTED AUDIO?",
                                      "This station is not encrypted");
        media.insecure_audio_offer = true;
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        {
            UiRect panel = media_failed_panel_rect(&media, WIDTH, HEIGHT);
            UiRect chip = media_failed_retry_rect(&media, panel);
            UiRect back = media_failed_back_rect(panel, false);
            CHECK(panel.x >= 16 && panel.x + panel.width <= WIDTH - 16
                  && chip.x + chip.width + 8 <= back.x);
            CHECK(shown_whole(play) && run_inside(play, chip)
                  && !has_whole_run(retry)
                  && run_inside(tilefinch_ui_text("O Back"), back));
            CHECK(psp_ui_media_activate_at(&media, chip.x + 2,
                      chip.y + chip.height / 2, WIDTH, HEIGHT).action
                  == PSP_UI_MEDIA_ACTION_RETRY);
        }
        /* A remembered no offers no Retry and no restart advice. */
        psp_ui_media_set_error_reason(&media, "UNENCRYPTED AUDIO BLOCKED",
                                      "Change it in Site information");
        CHECK(!media.insecure_audio_offer);
        media.retry_unavailable = media.insecure_audio_blocked = true;
        CHECK(paint_media(&media, provider) && validate_frame(ui));
        CHECK(!has_whole_run(play) && !has_whole_run(retry)
              && !has_whole_run(tilefinch_ui_text(
                     "Restart Tilefinch to use video")));

        /* Help: the last error, version and licence details. */
        static const struct { unsigned item; const char *lines[3]; } help[] = {
            {2, {"Last error summary", "Included first in Diagnostic QR",
                 "Photograph every part to keep the full report."}},
            {4, {"Version & system", "Sony PSP release build",
                 "480 x 272  |  333 MHz"}},
            {5, {"Licences", "Tilefinch is MIT licensed.",
                 "Third-party and font licences are included."}},
        };
        ui->screen = PSP_UI_SCREEN_HELP_DETAIL;
        for (size_t at = 0; at < sizeof(help) / sizeof(help[0]); at++) {
            ui->menu_selection = help[at].item;
            CHECK(paint(ui, provider));
            for (size_t line = 0; line < 3; line++)
                CHECK(shown_whole(tilefinch_ui_text(help[at].lines[line])));
            CHECK(shown_whole(tilefinch_ui_text("O Back")));
        }

        /* Site data & storage, with each place a site can be kept. The
           view is filled as psp_app_storage.c fills it. */
        static const char *const places[4] = {
            "Memory Stick", "This session", "RAM only", "RAM"};
        PspUiSiteStorageView sites = {.count = 4};
        snprintf(sites.stick_free, sizeof(sites.stick_free), "%s 1.2 GB",
                 tilefinch_ui_text("Free space:"));
        for (unsigned site = 0; site < 4; site++) {
            snprintf(sites.rows[site].origin, sizeof(sites.rows[site].origin),
                     "site%u.example.org", site);
            snprintf(sites.rows[site].detail, sizeof(sites.rows[site].detail),
                     "1.4 MB  %s", tilefinch_ui_text(places[site]));
            sites.rows[site].state = (uint8_t) (site == 1 ? 3u : site);
        }
        ui->site_storage = &sites;
        ui->screen = PSP_UI_SCREEN_DATA_OPTIONS;
        static const char *const expected[] = {
            "Site data & storage", "SITES", "MEMORY STICK", "CACHES",
            "CLEAR ALL SITES", "Offer Memory Stick", "Save RAM storage at exit",
            "Memory cache", "Disk cache", "Keep compiled scripts",
            "Clear HTTP caches", "Clear compiled scripts", "Clear cookies",
            "Clear local storage", "Clear session storage",
            "Faster revisits; uses Memory Stick space",
        };
        bool seen[sizeof(expected) / sizeof(expected[0])] = {false};
        bool seen_free = false, seen_details = true;
        for (unsigned row = 0; row < 4 + UI_SITE_DATA_FIXED_ROWS; row++) {
            ui->data_options_selection = row;
            ui->keep_compiled_scripts = row % 2u != 0;
            ui->persistent_cache_mb = row % 3u ? 0 : 8;
            CHECK(paint(ui, provider));
            for (size_t at = 0; at < sizeof(expected) / sizeof(expected[0]); at++)
                seen[at] |= has_whole_run(tilefinch_ui_text(expected[at]));
            seen_free |= has_whole_run(sites.stick_free);
            if (row < 4) seen_details &= has_whole_run(sites.rows[row].detail);
            if (row >= 4 + UI_SITE_DATA_CLEAR_CACHE) {
                ui->data_clear_confirmation = (uint8_t) (row + 1);
                CHECK(paint(ui, provider)
                      && shown_whole(tilefinch_ui_text("Press X again to confirm")));
                ui->data_clear_confirmation = 0;
            }
        }
        for (size_t at = 0; at < sizeof(expected) / sizeof(expected[0]); at++)
            if (!seen[at]) {
                fprintf(stderr, "never shown whole: locale=%s '%s'\n",
                        language_code, expected[at]);
                CHECK(false);
            }
        CHECK(seen_free && seen_details);
        sites.count = 0;
        ui->data_options_selection = 0;
        CHECK(paint(ui, provider)
              && shown_whole(tilefinch_ui_text("No site is storing data")));
        sites.count = 4;

        /* One site's storage: its place, the choice and its explanation. */
        static const char *const explanations[4] = {
            "Offers the Memory Stick when RAM runs out.",
            "Kept on the Memory Stick across restarts.",
            "Stays in RAM; the Memory Stick is never offered.",
            "On the Memory Stick until you exit."};
        static const char *const states[4] = {
            "Ask", "Memory Stick", "RAM only", "This session"};
        ui->screen = PSP_UI_SCREEN_STORAGE_SITE;
        for (unsigned site = 0; site < 4; site++) {
            ui->data_options_selection = site;
            unsigned state = sites.rows[site].state;
            for (unsigned focus = 0; focus < 3; focus++) {
                ui->data_clear_confirmation = (uint8_t) focus;
                CHECK(paint(ui, provider));
                CHECK(shown_whole(sites.rows[site].detail)
                      && shown_whole(tilefinch_ui_text("Site storage"))
                      && shown_whole(tilefinch_ui_text(states[state]))
                      && shown_whole(tilefinch_ui_text(explanations[state]))
                      && shown_whole(tilefinch_ui_text(
                             focus == 2 ? "Press X again to confirm"
                                        : "Delete its data")));
            }
        }
        ui->data_clear_confirmation = 0;
        ui->data_options_selection = 0;
        ui->site_storage = NULL;

        /* The same choice in Permissions & controls. */
        ui->screen = PSP_UI_SCREEN_SITE_CONTROLS;
        for (unsigned state = 0; state < 4; state++) {
            ui->site_storage_state = (uint8_t) state;
            ui->menu_selection = UI_SITE_CONTROLS_ITEM_COUNT - 1u;
            CHECK(paint(ui, provider)
                  && shown_whole(tilefinch_ui_text(states[state])));
        }
        ui->site_storage_state = 0;
        ui->menu_selection = 0;

        CHECK(test_site_information(ui, provider));
    }
    ui->browser_ui_scale = 1;
    ui->keep_compiled_scripts = false;
    ui->persistent_cache_mb = 0;
    ui->screen = PSP_UI_SCREEN_PAGE;
    return true;
}

/* Statuses built from two catalog lines (pack name and state, a notice and
   its menu path), a trailing progress count, mixed-case keys and the
   second-line suggestions: every line arrives translated and whole at both
   chrome scales. */
static bool test_composed_toasts(PspUiState *ui, TilefinchGlyphProvider *provider)
{
    static const char *const statuses[] = {
        "Cyrillic\nDownloading 45%", "Japanese\nInstalling 7%",
        "Devanagari (Hindi)\nChecking...", "Simplified Chinese\nDownloading...",
        "Traditional Chinese\nLanguage pack installed",
        "Color Emoji\nInstalled. Reload the page to redraw it",
        "Color Emoji\nInstalled",
        "Bengali-Assamese\nLanguage pack installed",
        "Won't ask again. Install it later from:\n"
            "Settings > Appearance > Language & emoji",
        "CONTENT BLOCKER IS OFF\nSETTINGS > PRIVACY & SECURITY",
        "JAVASCRIPT IS OFF FOR ALL SITES\nSETTINGS > BROWSING & INPUT",
        "PAGE CONTROLS ON\nSTART+SELECT EXITS",
        "SAVING SCREENSHOT 45%", "Homepage unavailable", "Nothing to retry",
        /* Long enough to wrap at a space in most languages. */
        "SELECT ENGLISH UI AND RESTART BEFORE REMOVING THIS PACK",
        "YOUTUBE SIGN-IN NOT CONFIRMED - TRY SIGN IN AGAIN",
    };
    enum { KINDS = 7 };
    size_t count = sizeof(statuses) / sizeof(statuses[0]);
    bool english = !strcmp(language_code, "en");
    for (unsigned pack = 0; pack < TILEFINCH_GLYPH_PACK_COUNT && !english; pack++) {
        const char *label = tilefinch_glyph_pack_spec((TilefinchGlyphPack) pack)->label;
        if (tilefinch_ui_text(label) == label) {
            fprintf(stderr, "untranslated pack label locale=%s '%s'\n", language_code, label);
            CHECK(false);
        }
    }
    ui->screen = PSP_UI_SCREEN_PAGE;
    ui->chrome_visible = false;
    for (unsigned scale = 1; scale <= 2; scale++) {
        ui->browser_ui_scale = scale;
        for (size_t row = 0; row < count + KINDS; row++) {
            char first[300], second[300] = "";
            bool first_translated = false;
            if (row < count) {
                char english_lines[PSP_UI_STATUS_CAPACITY];
                snprintf(english_lines, sizeof(english_lines), "%s", statuses[row]);
                char *next = strchr(english_lines, '\n');
                if (next != NULL) *next++ = '\0';
                char joined_expected[600];
                first_translated = expected_toast_line(english_lines, first, sizeof(first));
                if (next != NULL) expected_toast_line(next, second, sizeof(second));
                snprintf(joined_expected, sizeof(joined_expected), "%s%s%s",
                         first, next != NULL ? "\n" : "", second);
                psp_ui_status_sentence_case(joined_expected);
                char *split = strchr(joined_expected, '\n');
                if (split != NULL) *split++ = '\0';
                snprintf(first, sizeof(first), "%s", joined_expected);
                snprintf(second, sizeof(second), "%s", split != NULL ? split : "");
                psp_ui_show_status(ui, statuses[row], 60);
            } else {
                const char *headline = "PAGE SCRIPTS ARE RUNNING LOW ON MEMORY";
                unsigned kind = (unsigned) (row - count);
                if (kind == 0) {
                    psp_ui_show_wifi_sign_in_status(ui, "THIS WI-FI MAY REQUIRE SIGN-IN", 60);
                    headline = "THIS WI-FI MAY REQUIRE SIGN-IN";
                    snprintf(second, sizeof(second), "%s",
                             tilefinch_ui_text("X Open Wi-Fi sign-in  O Cancel"));
                } else if (kind == 1) {
                    psp_ui_show_heavy_scripts_status(ui, headline, 60);
                    snprintf(second, sizeof(second), "%s",
                             tilefinch_ui_text("X Stop page scripts  O Keep reading"));
                } else {
                    headline = "PAGE UNAVAILABLE";
                    psp_ui_show_tls_status(ui, headline, (TilefinchTlsGuidance) (kind - 1u), 60);
                    static const char *const guidance[] = {
                        "Make sure date/time in XMB Settings is correct",
                        "Try another Wi-Fi or update Tilefinch",
                        "Secure connection was redirected. Try another Wi-Fi network",
                        "Secure connection unsupported. Update Tilefinch",
                        "Open Help & diagnostics for details"
                    };
                    snprintf(second, sizeof(second), "%s", tilefinch_ui_text(guidance[kind - 2u]));
                    CHECK(english || strcmp(second, guidance[kind - 2u]) != 0);
                }
                snprintf(first, sizeof(first), "%s", tilefinch_ui_text(headline));
                psp_ui_status_sentence_case(first);
            }
            ui->toast_entry_frames = 0;
            CHECK(paint(ui, provider));
            /* A long single line may wrap at a space; rejoined it must be
               the expected text. Arabic catalog text is visual order, so
               its first painted line is the right part of the string. */
            char rejoined[600] = "";
            if (run_count == 2 && !strcmp(language_code, "ar") && first_translated)
                snprintf(rejoined, sizeof(rejoined), "%s %s", runs[1].text, runs[0].text);
            else if (run_count == 2)
                snprintf(rejoined, sizeof(rejoined), "%s %s", runs[0].text, runs[1].text);
            bool whole = second[0] == '\0'
                ? (run_count == 1 && !strcmp(runs[0].text, first))
                  || (run_count == 2 && !strcmp(rejoined, first))
                : run_count == 2 && !strcmp(runs[0].text, first) && !strcmp(runs[1].text, second);
            for (size_t r = 0; r < run_count; r++)
                if (runs[r].truncated || runs[r].missing_glyph
                    || runs[r].left < 12 || runs[r].right > WIDTH - 12) whole = false;
            if (!whole) {
                fprintf(stderr, "composed toast locale=%s scale=%u row=%zu expected '%s' / '%s'\n",
                        language_code, scale, row, first, second);
                for (size_t r = 0; r < run_count; r++)
                    fprintf(stderr, "  '%s' %d..%d truncated=%u missing=%u\n", runs[r].text,
                            runs[r].left, runs[r].right, runs[r].truncated, runs[r].missing_glyph);
                CHECK(false);
            }
        }
    }
    ui->toast_frames = 0;
    ui->status[0] = '\0';
    ui->toast_kind = PSP_UI_TOAST_PLAIN;
    ui->browser_ui_scale = 1;
    return true;
}

static bool legend_button(const char *text)
{
    static const char *const buttons[] = {"X", "O", "TRI", "START", "□"};
    for (size_t at = 0; at < sizeof(buttons) / sizeof(buttons[0]); at++)
        if (!strcmp(text, buttons[at])) return true;
    return false;
}

/* The page hint row, measured with the bound chrome faces: nothing
   overlaps or clips, a word sits closer to its own glyph than to the next
   pair's glyph, and the legend stays clear of the right-hand readouts. */
static bool check_legend_runs(const char *what, unsigned scale)
{
    CHECK(!run_overflow && run_count > 0);
    bool fail = false;
    for (size_t i = 0; i < run_count; i++) {
        const TextRun *a = &runs[i];
        /* Only a page-supplied name or portal host may ellipsize. */
        static const char *const fixed[] = {
            "Open", "Edit", "Back", "Bar", "Search", "Enter", "Reload",
            "Cancel", "100%", "B999+"
        };
        bool must_fit = legend_button(a->text);
        for (size_t at = 0; at < sizeof(fixed) / sizeof(fixed[0]); at++)
            must_fit |= !strcmp(a->text, fixed[at]);
        if ((must_fit && a->truncated) || a->missing_glyph
            || a->left < 0 || a->right > WIDTH)
            fail = true;
        for (size_t j = i + 1; j < run_count; j++) {
            const TextRun *b = &runs[j];
            int gap = a->right <= b->left ? b->left - a->right
                : b->right <= a->left ? a->left - b->right : -1;
            bool inner = legend_button(a->text) && j == i + 1
                && !legend_button(b->text);
            /* glyph -> its word: 2..8 px of ink; anything else: a pair gap */
            if (inner ? gap < 2 || gap > UI_LEGEND_INNER_GAP + 4
                      : gap < UI_LEGEND_PAIR_GAP - 3)
                fail = true;
        }
    }
    if (fail) {
        fprintf(stderr, "legend row %s scale=%u:\n", what, scale);
        for (size_t r = 0; r < run_count; r++)
            fprintf(stderr, "  '%s' ink=%d..%d%s%s\n", runs[r].text, runs[r].left,
                    runs[r].right, runs[r].truncated ? " truncated" : "",
                    runs[r].missing_glyph ? " missing" : "");
        return false;
    }
    return true;
}

static bool test_page_legend_row(void)
{
    for (unsigned scale = 1; scale <= 2; scale++) {
        for (unsigned variant = 0; variant < 6; variant++) {
            PspUiState ui;
            psp_ui_init(&ui);
            ui.screen = PSP_UI_SCREEN_PAGE;
            ui.chrome_visible = true;
            ui.browser_ui_scale = scale;
            ui.focus_editable = variant == 1;
            ui.page_requests_blocked = variant == 2 ? 1234u : 0u;
            if (variant == 3 || variant == 4) {
                ui.focus_label_valid = true;
                snprintf(ui.focus_label, sizeof(ui.focus_label), "%s",
                    variant == 3 ? "Like"
                        : "A focused control whose name is far wider than the row");
            }
            if (variant == 5) {
                ui.captive_portal_active = true;
                snprintf(ui.url, sizeof(ui.url), "%s",
                    "http://a-very-long-captive-portal-host-name.example.net/login");
            }
            run_count = 0; run_overflow = false;
            draw_bottom_bar(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
            char what[32];
            snprintf(what, sizeof(what), "variant=%u", variant);
            CHECK(check_legend_runs(what, scale));
            size_t buttons = 0;
            for (size_t r = 0; r < run_count; r++)
                buttons += legend_button(runs[r].text);
            /* Every pair fits at both scales today; the fit policy below
               handles a narrower row. */
            CHECK(buttons == (variant >= 3 ? (variant == 5 ? 1u : 2u) : 5u));
        }
    }
    /* The fit policy drops the lowest-kept pair first, never overflows,
       and keeps display order. */
    static const UiLegendPair pairs[] = {
        {"X", "Open", 4u}, {"O", "Back", 3u}, {"TRI", "Bar", 0u},
        {"START", "Search", 2u}, {"□", "Reload", 1u}
    };
    for (unsigned scale = 1; scale <= 2; scale++) {
        size_t previous = 6;
        for (int space = 480; space >= 0; space -= 4) {
            bool shown[5];
            legend_row_fit(pairs, 5, space, (int) scale, shown);
            int total = 0;
            size_t visible = 0;
            for (size_t at = 0; at < 5; at++) {
                if (!shown[at]) continue;
                total += button_legend_width(pairs[at].button, pairs[at].label,
                                             (int) scale)
                    + (visible++ ? UI_LEGEND_PAIR_GAP : 0);
            }
            CHECK(total <= space && visible <= previous);
            /* Dropped pairs are exactly the lowest `keep` values. */
            for (size_t at = 0; at < 5; at++)
                CHECK(shown[at] == (pairs[at].keep >= 5u - visible));
            previous = visible;
        }
        CHECK(previous == 0);
    }
    return true;
}

/* A page's title in the chrome reads in display order wherever it is shown,
   and a long right-to-left one loses its end, which is at the left. */
static bool page_text_run(const char *text)
{
    for (size_t i = 0; i < run_count; i++)
        if (!strcmp(runs[i].text, text)) return true;
    return false;
}

static bool test_page_text_display_order(void)
{
    const char *logical = "\xd7\x90\xd7\x91 \xd7\x92\xd7\x93";
    const char *visual = "\xd7\x93\xd7\x92 \xd7\x91\xd7\x90";
    /* The cut's bidi scratch is charged to the owner's Budget (F11) and
       returned before the draw ends. */
    static Budget text_budget;
    budget_init(&text_budget, 256u * 1024u);
    psp_ui_set_text_budget(&text_budget);
    PspUiState ui;
    psp_ui_init(&ui);
    ui.browser_ui_scale = 1;
    ui.overlay_animation_frames = 0;
    PspUiTabsView tabs = {.count = 2};
    snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "%s", logical);
    snprintf(tabs.titles[1], sizeof(tabs.titles[1]), "Plain title");
    ui.tabs = &tabs;
    ui.screen = PSP_UI_SCREEN_TABS;
    run_count = 0; run_overflow = false;
    psp_ui_composite(&ui, storage + 16, WIDTH, HEIGHT, STRIDE);
    CHECK(page_text_run(visual) && !page_text_run(logical));
    CHECK(page_text_run("Plain title"));
    ui.tabs = NULL;

    PspUiHomeView home = {.tile_count = 1, .continue_count = 1,
                          .engine_ready = true};
    snprintf(home.tiles[0].label, sizeof(home.tiles[0].label), "%s", logical);
    snprintf(home.continues[0].label, sizeof(home.continues[0].label), "%s",
             logical);
    ui.home = &home;
    ui.screen = PSP_UI_SCREEN_HOME;
    run_count = 0; run_overflow = false;
    psp_ui_composite(&ui, storage + 16, WIDTH, HEIGHT, STRIDE);
    size_t shown = 0;
    for (size_t i = 0; i < run_count; i++) {
        CHECK(strcmp(runs[i].text, logical) != 0);
        shown += !strcmp(runs[i].text, visual);
    }
    CHECK(shown == 2u);
    ui.home = NULL;

    /* With no path, the address field's second line is the title. */
    psp_ui_set_page(&ui, logical, "https://example.test", true);
    run_count = 0; run_overflow = false;
    draw_top_bar(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0, 0);
    CHECK(page_text_run(visual) && !page_text_run(logical));

    char longer[160] = "";
    for (int word = 0; word < 8; word++) strcat(longer, "\xd7\x90\xd7\x91\xd7\x92 ");
    strcat(longer, "\xd7\x93\xd7\x94");
    run_count = 0; run_overflow = false;
    draw_page_text(storage, WIDTH, HEIGHT, STRIDE, 10, 10, longer, 64, 70,
                   0xffff, 1, NULL, false);
    CHECK(run_count == 1 && !runs[0].truncated);
    CHECK(!strncmp(runs[0].text, "...", 3));
    /* The start of the title, its first letters, is kept at the right. */
    const char *end = runs[0].text + strlen(runs[0].text);
    CHECK(strlen(runs[0].text) > 6 && !strcmp(end - 6, "\xd7\x92\xd7\x91\xd7\x90"));
    CHECK(text_budget.peak > 0 && text_budget.current == 0);

    /* F12: the player's title, already cut to the bar with its dots on the
       left, is drawn whole: the painter's own ellipsis reserve used to cut
       its right end, the title's first letters, a second time. */
    PspUiMediaPresentation presentation = {0};
    PspUiMediaState media = {.visible = true, .controls_visible = true,
                             .presentation = &presentation};
    char long_title[400] = "";
    for (int letter = 0; letter < 120; letter++)
        strcat(long_title, letter % 2 ? "\xd7\x91" : "\xd7\x90");
    const char *title = psp_ui_media_shown_title(&media, &text_budget,
                                                 long_title);
    CHECK(title != long_title && !strncmp(title, "...", 3));
    snprintf(media.title, sizeof(media.title), "%s", title);
    run_count = 0; run_overflow = false;
    psp_ui_media_composite(&media, storage + 16, WIDTH, HEIGHT, STRIDE);
    bool drawn = false;
    for (size_t i = 0; i < run_count; i++) {
        if (strcmp(runs[i].text, title) != 0) continue;
        drawn = true;
        CHECK(!runs[i].truncated);
    }
    CHECK(drawn && text_budget.current == 0);
    return !run_overflow;
}

/* UI-1: a host that does not fit gives way at its start. The end of a
   host names its owner, so a lookalike such as
   accounts.google.com.<padding>.evil.example must show "...evil.example"
   and never the Google-looking start, on every surface that names the
   page's site. */
#define UI_LOOKALIKE_PAD "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
static const char ui_lookalike_host[] =
    "accounts.google.com." UI_LOOKALIKE_PAD ".evil.example";
/* Fits the chrome's 48-byte site fields but none of their rows. */
static const char ui_lookalike_short[] =
    "accounts.google.com.aaaaaaaaaaaaa.evil.example";

static bool host_end_shown(const char *what)
{
    bool end = false, start = false;
    for (size_t at = 0; at < run_count; at++) {
        const char *text = runs[at].text;
        size_t length = strlen(text);
        if (strstr(text, "google") != NULL) start = true;
        if (length >= 12 && !strcmp(text + length - 12, "evil.example")
            && strstr(text, "...") != NULL && !runs[at].truncated
            && runs[at].right <= WIDTH)
            end = true;
    }
    if (!end || start || run_overflow) {
        fprintf(stderr, "%s: host end %s, lookalike start %s\n", what,
                end ? "shown" : "missing", start ? "shown" : "hidden");
        for (size_t at = 0; at < run_count; at++)
            fprintf(stderr, "  '%s'%s\n", runs[at].text,
                    runs[at].truncated ? " truncated" : "");
        return false;
    }
    return true;
}

static bool test_long_host_keeps_its_end(void)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s/signin?continue=1",
             ui_lookalike_host);
    for (unsigned scale = 1; scale <= 2; scale++) {
        static PspUiState ui;
        psp_ui_init(&ui);
        ui.browser_ui_scale = scale;
        ui.overlay_animation_frames = 0;
        psp_ui_set_page(&ui, "Sign in", url, true);
        char what[64];

        run_count = 0; run_overflow = false;
        draw_top_bar(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0, 0);
        snprintf(what, sizeof(what), "address bar scale=%u", scale);
        CHECK(host_end_shown(what));

        ui.captive_portal_active = true;
        run_count = 0; run_overflow = false;
        draw_bottom_bar(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        snprintf(what, sizeof(what), "Wi-Fi sign-in bar scale=%u", scale);
        CHECK(host_end_shown(what));
        ui.captive_portal_active = false;

        run_count = 0; run_overflow = false;
        draw_page_information(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        CHECK(host_end_shown("site information"));

        run_count = 0; run_overflow = false;
        draw_site_controls(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        CHECK(host_end_shown("permissions & controls"));

        PspUiPageDialogView dialog = {
            .kind = PAGE_DIALOG_ALERT, .host = ui_lookalike_host,
            .message = "Your session expired."
        };
        psp_ui_show_page_dialog(&ui, &dialog);
        ui.overlay_animation_frames = 0;
        run_count = 0; run_overflow = false;
        draw_page_dialog(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0);
        CHECK(host_end_shown("page dialog"));
        ui.page_dialog = NULL;

        PspUiFileDownloadPreview download = {.name = "invoice.pdf"};
        snprintf(download.origin, sizeof(download.origin), "https://%s",
                 ui_lookalike_host);
        ui.file_download_preview = &download;
        run_count = 0; run_overflow = false;
        draw_file_download(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        CHECK(host_end_shown("file download"));
        ui.file_download_preview = NULL;

        char origin[256];
        snprintf(origin, sizeof(origin), "https://%s", ui_lookalike_host);
        psp_ui_show_storage_offer(&ui, origin, 1024, 2048, 4096, false, 0);
        ui.overlay_animation_frames = 0;
        run_count = 0; run_overflow = false;
        draw_storage_offer(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0);
        CHECK(host_end_shown("storage offer"));

        psp_ui_show_heavy_offer(&ui, ui_lookalike_host, 4096, 1000);
        ui.overlay_animation_frames = 0;
        run_count = 0; run_overflow = false;
        draw_heavy_offer(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0);
        CHECK(host_end_shown("large app offer"));

        /* Fields filled by the frontend hold at most 47 bytes. */
        PspUiTabsView tabs = {.count = 1};
        snprintf(tabs.titles[0], sizeof(tabs.titles[0]), "Sign in");
        snprintf(tabs.domains[0], sizeof(tabs.domains[0]), "%s",
                 ui_lookalike_short);
        CHECK(!strcmp(tabs.domains[0], ui_lookalike_short));
        ui.tabs = &tabs;
        run_count = 0; run_overflow = false;
        draw_tabs(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0, 0);
        bool tab_end = false;
        for (size_t at = 0; at < run_count; at++) {
            const char *text = runs[at].text;
            size_t length = strlen(text);
            CHECK(strncmp(text, "accounts.google.com.", 20) != 0
                  || !strcmp(text, ui_lookalike_short));
            tab_end |= length >= 12 && !runs[at].truncated
                && !strcmp(text + length - 12, "evil.example");
        }
        CHECK(tab_end);
        ui.tabs = NULL;

        static PspUiSiteStorageView sites;
        memset(&sites, 0, sizeof(sites));
        sites.count = 1;
        snprintf(sites.rows[0].origin, sizeof(sites.rows[0].origin), "%s",
                 ui_lookalike_short);
        snprintf(sites.rows[0].detail, sizeof(sites.rows[0].detail),
                 "12 KB  RAM");
        ui.screen = PSP_UI_SCREEN_DATA_OPTIONS;
        psp_ui_set_site_storage(&ui, &sites);
        ui.data_options_selection = 0;
        run_count = 0; run_overflow = false;
        draw_site_data(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        CHECK(host_end_shown("site data & storage"));
        ui.screen = PSP_UI_SCREEN_STORAGE_SITE;
        run_count = 0; run_overflow = false;
        draw_storage_site(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0);
        CHECK(host_end_shown("site storage"));
        ui.site_storage = NULL;
    }

    /* Saved addresses: the omnibox's suggestions and the Collections rows,
       titled and untitled. A host that fits keeps its whole name and only
       the path gives way. */
    static const char short_host_url[] =
        "https://evil.example/" UI_LOOKALIKE_PAD UI_LOOKALIKE_PAD;
    for (unsigned titled = 0; titled < 2; titled++) {
        static PspUiState ui;
        psp_ui_init(&ui);
        ui.overlay_animation_frames = 0;
        BrowserProfileSuggestion suggestion = {
            .url = url, .title = titled ? "Sign in" : NULL};
        PspUiTextEntryView entry = {
            .description = "SEARCH OR ADDRESS", .text = "evil",
            .suggestions = &suggestion, .cursor = 4,
            .suggestion_count = 1, .suggestion_selection = 0};
        psp_ui_set_text_entry(&ui, &entry);
        run_count = 0; run_overflow = false;
        draw_text_entry(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0, 0);
        CHECK(host_end_shown(titled ? "titled suggestion"
                                    : "untitled suggestion"));
        suggestion.url = short_host_url;
        run_count = 0; run_overflow = false;
        draw_text_entry(&ui, storage, WIDTH, HEIGHT, STRIDE, 0, 0, 0, 0);
        bool path_cut = false;
        for (size_t at = 0; at < run_count; at++)
            path_cut |= !strncmp(runs[at].text, "https://evil.example/aaa",
                                 24u) && runs[at].truncated;
        CHECK(path_cut && !run_overflow);
        psp_ui_clear_text_entry(&ui);

        static PspUiCollectionsView view;
        memset(&view, 0, sizeof(view));
        view.section = PSP_UI_COLLECTION_HISTORY;
        view.count = 2;
        view.rows[0].title = titled ? "Sign in" : url;
        view.rows[0].detail = url;
        view.rows[1].title = "Example";
        view.rows[1].detail = short_host_url;
        psp_ui_show_collections(&ui, PSP_UI_COLLECTION_HISTORY);
        ui.overlay_animation_frames = 0;
        psp_ui_set_collections(&ui, &view);
        run_count = 0; run_overflow = false;
        draw_collections(&ui, storage, WIDTH, HEIGHT, STRIDE,
                         ui_surface_accent(BROWSER_CHROME_THEME_FINCH));
        CHECK(host_end_shown(titled ? "titled history row"
                                    : "untitled history row"));
        path_cut = false;
        for (size_t at = 0; at < run_count; at++)
            path_cut |= !strncmp(runs[at].text, "https://evil.example/aaa",
                                 24u) && runs[at].truncated;
        CHECK(path_cut);
        ui.collections = NULL;
    }
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

static lxb_dom_node_t *provider_node(BrowserEngine *engine, const char *id)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(
        browser_engine_navigation(engine)->page.document.html);
    lxb_dom_node_t *node = root;
    for (size_t visits = 0; node && visits < 8192; visits++) {
        size_t length = 0;
        const char *value = document_attribute(node, "id", &length);
        if (value && length == strlen(id) && !memcmp(value, id, length)) return node;
        if (node->first_child) node = node->first_child;
        else {
            while (node != root && !node->next) node = node->parent;
            node = node == root ? NULL : node->next;
        }
    }
    return NULL;
}

/* The width layout gives this text: right-to-left letters take the form the
   shaper picks, whose advance differs from the isolated letter's, and a letter
   a ligature absorbs takes none. */
static int shaped_text_width_fixed(Budget *budget, const FontFace *face, FontFamily family,
                                   const char *text, size_t length, int size_fixed, bool bold)
{
    if (!text_bidi_maybe_needed(text, length))
        return font_text_width_for_family_at_size_fixed(
            face, family, text, length, size_fixed, false, bold);
    TextBidiStatus status;
    TextBidiParagraph *paragraph = text_bidi_paragraph_create(
        budget, text, length, TEXT_BIDI_BASE_AUTO_LTR, &status);
    if (!paragraph) return -1;
    const TextBidiUnit *units = text_bidi_paragraph_units(paragraph);
    int width = 0;
    for (size_t at = 0; at < text_bidi_paragraph_count(paragraph); at++) {
        unsigned char encoded[4];
        size_t bytes = tilefinch_utf8_encode(units[at].shaped_codepoint, encoded);
        width += font_text_width_for_family_at_size_fixed(
            face, family, (const char *)encoded, bytes, size_fixed, false, bold);
    }
    text_bidi_paragraph_destroy(paragraph);
    return width;
}

/* A right-to-left letter still in its logical form. A notice that the catalog
   has already shaped into display order holds presentation forms instead, and
   paints as it stands. */
static bool text_has_logical_rtl_letter(const char *text, size_t length)
{
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (!used) return false;
        if ((codepoint >= 0x0590u && codepoint <= 0x05ffu)
            || (codepoint >= 0x0620u && codepoint <= 0x064au)
            || (codepoint >= 0x066eu && codepoint <= 0x06d3u)
            || (codepoint >= 0x0750u && codepoint <= 0x077fu)
            || (codepoint >= 0x08a0u && codepoint <= 0x08ffu)) return true;
        at += used;
    }
    return false;
}

static bool provider_text_fits(BrowserEngine *engine, const FontSet *fonts)
{
    const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type != DRAW_TEXT || !command->text_length) continue;
        if (command->x < 0 || command->x + command->width > WIDTH)
            fprintf(stderr, "text bounds x=%d width=%d text=%.*s\n", command->x,
                command->width, (int)command->text_length, command->text);
        CHECK(command->x >= 0 && command->x + command->width <= WIDTH);
        bool bold = draw_uses_bold_face(command);
        const FontFace *face = draw_command_face(layout, command);
        int width = (shaped_text_width_fixed(browser_engine_budget(engine), face,
            font_context_metric_family(layout->web_fonts, draw_command_font_family(command), face), command->text,
            command->text_length, draw_command_text_font_size_fixed(command),
            bold) + 63) / 64;
        CHECK(width > 0);
        if (width > command->width + 2)
            fprintf(stderr, "text width=%d available=%d text=%.*s\n", width,
                command->width, (int)command->text_length, command->text);
        CHECK(width <= command->width + 2);
        if (text_has_logical_rtl_letter(command->text, command->text_length)
            && !layout_bidi_command_for_index(layout, at))
            fprintf(stderr, "right-to-left text painted in logical order: %.*s\n",
                (int)command->text_length, command->text);
        CHECK(!text_has_logical_rtl_letter(command->text, command->text_length)
            || layout_bidi_command_for_index(layout, at));
    }
    (void) fonts;
    return true;
}

static bool test_bidi_glyph_raster(const FontSet *fonts, Budget *budget)
{
    enum { W = 64, H = 32 };
    size_t owned = budget->current;
    DrawCommand command = {.type = DRAW_TEXT, .x = 2, .y = 2, .width = 60,
        .height = 24, .text = "ABC", .text_length = 3, .color = 0xffffff,
        .opacity_scale = 256};
    draw_command_set_text_font_size_fixed(&command, 16 * 64);
    LayoutBidiGlyph glyphs[] = {
        {.codepoint = 'C', .x_fixed = 2 * 64},
        {.codepoint = 'A', .x_fixed = 22 * 64},
        {.codepoint = 'B', .x_fixed = 42 * 64}
    };
    LayoutBidiCommand bidi = {.command_index = 0, .glyph_count = 3};
    LayoutDocument layout = {.budget = budget, .fonts = fonts,
        .commands = &command, .count = 1, .capacity = 1, .width = W,
        .scroll_width = W, .height = H, .bidi_commands = &bidi,
        .bidi_command_count = 1, .bidi_glyphs = glyphs, .bidi_glyph_count = 3};
    CHECK(viewport_context_init(&layout.viewport, W, H, W, H));
    uint16_t actual[W * H] = {0}, expected[W * H] = {0};
    TileCache cache = {0};
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
        && tile_cache_set_frame(&cache, actual, W * H)
        && tile_cache_render_frame(&cache, 0, W, H, NULL));
    tile_cache_destroy(&cache);
    DrawCommand reference[3];
    for (size_t at = 0; at < 3; at++) {
        reference[at] = command;
        reference[at].text = at == 0 ? "C" : at == 1 ? "A" : "B";
        reference[at].text_length = 1;
        reference[at].width = 18;
        draw_command_set_text_x_fixed(&reference[at], glyphs[at].x_fixed);
    }
    layout.commands = reference;
    layout.count = layout.capacity = 3;
    layout.bidi_command_count = 0;
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
        && tile_cache_set_frame(&cache, expected, W * H)
        && tile_cache_render_frame(&cache, 0, W, H, NULL));
    tile_cache_destroy(&cache);
    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    /* The shadow pass paints moved copies of the command; they must keep the
       visual order, as must the fixed and sticky passes that copy it too. */
    draw_command_set_text_shadow_blur(&command, 2);
    for (size_t at = 0; at < 3; at++)
        draw_command_set_text_shadow_blur(&reference[at], 2);
    layout.commands = &command;
    layout.count = layout.capacity = 1;
    layout.bidi_command_count = 1;
    memset(actual, 0, sizeof(actual));
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
        && tile_cache_set_frame(&cache, actual, W * H)
        && tile_cache_render_frame(&cache, 0, W, H, NULL));
    tile_cache_destroy(&cache);
    layout.commands = reference;
    layout.count = layout.capacity = 3;
    layout.bidi_command_count = 0;
    memset(expected, 0, sizeof(expected));
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
        && tile_cache_set_frame(&cache, expected, W * H)
        && tile_cache_render_frame(&cache, 0, W, H, NULL));
    tile_cache_destroy(&cache);
    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    CHECK(budget->current == owned);
    return true;
}

static bool provider_paint(BrowserEngine *engine, TilefinchGlyphProvider *provider)
{
    for (unsigned pass = 0; pass < 64; pass++) {
        CHECK(browser_engine_render_frame(engine, NULL));
        bool changed = false;
        size_t read = 0;
        CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
        if (!read) return true;
        tile_cache_repaint_glyphs(browser_engine_render_shell(engine));
    }
    CHECK(false);
}

static bool test_provider_language(const FontSet *fonts, TilefinchGlyphProvider *provider)
{
    static const char *const notices[] = {
        "Search above or select Refresh.", "Reloading suggestions...",
        "New suggestions ready - select Refresh.", "Suggestions up to date.",
        "Suggestions unavailable - try Refresh.", "Sign in to view your subscriptions.",
        "Suggestions will load in the background.", "Select Like or Unlike.",
        "Saving like...", "Video liked.", "Like failed - select Like to retry.",
        "Sign in to like this video.", "Removing like...", "Like removed.",
        "Unlike failed - select Unlike to retry.", "Sign in to unlike this video.",
        "Updating...", "Added to playlist.", "Removed from playlist.", "Subscribed.", "Unsubscribed.",
        "Not confirmed. Select again to retry.", "Sign in to use this action.", "Select an action."
    };
    BrowserDeviceProfile device;
    browser_device_profile_psp3000(&device);
    BrowserConfig config;
    browser_config_init(&config, &device);
    config.memory_limit = 16u * 1024u * 1024u;
    config.resources.enabled = false;
    config.resources.web_fonts_enabled = false;
    CHECK(browser_config_set_font_paths(&config,
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Oblique-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Regular.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Bold.ttf", 1536u * 1024u));
    char error[256] = {0};
    for (unsigned lane = 0; lane < 6; lane++) {
        fprintf(stderr, "provider layout language=%s lane=%u\n", language_code, lane);
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine && browser_engine_set_youtube_topics(engine, false));
        if (lane == 1) {
            CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_ROOT
                "/tests/fixtures/http-youtube-personalized", error, sizeof(error)));
            CHECK(browser_session_cookie_set_http(browser_engine_session(engine),
                "https://m.youtube.com/", "SAPISID=fixture; Secure; HttpOnly; Path=/"));
        }
        if (lane < 2) {
            CHECK(browser_engine_load_url(engine, "https://www.youtube.com/", true));
            CHECK(browser_engine_render_frame(engine, NULL));
            if (lane == 1) {
                for (unsigned turn = 0; turn < 256; turn++) {
                    bool changed = false;
                    (void) browser_engine_run_idle_work(engine, &changed);
                    if (browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT) break;
                }
                CHECK(browser_engine_youtube_feed_state(engine) == BROWSER_YOUTUBE_FEED_CURRENT);
            }
        } else if (lane == 2) {
            static const char watch[] = "<script>var ytInitialData='{\"chapterRenderer\":{"
                "\"title\":{\"simpleText\":\"A long chapter title that wraps safely on the PSP screen\"},"
                "\"timeRangeStartMillis\":60000}}';</script>"
                "<script>var ytInitialPlayerResponse={\"videoDetails\":{\"videoId\":\"TFTEST00001\","
                "\"channelId\":\"UCabcdefghijklmnopqrstuv\","
                "\"title\":\"Account\",\"shortDescription\":\"Account\",\"lengthSeconds\":\"200\"}};</script>";
            YoutubeLiteDocument document = {0};
            CHECK(youtube_lite_build_document(browser_engine_budget(engine),
                "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details",
                watch, sizeof(watch) - 1u, &document, error, sizeof(error)));
            /* Author text coinciding with a UI key must not be translated. */
            CHECK(strstr(document.html, "<h1>Account</h1>") != NULL
                && strstr(document.html, "{{") == NULL);
            CHECK(browser_engine_commit_html(engine,
                "https://www.youtube.com/watch?v=TFTEST00001&tilefinch_view=details",
                document.html, document.html_length, true));
            youtube_lite_document_destroy(&document);
        } else if (lane < 5) {
            static const char library[] = "<script>var ytInitialData='{\"contents\":["
                "{\"playlistRenderer\":{\"playlistId\":\"PLfixture\",\"title\":{\"simpleText\":\"A saved playlist\"}}}]}';</script>";
            static const char playlist[] = "<script>var ytInitialData='{\"playlistVideoListRenderer\":{"
                "\"playlistId\":\"WL\",\"isEditable\":true,\"contents\":[{\"playlistVideoRenderer\":{"
                "\"videoId\":\"TFTEST00001\",\"title\":{\"simpleText\":\"A saved video\"}}},"
                "{\"playlistVideoRenderer\":{\"videoId\":\"TFTEST00002\",\"title\":{\"simpleText\":\"Another saved video\"}}}]}}';</script>";
            const char *url = lane == 3
                ? "https://www.youtube.com/feed/playlists?tilefinch_save=TFTEST00001"
                : "https://www.youtube.com/playlist?list=WL";
            const char *source = lane == 3 ? library : playlist;
            YoutubeLiteDocument document = {0};
            CHECK(youtube_lite_build_document(browser_engine_budget(engine), url,
                source, strlen(source), &document, error, sizeof(error)));
            CHECK(browser_engine_commit_html(engine, url, document.html,
                document.html_length, true));
            youtube_lite_document_destroy(&document);
        } else {
            static const char search[] = "<script>var ytInitialData='{}';</script>";
            const char *url = "https://www.youtube.com/results?search_query=fixture&tilefinch_date=2&tilefinch_duration=2&tilefinch_sort=1";
            YoutubeLiteDocument document = {0};
            CHECK(youtube_lite_build_document(browser_engine_budget(engine), url,
                search, sizeof(search) - 1, &document, error, sizeof(error))
                && browser_engine_commit_html(engine, url, document.html, document.html_length, true));
            youtube_lite_document_destroy(&document);
        }
        if (lane == 2 || lane == 5) {
            lxb_dom_node_t *panel = provider_node(engine, lane == 2 ? "yt-chapters" : "yt-filter-panel");
            ControllerAction action = {0};
            CHECK(panel && browser_engine_focus_node(engine, panel->first_child)
                && browser_engine_activate(engine, &action)
                && action.type == CONTROLLER_ACTION_CONTROL);
        }
        CHECK(provider_paint(engine, provider));
        lxb_dom_node_t *search = provider_node(engine, "yt-search");
        size_t length = 0;
        const char *placeholder = document_attribute(search, "placeholder", &length);
        const char *translated = tilefinch_ui_logical_text("Search YouTube");
        CHECK(placeholder && length == strlen(translated)
            && !memcmp(placeholder, translated, length));
        CHECK(provider_text_fits(engine, fonts));
        size_t query_length = 0;
        (void) document_attribute(search, "value", &query_length);
        if (!strcmp(language_code, "ar") && !query_length) {
            /* Read from the right: the box's edge, the Arabic words, YouTube. */
            const LayoutDocument *searched = &browser_engine_navigation(engine)->page.layout;
            const LayoutNodeBox *box = layout_box_for_node(searched, search);
            size_t found = searched->count;
            for (size_t at = 0; found == searched->count && at < searched->count; at++)
                if (searched->commands[at].type == DRAW_TEXT && searched->commands[at].text == placeholder)
                    found = at;
            CHECK(box && found < searched->count);
            const DrawCommand *text = &searched->commands[found];
            const LayoutBidiCommand *bidi = layout_bidi_command_for_index(searched, found);
            CHECK(bidi && bidi->glyph_count > 8);
            const LayoutBidiGlyph *glyphs = &searched->bidi_glyphs[bidi->glyph_start];
            CHECK(glyphs[0].codepoint == 'Y' && glyphs[6].codepoint == 'e');
            for (size_t at = 1; at < bidi->glyph_count; at++)
                CHECK(glyphs[at - 1].x_fixed <= glyphs[at].x_fixed);
            CHECK(glyphs[bidi->glyph_count - 1].codepoint == 0x0627u
                || glyphs[bidi->glyph_count - 1].codepoint == 0xfe8du);
            CHECK(text->x + text->width > box->x + box->width / 2
                && text->x + text->width <= box->x + box->width);
        }
        /* Background/border insertions must not attach a neighboring RTL
           label's glyph map to the unlocalized brand. */
        const LayoutDocument *painted = &browser_engine_navigation(engine)->page.layout;
        for (size_t at = 0; at < painted->count; at++) {
            const DrawCommand *command = &painted->commands[at];
            if (command->type != DRAW_TEXT || command->text_length != 7
                || memcmp(command->text, "YouTube", 7)) continue;
            const LayoutBidiCommand *b = layout_bidi_command_for_index(painted, at);
            for (size_t g = 0; b && g < b->glyph_count; g++)
                CHECK(painted->bidi_glyphs[b->glyph_start + g].codepoint < 128);
        }
        const LayoutDocument *notice_layout = &browser_engine_navigation(engine)->page.layout;
        const LayoutNodeBox *notice = layout_box_for_node(notice_layout,
            provider_node(engine, lane >= 3 ? "yt-library-status"
                : lane == 2 ? "yt-like-status" : "yt-feed-status"));
        if (notice) {
            const FontFace *face = font_context_face(notice_layout->fonts, notice_layout->web_fonts, FONT_SANS);
            for (size_t at = 0; at < sizeof(notices) / sizeof(*notices); at++) {
                const char *text = tilefinch_ui_text(notices[at]);
                int width = (font_text_width_at_size_fixed(face, text, strlen(text), 11 * 64, false) + 63) / 64;
                CHECK(width <= notice->width);
            }
            if (lane == 1 && !strcmp(language_code, "ar")) {
                for (size_t at = 0; at < notice_layout->bidi_command_count; at++) {
                    size_t command = notice_layout->bidi_commands[at].command_index;
                    CHECK(command < notice->command_start || command >= notice->command_end);
                }
            }
        }
        if (capture_directory) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s-youtube-%u.ppm", capture_directory, language_code, lane);
            CHECK(browser_engine_render_frame(engine, path));
        }
        if (lane == 1) {
            const LayoutDocument *layout = &browser_engine_navigation(engine)->page.layout;
            const LayoutNodeBox *account = layout_box_for_node(layout, provider_node(engine, "yt-account-action"));
            CHECK(account && account->x + account->width <= WIDTH - 5);
            /* Sign out is in the Account panel, never in the header. */
            CHECK(provider_node(engine, "yt-signout-action") == NULL);
            CHECK(browser_engine_focus_node(engine, provider_node(engine, "yt-account-action")));
            ControllerAction action;
            CHECK(browser_engine_activate(engine, &action) && action.type == CONTROLLER_ACTION_CONTROL);
            CHECK(provider_paint(engine, provider) && provider_text_fits(engine, fonts));
            const LayoutNodeBox *panel = layout_box_for_node(&browser_engine_navigation(engine)->page.layout,
                provider_node(engine, "yt-account-panel"));
            CHECK(panel && panel->y >= 30 && panel->y + panel->height <= HEIGHT);
            const LayoutNodeBox *signout = layout_box_for_node(&browser_engine_navigation(engine)->page.layout,
                provider_node(engine, "yt-signout-open"));
            CHECK(signout && signout->x + signout->width <= WIDTH - 5
                && signout->y + signout->height <= panel->y + panel->height);
            if (capture_directory) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s-youtube-account.ppm", capture_directory, language_code);
                CHECK(browser_engine_render_frame(engine, path));
            }
            /* The page that asks before Sign out: both answers on the
               first screen, Cancel focused. */
            CHECK(browser_engine_load_url(engine, YOUTUBE_LITE_SIGNOUT_CONFIRM_URL, true));
            CHECK(provider_paint(engine, provider) && provider_text_fits(engine, fonts));
            const LayoutDocument *confirm = &browser_engine_navigation(engine)->page.layout;
            lxb_dom_node_t *cancel = provider_node(engine, "yt-signout-cancel");
            const LayoutNodeBox *answers[] = {layout_box_for_node(confirm, cancel),
                layout_box_for_node(confirm, provider_node(engine, "yt-signout-action"))};
            for (unsigned i = 0; i < 2u; i++)
                CHECK(answers[i] && answers[i]->x + answers[i]->width <= WIDTH - 5
                    && answers[i]->y + answers[i]->height <= HEIGHT);
            CHECK(controller_focused_node(browser_engine_controller(engine)) == cancel);
            if (capture_directory) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s-youtube-signout.ppm", capture_directory, language_code);
                CHECK(browser_engine_render_frame(engine, path));
            }
        }
        Budget *owned_budget = browser_engine_budget(engine);
        CHECK(owned_budget && browser_engine_shutdown(engine) && owned_budget->current == 0);
        browser_engine_destroy(engine);
        if (lane == 1) fetch_trace_end();
    }
    return true;
}

static const char *real_pack_path(const char *id)
{
    for (size_t at = 0; at < real_pack_count; at++)
        if (!strcmp(real_packs[at].id, id)) return real_packs[at].path;
    return NULL;
}

/* Every glyph of a row must come from the built-in face or the language's own
   pack. Right-to-left rows are asked for the forms the shaper picks. */
static bool real_pack_covers_row(TilefinchGlyphProvider *proof, const FontSet *fonts,
                                 Budget *budget, const char *code, const char *text)
{
    const FontFace *face = font_set_face(fonts, FONT_SANS);
    size_t bytes = strlen(text);
    if (text_bidi_maybe_needed(text, bytes)) {
        TextBidiStatus status;
        TextBidiParagraph *paragraph = text_bidi_paragraph_create(
            budget, text, bytes, TEXT_BIDI_BASE_AUTO_LTR, &status);
        CHECK(paragraph);
        const TextBidiUnit *units = text_bidi_paragraph_units(paragraph);
        bool covered = true;
        for (size_t at = 0; at < text_bidi_paragraph_count(paragraph); at++) {
            unsigned cp = units[at].shaped_codepoint;
            if (cp < 128 || cp == 0x25a1u || font_codepoint_default_ignorable(cp)
                || font_face_has_codepoint(face, cp)) continue;
            unsigned char encoded[4];
            size_t length = tilefinch_utf8_encode(cp, encoded), used;
            uint32_t key;
            if (!tilefinch_glyph_provider_match(proof, (const char *)encoded, length,
                                                &used, &key, NULL)) {
                fprintf(stderr, "real pack missing locale=%s U+%04X in %s\n", code, cp, text);
                covered = false;
            }
        }
        text_bidi_paragraph_destroy(paragraph);
        return covered;
    }
    for (size_t at = 0; at < bytes;) {
        unsigned cp;
        size_t used = font_utf8_next(text + at, bytes - at, &cp);
        CHECK(used);
        /* Square is native controller geometry, separately tested;
           it intentionally is not in the staged Latin subset. */
        if (cp >= 128 && cp != 0x25a1u && !font_codepoint_default_ignorable(cp)
            && !font_face_has_codepoint(face, cp)) {
            uint32_t key;
            if (!tilefinch_glyph_provider_match(proof, text + at, bytes - at, &used, &key, NULL)) {
                fprintf(stderr, "real pack missing locale=%s U+%04X in %s\n", code, cp, text);
                return false;
            }
        }
        at += used;
    }
    return true;
}

static bool test_real_pack_coverage(Budget *budget, const FontSet *fonts)
{
    for (unsigned language = 1; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
        TilefinchGlyphPack pack;
        if (!tilefinch_glyph_pack_for_language(spec->glyphs, &pack)) continue;
        const char *id = tilefinch_glyph_pack_spec(pack)->id;
        const char *path = real_pack_path(id);
        if (!path) continue;
        /* This provider is deliberately not installed. Built-in face coverage
           must not consult the synthetic all-language metric fallback. */
        TilefinchGlyphProvider *proof = tilefinch_glyph_provider_create(budget);
        CHECK(proof && tilefinch_glyph_provider_attach(proof, path, id));
        TilefinchUiTranslation *translation;
        CHECK(load_catalog(budget, language, &translation));
        bool covered = true;
        for (size_t row = 0; row < key_count; row++)
            covered &= real_pack_covers_row(proof, fonts, budget, spec->code, values[row]);
        tilefinch_ui_translation_destroy(translation);
        tilefinch_glyph_provider_destroy(proof);
        CHECK(covered);
    }
    return true;
}

/* Loads a chrome glyph the way the painter does, finishing queued pack
   payloads first so a pending page cannot stand in for the answer. */
static const FontGlyph *settled_chrome_glyph(TilefinchGlyphProvider *provider,
                                             unsigned key)
{
    const FontGlyph *glyph = NULL;
    for (unsigned pass = 0; pass < 64; pass++) {
        bool missing = false;
        glyph = chrome_font_glyph(key, 1, false, &missing);
        if (glyph != NULL || provider == NULL) return glyph;
        bool changed = false;
        size_t read = 0;
        if (!tilefinch_glyph_provider_pump(provider, 65536, &changed, &read) || !read)
            return glyph;
    }
    return glyph;
}

/* A page title in Devanagari drawn before its pack exists caches misses;
   attaching the pack must let the chrome draw it, and a re-arranged
   provider must not serve a cluster key cached under the old one. */
static bool test_chrome_cache_follows_packs(void)
{
    char hindi[64], bengali[64];
    CHECK(make_metric_pack(hindi, false) && make_metric_pack(bengali, true));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
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
    const unsigned ka = 0x0915u;
    CHECK(settled_chrome_glyph(NULL, ka) == NULL);
    uint32_t epoch = chrome_font_epoch;

    /* The lazy path binds an empty provider, then attaches the pack. */
    TilefinchGlyphProvider *first = tilefinch_glyph_provider_create(&budget);
    CHECK(first && font_optional_glyph_provider_install(first));
    CHECK(settled_chrome_glyph(first, ka) == NULL);
    CHECK(tilefinch_glyph_provider_attach(first, hindi, "glyph-ui-test")
          && tilefinch_glyph_provider_attach(first, bengali, "glyph-ui-bn-test"));
    CHECK(settled_chrome_glyph(first, ka) != NULL);
    /* The cached chrome bars and media rasters are keyed by this epoch. */
    CHECK(chrome_font_epoch != epoch);
    unsigned cluster = 0;
    size_t characters = 0;
    CHECK(ui_text_next_glyph("कि", strlen("कि"), &cluster, &characters)
              == strlen("कि") && cluster > 0x10ffffu);
    CHECK(settled_chrome_glyph(first, cluster) != NULL);
    CHECK(font_optional_glyph_provider_uninstall(first));
    tilefinch_glyph_provider_destroy(first);

    /* A replacement provider: nothing loaded under the old arrangement,
       neither a codepoint nor an opaque cluster key, may still be served. */
    TilefinchGlyphProvider *second = tilefinch_glyph_provider_create(&budget);
    CHECK(second && font_optional_glyph_provider_install(second));
    CHECK(settled_chrome_glyph(second, ka) == NULL);
    CHECK(settled_chrome_glyph(second, cluster) == NULL);
    CHECK(tilefinch_glyph_provider_attach(second, bengali, "glyph-ui-bn-test"));
    CHECK(settled_chrome_glyph(second, ka) != NULL);
    CHECK(font_optional_glyph_provider_uninstall(second));
    tilefinch_glyph_provider_destroy(second);
    CHECK(settled_chrome_glyph(NULL, ka) == NULL);
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(unlink(hindi) == 0 && unlink(bengali) == 0 && budget.current == 0);
    return true;
}

/* U+0915 alone: a hollow 16x16 cell with the descent of the deepest real pack. */
static bool make_low_baseline_pack(char path[64], const char *id, unsigned baseline)
{
    enum { PAGES = 80 + TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2,
           PAYLOAD = PAGES + 36, NOTICE = 1, BYTES = PAYLOAD + 32 + NOTICE };
    unsigned char bytes[BYTES] = {0};
    memcpy(bytes, "TFGFv1\0\0", 8);
    put16(bytes + 8, 1);
    bytes[10] = 1; bytes[11] = 16; bytes[12] = 16; bytes[13] = 64;
    bytes[14] = (unsigned char)strlen(id); bytes[15] = (unsigned char)baseline;
    memcpy(bytes + 44, id, strlen(id));
    put32(bytes + 16, 1); put16(bytes + 20, 1);
    put32(bytes + 24, 80); put32(bytes + 28, PAGES);
    put32(bytes + 32, PAYLOAD); put32(bytes + 36, PAYLOAD);
    put32(bytes + 40, BYTES); put32(bytes + 72, NOTICE);
    memset(bytes + 80, 0xff, TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2);
    put16(bytes + 80 + 0x09 * 2, 0);
    bytes[PAGES + 4 + 0x15 / 8] = 1u << (0x15 & 7u);
    for (unsigned y = 0; y < 16; y++) {
        bytes[PAYLOAD + y * 2] = y == 0 || y == 15 ? 255 : 128;
        bytes[PAYLOAD + y * 2 + 1] = y == 0 || y == 15 ? 255 : 1;
    }
    bytes[BYTES - 1] = 15u << 4;
    snprintf(path, 64, "/tmp/tilefinch-ui-low-XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    CHECK(file && fwrite(bytes, 1, BYTES, file) == BYTES && fclose(file) == 0);
    return true;
}

/* Hashes narrow the search, but never establish the identity of a title. */
static bool test_page_text_cache_collision(void)
{
    const char *first = "אב c82b207b1aff19a7";
    const char *second = "אב 6a370ca42ae12b2f";
    char shown_first[256], shown_second[256], expected[256];
    memset(ui_page_text_rows, 0, sizeof(ui_page_text_rows));
    snprintf(shown_first, sizeof(shown_first), "%s",
        ui_page_text_visual(first, 160, 1, NULL, false));
    snprintf(shown_second, sizeof(shown_second), "%s",
        ui_page_text_visual(second, 160, 1, NULL, false));
    Budget scratch;
    budget_init(&scratch, 65536);
    CHECK(psp_ui_visual_text(&scratch, second, 160, 1, NULL, 1,
                             expected, sizeof(expected), NULL));
    CHECK(strcmp(shown_first, expected) != 0);
    CHECK(strcmp(shown_second, expected) == 0 && scratch.current == 0);
    return true;
}

/* A pending pack uses fallback advances. Retrying after its payload arrives
   must measure again even though no pack was attached or replaced. */
static bool test_page_text_cache_pending(void)
{
    char pack[64];
    CHECK(make_low_baseline_pack(pack, "glyph-ui-low-test", 11));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
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
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider && font_optional_glyph_provider_install(provider));
    CHECK(tilefinch_glyph_provider_attach(provider, pack, "glyph-ui-low-test"));
    const char *title = "אבकककककककककककककककककक";
    char pending[256], cached[256], expected[256];
    memset(ui_page_text_rows, 0, sizeof(ui_page_text_rows));
    uint64_t generation = font_optional_glyph_generation();
    snprintf(pending, sizeof(pending), "%s",
        ui_page_text_visual(title, 100, 1, NULL, false));
    CHECK(settled_chrome_glyph(provider, 0x0915) != NULL);
    CHECK(generation == font_optional_glyph_generation());
    snprintf(cached, sizeof(cached), "%s",
        ui_page_text_visual(title, 100, 1, NULL, false));
    Budget scratch;
    budget_init(&scratch, 65536);
    CHECK(psp_ui_visual_text(&scratch, title, 100, 1, NULL, 1,
                             expected, sizeof(expected), NULL));
    CHECK(strcmp(pending, expected) != 0);
    CHECK(strcmp(cached, expected) == 0 && scratch.current == 0);
    /* Once settled, repeated frames retain their measured row. */
    const char *warm = ui_page_text_visual(title, 100, 1, NULL, false);
    uint32_t state = chrome_glyph_state_generation;
    CHECK(warm == ui_page_text_visual(title, 100, 1, NULL, false));
    CHECK(state == chrome_glyph_state_generation);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(unlink(pack) == 0 && budget.current == 0);
    return true;
}

/* Paints one run once its pack page has arrived. */
static bool draw_settled_run(TilefinchGlyphProvider *provider, int y, const char *text, int scale)
{
    for (unsigned pass = 0; pass < 64; pass++) {
        memset(storage, 0, sizeof(storage));
        run_count = 0; run_overflow = false;
        draw_text(storage, WIDTH, HEIGHT, STRIDE, 24, y, text, 64, 0xffff, scale);
        bool changed = false;
        size_t read = 0;
        CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
        if (!read) return run_count == 1;
    }
    return false;
}

static bool row_has_ink(int y, int left, int right)
{
    for (int x = left; x < right; x++)
        if (storage[y * STRIDE + x]) return true;
    return false;
}

/* A pack cell keeps its descent, which reaches below the rows laid out for the
   built-in face. Rows at the screen's edge must rise instead of losing it. */
static bool test_bottom_row_keeps_pack_descent(void)
{
    char pack[64];
    CHECK(make_low_baseline_pack(pack, "glyph-ui-low-test", 11));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget,
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Oblique-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSans-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/DejaVuSerif-Bold-Latin.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Regular.ttf",
        TILEFINCH_TEST_ROOT "/fonts/TilefinchSans-Bold.ttf", 2u * 1024u * 1024u));
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider && tilefinch_glyph_provider_attach(provider, pack, "glyph-ui-low-test")
        && font_optional_glyph_provider_install(provider));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS),
        font_set_face_variant(&fonts, FONT_SANS, false, true), 1);
    static const struct { int y, scale; } edge[] = {{257, 2}, {259, 1}, {271, 2}, {271, 1}};
    for (size_t at = 0; at < sizeof(edge) / sizeof(edge[0]); at++) {
        CHECK(draw_settled_run(provider, edge[at].y, "X \xe0\xa4\x95", edge[at].scale));
        CHECK(!runs[0].missing_glyph && runs[0].bottom <= HEIGHT);
        /* The cell's last row is solid, so it proves the descent was painted. */
        CHECK(runs[0].bottom == HEIGHT && row_has_ink(HEIGHT - 1, runs[0].right - 8, runs[0].right));
    }
    /* The settings heading stays above the hint row under it. */
    for (unsigned pass = 0; pass < 64; pass++) {
        run_count = 0; run_overflow = false;
        draw_ui_settings_heading(storage, WIDTH, HEIGHT, STRIDE,
            (UiRect) {58, 10, WIDTH - 116, 252}, "\xe0\xa4\x95", 0xffff, 0xffff);
        bool changed = false;
        size_t read = 0;
        CHECK(tilefinch_glyph_provider_pump(provider, 65536, &changed, &read));
        if (!read) break;
    }
    CHECK(run_count == 3);
    for (size_t at = 0; at < run_count; at++) CHECK(runs[at].bottom <= 10 + 31);
    /* Away from the edge a run stays exactly where it was asked to be. */
    CHECK(draw_settled_run(provider, 100, "X \xe0\xa4\x95", 2));
    CHECK(runs[0].bottom == 100 + 13 + 5);
    /* The built-in face never moves: same ink at the edge as mid-screen. */
    uint16_t expected[20][120];
    CHECK(draw_settled_run(provider, 100, "X Page", 2));
    int bottom = runs[0].bottom - 100;
    for (int row = 0; row < 20; row++)
        memcpy(expected[row], &storage[(100 + row) * STRIDE], sizeof(expected[row]));
    int edge_y = HEIGHT - bottom;
    CHECK(draw_settled_run(provider, edge_y, "X Page", 2));
    CHECK(runs[0].bottom == HEIGHT);
    for (int row = 0; row < 20 && edge_y + row < HEIGHT; row++)
        CHECK(!memcmp(expected[row], &storage[(edge_y + row) * STRIDE], sizeof(expected[row])));
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    psp_ui_clear_chrome_font();
    font_set_destroy(&fonts);
    CHECK(unlink(pack) == 0 && budget.current == 0);
    return true;
}

static bool test_all_languages(void)
{
    char path[64], bengali_path[64] = {0};
    CHECK(make_metric_pack(path, false));
    const char *real_bengali_pack = real_pack_path("glyph-bengali-assamese");
    if (!real_bengali_pack) CHECK(make_metric_pack(bengali_path, true));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider);
    for (size_t at = 0; at < real_pack_count; at++)
        CHECK(tilefinch_glyph_provider_attach(provider, real_packs[at].path, real_packs[at].id));
    if (!real_bengali_pack)
        CHECK(tilefinch_glyph_provider_attach(provider, bengali_path, "glyph-ui-bn-test"));
    CHECK(tilefinch_glyph_provider_attach(provider, path, "glyph-ui-test")
        && font_optional_glyph_provider_install(provider));
    unsigned cluster_key = 0;
    size_t cluster_characters = 0;
    CHECK(ui_text_next_glyph("कि", strlen("कि"), &cluster_key, &cluster_characters)
              == strlen("कि")
          && cluster_characters == 2 && cluster_key > 0x10ffffu);
    CHECK(ui_text_next_glyph("কি", strlen("কি"), &cluster_key, &cluster_characters)
              == strlen("কি")
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
    CHECK(font_optional_glyph_provider_uninstall(provider));
    CHECK(test_real_pack_coverage(&budget, &fonts));
    CHECK(font_optional_glyph_provider_install(provider));
    psp_ui_set_chrome_fonts(font_set_face(&fonts, FONT_SANS),
        font_set_face_variant(&fonts, FONT_SANS, false, true), 1);
    CHECK(test_square_button_symbol());
    CHECK(test_page_legend_row());
    CHECK(test_bidi_glyph_raster(&fonts, &budget));
    for (unsigned language = 0; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        TilefinchUiTranslation *translation;
        CHECK(load_catalog(&budget, language, &translation));
        tilefinch_ui_translation_bind(translation);
        language_code = tilefinch_ui_language_spec(language)->code;
        if (language == TILEFINCH_UI_LANGUAGE_PORTUGUESE) {
            /* Test the shipped faces directly: the synthetic optional pack
               must not conceal a missing Portuguese accent. */
            for (size_t row = 0; row < key_count; row++) {
                size_t bytes = strlen(values[row]);
                for (size_t at = 0; at < bytes;) {
                    unsigned cp;
                    size_t used = font_utf8_next(values[row] + at, bytes - at, &cp);
                    CHECK(used);
                    at += used;
                    if (cp < 128 || cp >= 0x250) continue;
                    CHECK(font_face_has_codepoint(font_set_face(&fonts, FONT_SANS), cp));
                    CHECK(font_face_has_codepoint(font_set_face_variant(&fonts, FONT_SANS, false, true), cp));
                    CHECK(font_face_has_codepoint(font_set_face(&fonts, FONT_SERIF), cp));
                }
            }
        }
        CHECK(test_provider_language(&fonts, provider));
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
            CHECK(test_player_language(&ui, provider));
            CHECK(test_audio_player_language(
                &ui, provider, theme ? "light" : "dark"));
            CHECK(test_download_language(&ui, provider));
            CHECK(test_page_dialog_language(&ui, provider));
            if (theme == 0) CHECK(test_status_toasts(&ui, provider));
            if (theme == 0) CHECK(test_composed_toasts(&ui, provider));
            if (theme == 0) CHECK(test_state_text(&ui, provider));
            if (theme == 0) CHECK(test_remaining_screens(&ui, provider));
            ui.screen = PSP_UI_SCREEN_PAGE;
            ui.chrome_visible = true;
            ui.focus_label_valid = true;
            static const char *const action_names[] = {
                "Like", "Unlike", "Watch Later", "Remove from Watch Later",
                "Subscribe", "Unsubscribe", "Save to playlist"
            };
            for (unsigned scale = 1; scale <= 2; scale++) {
                ui.browser_ui_scale = scale;
                for (size_t a = 0; a < sizeof(action_names) / sizeof(action_names[0]); a++) {
                    const char *name = tilefinch_ui_logical_text(action_names[a]);
                    CHECK(strlen(name) < sizeof(ui.focus_label));
                    snprintf(ui.focus_label, sizeof(ui.focus_label), "%s", name);
                    CHECK(paint(&ui, provider));
                    bool named = false;
                    /* The name keeps a pair gap before the "O Back" legend. */
                    int back_left = WIDTH;
                    for (size_t r = 0; r < run_count; r++)
                        if (!strcmp(runs[r].text, "O")) back_left = runs[r].left;
                    CHECK(back_left < WIDTH);
                    for (size_t r = 0; r < run_count; r++) {
                        if (!strcmp(runs[r].text, tilefinch_ui_text(action_names[a]))) {
                            if (runs[r].missing_glyph || runs[r].truncated || runs[r].left < 7
                                || runs[r].right > back_left - UI_LEGEND_PAIR_GAP + 2)
                                fprintf(stderr, "focus hint locale=%s scale=%u key=%s ink=%d..%d missing=%u truncated=%u text=%s\n",
                                    language_code, scale, action_names[a], runs[r].left, runs[r].right,
                                    runs[r].missing_glyph, runs[r].truncated, runs[r].text);
                            CHECK(!runs[r].missing_glyph && !runs[r].truncated
                                  && runs[r].left >= 7
                                  && runs[r].right <= back_left - UI_LEGEND_PAIR_GAP + 2);
                            named = true;
                        }
                    }
                    CHECK(named);
                }
                /* A playlist row joins the action to its title. The action
                   half still paints in its pre-shaped display form. */
                CHECK(snprintf(ui.focus_label, sizeof(ui.focus_label), "%s: Episode 1",
                    tilefinch_ui_logical_text("Play from here")) < (int) sizeof(ui.focus_label));
                CHECK(paint(&ui, provider));
                char joined[320];
                snprintf(joined, sizeof(joined), "%s: Episode 1", tilefinch_ui_text("Play from here"));
                bool row_named = false;
                for (size_t r = 0; r < run_count; r++)
                    row_named |= !strcmp(runs[r].text, joined) && !runs[r].missing_glyph;
                if (!row_named)
                    fprintf(stderr, "playlist hint locale=%s scale=%u missing '%s'\n",
                        language_code, scale, joined);
                CHECK(row_named);
            }
            ui.focus_label[0] = '\0';
            ui.focus_label_valid = false;
            ui.browser_ui_scale = 1;
            ui.chrome_visible = false;
            ui.screen = PSP_UI_SCREEN_OPTIONS;
            for (unsigned group = 0; group < UI_SETTINGS_GROUP_COUNT; group++) {
                ui.options_group_selection = group;
                CHECK(paint(&ui, provider));
            }
            if (theme == 0) {
                /* Warm settings frames: how often a retained Unicode glyph
                   is found without scanning the whole table. */
                TilefinchTestFaults *faults = tilefinch_test_faults();
                faults->chrome_unicode_lookups = faults->chrome_unicode_scans
                    = faults->chrome_unicode_scan_hits = 0;
                for (unsigned group = 0; group < UI_SETTINGS_GROUP_COUNT; group++) {
                    ui.options_group_selection = group;
                    CHECK(paint(&ui, provider));
                }
                fprintf(stderr, "chrome unicode lookups: locale=%s settings frames=%u "
                        "lookups=%lu scans=%lu scan-hits=%lu\n", language_code,
                        (unsigned) UI_SETTINGS_GROUP_COUNT, faults->chrome_unicode_lookups,
                        faults->chrome_unicode_scans, faults->chrome_unicode_scan_hits);
            }
            CHECK(snapshot(theme ? "settings-light" : "settings-dark"));
            ui.screen = PSP_UI_SCREEN_OPTION_ITEMS;
            for (unsigned option = 0; option < UI_OPTIONS_ITEM_COUNT; option++) {
                ui.options_selection = option;
                CHECK(paint(&ui, provider));
            }
            /* Appearance with Page zoom, the row after the text size,
               selected. */
            ui.options_group_selection = 0;
            ui.options_selection = UI_OPTION_ZOOM_INDEX;
            ui.page_zoom_percent = 75;
            CHECK(ui_option_display_position(ui.options_selection)
                      == UI_OPTION_FONT_INDEX + 1u
                  && paint(&ui, provider));
            CHECK(snapshot(theme ? "appearance-light" : "appearance-dark"));
            ui.page_zoom_percent = VIEWPORT_PAGE_ZOOM_DEFAULT;
            if (theme == 0) {
                TilefinchTestFaults *faults = tilefinch_test_faults();
                faults->chrome_unicode_lookups = faults->chrome_unicode_scans
                    = faults->chrome_unicode_scan_hits = 0;
                for (unsigned option = 0; option < UI_OPTIONS_ITEM_COUNT; option++) {
                    ui.options_selection = option;
                    CHECK(paint(&ui, provider));
                }
                fprintf(stderr, "chrome unicode lookups: locale=%s option-item frames=%u "
                        "lookups=%lu scans=%lu scan-hits=%lu\n", language_code,
                        (unsigned) UI_OPTIONS_ITEM_COUNT, faults->chrome_unicode_lookups,
                        faults->chrome_unicode_scans, faults->chrome_unicode_scan_hits);
                /* The two-way hint finds a retained glyph without a scan
                   (a direct-mapped one rescanned up to 8% of lookups). */
                CHECK(faults->chrome_unicode_scan_hits * 33u
                      <= faults->chrome_unicode_lookups);
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
                    /* The translated phase keeps its percentage in every
                       language (the Indic phases are 31-35 bytes). */
                    if ((selected == 2 || selected == 3)
                        && (phase == PSP_UI_GLYPH_COMPONENT_DOWNLOADING
                            || phase == PSP_UI_GLYPH_COMPONENT_INSTALLING)) {
                        char progress[96];
                        snprintf(progress, sizeof(progress), "%s 100%%", tilefinch_ui_text(
                            phase == PSP_UI_GLYPH_COMPONENT_DOWNLOADING ? "Downloading" : "Installing"));
                        bool shown = false;
                        for (size_t r = 0; r < run_count; r++)
                            shown |= !strcmp(runs[r].text, progress) && !runs[r].truncated;
                        if (!shown)
                            fprintf(stderr, "pack progress locale=%s phase=%u missing '%s'\n",
                                    language_code, phase, progress);
                        CHECK(shown);
                    }
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
            psp_ui_show_youtube_login_offer(&ui);
            for (unsigned choice = 0; choice < 4u; choice++) {
                ui.menu_selection = (uint8_t) choice;
                CHECK(paint(&ui, provider));
            }
            ui.overlay_animation_frames = 0;
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
    if (bengali_path[0]) CHECK(unlink(bengali_path) == 0);
    CHECK(checked_frames > 2000 && checked_labels > 20000
          && checked_navigation_labels > 1000);
    printf("locale layout: %zu frames, %zu complete labels; no overlap or clipping\n", checked_frames, checked_labels);
    return true;
}

static bool test_menu_metadata_equivalence(void)
{
    static const uint8_t groups[] = {
        [UI_OPTION_WEB_FONTS] = 0,
        [UI_OPTION_CUSTOM_HOMEPAGE] = 1, [UI_OPTION_HISTORY] = 1,
        [UI_OPTION_RESTORE_LAST_PAGE] = 1, [UI_OPTION_TAB_HIBERNATION] = 1,
        [UI_OPTION_TEXT_ENTRY] = 1, [UI_OPTION_ANALOG_CURSOR] = 1,
        [UI_OPTION_GAMEPAD_FACE_MAPPING] = 1, [UI_OPTION_JAVASCRIPT] = 1,
        [UI_OPTION_SEARCH_ENGINE] = 1, [UI_OPTION_BASIC_FALLBACK] = 1,
        [UI_OPTION_HEAVY_PAGES] = 1,
        [UI_OPTION_VIDEO_SCALING] = 2, [UI_OPTION_ROTATE_HINT] = 2,
        [UI_OPTION_YOUTUBE_QUALITY] = 2, [UI_OPTION_VIDEO_LANGUAGE] = 2,
        [UI_OPTION_YOUTUBE_AUDIO_ONLY] = 2, [UI_OPTION_YOUTUBE_RESULTS] = 2,
        [UI_OPTION_VIDEO_STARTUP_BUFFERING] = 2, [UI_OPTION_YOUTUBE_TOPICS] = 2,
        [UI_OPTION_YOUTUBE_LOGIN] = 2, [UI_OPTION_RESUME_DOWNLOADS] = 2,
        [UI_OPTION_SAVE_PLAYBACK_POSITIONS] = 2, [UI_OPTION_CLEAR_PLAYBACK_POSITIONS] = 2,
        [UI_OPTION_CONTENT_BLOCKER] = 3, [UI_OPTION_COSMETIC_HIDING] = 3,
        [UI_OPTION_LOAD_ALLOWLIST] = 3, [UI_OPTION_SITE_DATA_ALLOWED] = 3,
        [UI_OPTION_TLS_SESSION_PERSISTENCE] = 3,
        [UI_OPTION_NETWORK_PROFILE] = 4, [UI_OPTION_WIFI_DIAGNOSTICS] = 4,
        [UI_OPTION_SITE_DATA] = 4, [UI_OPTION_SAVE_DIAGNOSTIC_REPORTS] = 4,
#ifdef TILEFINCH_PSP_POWER_TEST_MENU
        [UI_OPTION_POWER_TEST] = 4, [UI_OPTION_MEDIA_TEST] = 4,
#endif
        [UI_OPTION_UPDATE_CHECK] = 5, [UI_OPTION_UPDATE] = 5,
        [UI_OPTION_EXPERIMENTAL] = 6,
        [UI_OPTION_SITE_JAVASCRIPT] = UI_SETTINGS_GROUP_COUNT,
        [UI_OPTION_COOKIE_BANNERS] = UI_SETTINGS_GROUP_COUNT,
        [UI_OPTION_ALLOW_SITE] = UI_SETTINGS_GROUP_COUNT,
        [UI_OPTION_MIXED_CONTENT_SITE] = UI_SETTINGS_GROUP_COUNT,
        [UI_OPTION_THIRD_PARTY_COOKIES_SITE] = UI_SETTINGS_GROUP_COUNT,
        [UI_OPTION_DIAGNOSTIC_QR] = UI_SETTINGS_GROUP_COUNT
    };
    for (size_t option = 0; option < sizeof(groups); option++)
        CHECK(ui_option_group_index((UiOptionId)option) == groups[option]);
    CHECK(ui_option_group_index((UiOptionId)-1) == 0);
    CHECK(ui_option_group_index(UI_OPTION_YOUTUBE_SIGNIN_DIAGNOSTICS) == 4);
    CHECK(ui_option_group_index(UI_OPTION_YOUTUBE_CHANNEL_BANNERS) == 2);
    CHECK(ui_option_group_index((UiOptionId)(UI_OPTION_YOUTUBE_CHANNEL_BANNERS + 1)) == 0);

    static const unsigned flags[] = {
        PSP_UI_FAILURE_BASIC_VIEW, PSP_UI_FAILURE_RELOAD_BASIC,
        PSP_UI_FAILURE_READER, PSP_UI_FAILURE_WIFI,
        PSP_UI_FAILURE_DISABLE_JAVASCRIPT, PSP_UI_FAILURE_AUDIO_ONLY,
        PSP_UI_FAILURE_LOWER_QUALITY
    };
    static const char *const labels[] = {
        "Show Basic view", "Reload in Basic view", "Try Reader mode",
        "Open Wi-Fi sign-in", "Disable JavaScript for this site",
        "Use audio-only", "Lower video quality"
    };
    static const PspUiAction actions[] = {
        PSP_UI_ACTION_RECOVERY_BASIC, PSP_UI_ACTION_RECOVERY_RELOAD_BASIC,
        PSP_UI_ACTION_RECOVERY_READER, PSP_UI_ACTION_CHECK_WIFI_SIGN_IN,
        PSP_UI_ACTION_RECOVERY_DISABLE_JAVASCRIPT,
        PSP_UI_ACTION_RECOVERY_AUDIO_ONLY, PSP_UI_ACTION_RECOVERY_LOWER_QUALITY
    };
    for (unsigned heavy = 0; heavy < 2; heavy++) {
        for (unsigned mask = 0; mask < 256; mask++) {
            PspUiState ui;
            psp_ui_init(&ui);
            ui.screen = PSP_UI_SCREEN_FAILURE_RECOVERY;
            ui.failure_recovery_heavy = heavy;
            ui.failure_actions = mask;
            const char *expected[9];
            PspUiAction expected_actions[9];
            size_t count = 0;
            if (!heavy) {
                expected[count] = "Retry";
                expected_actions[count++] = PSP_UI_ACTION_RELOAD;
            }
            for (size_t at = 0; at < sizeof(flags) / sizeof(flags[0]); at++) {
                if (mask & flags[at]) {
                    expected[count] = labels[at];
                    expected_actions[count++] = actions[at];
                }
            }
            expected[count] = "Return to the last usable page";
            expected_actions[count++] = PSP_UI_ACTION_RECOVERY_RETURN;
            for (size_t capacity = 0; capacity <= 10; capacity++) {
                const char *actual[12];
                for (size_t at = 0; at < 12; at++) actual[at] = "untouched";
                size_t wanted = count < capacity ? count : capacity;
                CHECK(psp_ui_menu_failure_labels(&ui, actual, capacity) == wanted);
                CHECK(psp_ui_menu_failure_labels(&ui, NULL, capacity) == wanted);
                for (size_t at = 0; at < wanted; at++)
                    CHECK(!strcmp(actual[at], expected[at]));
                for (size_t at = wanted; at < 12; at++)
                    CHECK(!strcmp(actual[at], "untouched"));
            }
            for (size_t selected = 0; selected < count; selected++) {
                PspUiState copy = ui;
                copy.menu_selection = selected;
                PspUiIntent intent = {0};
                CHECK(psp_ui_menu_update(&copy, PSP_UI_BUTTON_CONFIRM, &intent));
                CHECK(intent.action == expected_actions[selected]);
            }
            PspUiIntent intent = {0};
            CHECK(psp_ui_menu_update(&ui, PSP_UI_BUTTON_UP, &intent));
            CHECK(ui.menu_selection == count - 1);
            CHECK(psp_ui_menu_update(&ui, PSP_UI_BUTTON_DOWN, &intent));
            CHECK(ui.menu_selection == 0);
        }
    }
    return true;
}

static bool test_opaque_panel_row_copy(void)
{
    static uint16_t actual[128 * 52 + 4], expected[128 * 52 + 4];
    uint32_t random = 1729;
    for (unsigned sample = 0; sample < 2048; sample++) {
        int width = 16 + sample % 97, height = 8 + sample % 41;
        int stride = width + sample % 10;
        unsigned offset = sample % 2;
        for (size_t at = 0; at < sizeof(actual) / sizeof(actual[0]); at++) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            actual[at] = expected[at] = (uint16_t) (random >> 16);
        }
        UiRect rect = {(int) (sample % 31) - 10, (int) (sample % 19) - 6,
                      30 + (int) (sample % 101), 2 + (int) (sample % 55)};
        uint16_t color = (uint16_t) random;
        unsigned opacity = sample % 6;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                if (x < rect.x || x >= rect.x + rect.width
                    || y < rect.y || y >= rect.y + rect.height) continue;
                size_t at = offset + (size_t) y * stride + x;
                expected[at] = opacity >= 4 ? color
                    : blend565(color, expected[at], opacity * 2);
            }
        }
        fill_rect(actual + offset, width, height, stride, rect, color, opacity);
        CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--page-text-collision-only"))
        return test_page_text_cache_collision() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "--page-text-pending-only"))
        return test_page_text_cache_pending() ? 0 : 1;
    if (argc > 1) capture_directory = argv[1];
    for (int at = 2; at < argc; at++) {
        char *path = strchr(argv[at], '=');
        if (!path || real_pack_count == sizeof(real_packs) / sizeof(real_packs[0])) {
            fprintf(stderr, "bad real pack argument: %s\n", argv[at]);
            return 2;
        }
        *path = '\0';
        real_packs[real_pack_count++] = (RealPack) {argv[at], path + 1};
    }
    return test_opaque_panel_row_copy() && test_menu_metadata_equivalence()
        && test_square_button_symbol() && test_page_text_display_order()
        && test_long_host_keeps_its_end()
        && test_page_text_cache_collision() && test_page_text_cache_pending()
        && test_chrome_cache_follows_packs()
        && test_bottom_row_keeps_pack_descent()
        && test_all_languages() ? 0 : 1;
}
