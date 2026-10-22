/* After an update moves the interface catalogs to a new version, a saved
   non-English language whose current catalog or glyph pack is missing
   falls back to English at boot. These tests drive the once-per-boot check
   the browser runs (src/psp_ui_language_repair.c) against real catalog
   bytes in a scratch install, and the offer it raises through the real
   PSP UI state machine. */
#include "tilefinch/psp_ui_language_repair.h"
#include "tilefinch/ui_language.h"
#include "tilefinch/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
    return false; } } while (0)

static unsigned char catalog[TILEFINCH_UI_TRANSLATION_BYTES + 1];

static size_t read_catalog(unsigned language)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    char source[1024];
    snprintf(source, sizeof(source), "%s/%s", TILEFINCH_UI_RESOURCE_ROOT,
             spec->resource_key);
    FILE *file = fopen(source, "rb");
    if (file == NULL) return 0;
    size_t length = fread(catalog, 1, sizeof(catalog), file);
    if (ferror(file) || fclose(file) != 0 || length >= sizeof(catalog))
        return 0;
    return length;
}

static bool catalog_path(const TilefinchInstallPaths *paths,
                         unsigned language, unsigned version,
                         char *path, size_t capacity)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    int n = snprintf(path, capacity, "%s/ui-language/%s-v%u.tful",
                     paths->data_dir, spec->code, version);
    return n > 0 && (size_t) n < capacity;
}

static bool install_catalog(Budget *budget, const TilefinchInstallPaths *paths,
                            unsigned language)
{
    size_t length = read_catalog(language);
    return length != 0 && tilefinch_ui_translation_install(
        budget, paths, language, catalog, length);
}

/* The previous version's file, as an older build left it. */
static bool write_old_catalog(const TilefinchInstallPaths *paths,
                              unsigned language)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    char directory[TILEFINCH_INSTALL_PATH_LIMIT], path[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(directory, sizeof(directory), "%s/ui-language", paths->data_dir);
    (void) mkdir(directory, 0700);
    if (!catalog_path(paths, language, spec->version - 1u, path, sizeof(path)))
        return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool okay = fprintf(file, "TFUI1\t%s\nSettings\tAjustes\n", spec->code) > 0;
    return fclose(file) == 0 && okay;
}

static bool remove_catalog(const TilefinchInstallPaths *paths,
                           unsigned language, unsigned version)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    return catalog_path(paths, language, version, path, sizeof(path))
        && (unlink(path) == 0 || access(path, F_OK) != 0);
}

/* A signed Devanagari pack at `sequence`, as boot attachment records it. */
static void set_devanagari(PspGlyphComponentSession *glyphs,
                           uint64_t sequence)
{
    memset(glyphs, 0, sizeof(*glyphs));
    TilefinchGlyphPack pack;
    if (sequence != 0
        && tilefinch_glyph_pack_for_language(BROWSER_GLYPH_LANGUAGE_DEVANAGARI,
                                             &pack)) {
        glyphs->installed_mask = (uint16_t) (1u << (unsigned) pack);
        glyphs->installed_sequences[pack] = sequence;
    }
}

static bool test_detection_matrix(Budget *budget,
                                  const TilefinchInstallPaths *paths)
{
    static PspGlyphComponentSession glyphs;
    const unsigned es = TILEFINCH_UI_LANGUAGE_SPANISH;
    const unsigned hi = TILEFINCH_UI_LANGUAGE_HINDI;
    const unsigned es_version = tilefinch_ui_language_spec(es)->version;
    set_devanagari(&glyphs, 0);

    /* English needs nothing, whatever is on the stick. */
    CHECK(psp_ui_language_repair_needed(
        paths, &glyphs, TILEFINCH_UI_LANGUAGE_ENGLISH, false)
          == PSP_UI_LANGUAGE_REPAIR_NONE);

    /* Spanish with no catalog at all, then with only the old version. */
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_CATALOG);
    CHECK(write_old_catalog(paths, es));
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_CATALOG);

    /* Spanish with the current catalog: nothing to offer. */
    CHECK(install_catalog(budget, paths, es));
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_NONE);
    /* A catalog that loaded is never second-guessed; nor is an unslotted
       install, which has no Memory Stick data directory to check. */
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, true)
          == PSP_UI_LANGUAGE_REPAIR_NONE);
    TilefinchInstallPaths unslotted = *paths;
    unslotted.slotted = false;
    CHECK(psp_ui_language_repair_needed(&unslotted, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_NONE);

    /* A damaged current catalog (one changed byte, or cut short). */
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(catalog_path(paths, es, es_version, path, sizeof(path)));
    FILE *changed = fopen(path, "r+b");
    CHECK(changed && fseek(changed, 12, SEEK_SET) == 0
          && fputc('#', changed) == '#' && fclose(changed) == 0);
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_CATALOG);
    CHECK(install_catalog(budget, paths, es) && truncate(path, 100) == 0);
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, es, false)
          == PSP_UI_LANGUAGE_REPAIR_CATALOG);

    /* Hindi: a missing catalog is named even with an old pack, since its
       download fetches the pack after it. */
    set_devanagari(&glyphs, 1);
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, hi, false)
          == PSP_UI_LANGUAGE_REPAIR_CATALOG);
    /* With the current catalog, a missing or older pack is the font. */
    CHECK(install_catalog(budget, paths, hi));
    set_devanagari(&glyphs, 0);
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, hi, false)
          == PSP_UI_LANGUAGE_REPAIR_FONT_PACK);
    unsigned required =
        tilefinch_ui_language_spec(hi)->minimum_glyph_sequence;
    CHECK(required > 1u);
    set_devanagari(&glyphs, required - 1u);
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, hi, false)
          == PSP_UI_LANGUAGE_REPAIR_FONT_PACK);
    CHECK(!psp_ui_language_pack_current(&glyphs, hi));
    set_devanagari(&glyphs, required);
    CHECK(psp_ui_language_pack_current(&glyphs, hi));
    CHECK(psp_ui_language_repair_needed(paths, &glyphs, hi, false)
          == PSP_UI_LANGUAGE_REPAIR_NONE);
    /* Spanish needs no pack at all. */
    TilefinchGlyphPack pack;
    CHECK(!psp_ui_language_pack(es, &pack) && psp_ui_language_pack(hi, &pack));

    CHECK(remove_catalog(paths, es, es_version)
          && remove_catalog(paths, es, es_version - 1u)
          && remove_catalog(paths, hi, tilefinch_ui_language_spec(hi)->version));
    return true;
}

static PspUiIntent press(PspUiState *ui, uint32_t button, unsigned elapsed)
{
    PspUiInput input = {.pressed = button, .analog_x = 128, .analog_y = 128,
                        .elapsed_ms = elapsed};
    return psp_ui_update(ui, &input);
}

/* Once per boot: it waits out a menu and app work without checking, shows
   the offer when the UI is idle, and never again after an answer. */
static bool test_offer_once_per_boot(Budget *budget,
                                     const TilefinchInstallPaths *paths)
{
    static PspGlyphComponentSession glyphs;
    const unsigned es = TILEFINCH_UI_LANGUAGE_SPANISH;
    const unsigned hi = TILEFINCH_UI_LANGUAGE_HINDI;
    memset(&glyphs, 0, sizeof(glyphs));
    CHECK(write_old_catalog(paths, es));
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_show_home(&ui);
    ui.toast_frames = 0;
    bool checked = false;

    /* A menu over Home, then app work: no toast, and no check yet. */
    ui.screen = PSP_UI_SCREEN_MENU;
    CHECK(!psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, false));
    CHECK(!checked && ui.toast_frames == 0);
    ui.screen = PSP_UI_SCREEN_HOME;
    psp_ui_show_status(&ui, "CONNECTED", 30);
    CHECK(!psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, false));
    CHECK(!checked && strcmp(ui.status, "CONNECTED") == 0);
    ui.toast_frames = 0;
    CHECK(!psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, true));
    CHECK(!checked && ui.toast_frames == 0);

    /* Idle: the offer for the missing catalog. */
    CHECK(psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, false));
    CHECK(checked && ui.toast_kind == PSP_UI_TOAST_LANGUAGE_REPAIR
          && ui.toast_payload == es
          && strcmp(ui.status, "Interface text needs a download") == 0);
    /* O dismisses it; it does not come back this session. */
    (void) press(&ui, PSP_UI_BUTTON_CANCEL, 16);
    CHECK(ui.toast_frames == 0 && ui.toast_kind == PSP_UI_TOAST_PLAIN);
    for (int frame = 0; frame < 3; frame++) {
        CHECK(!psp_ui_language_repair_offer_once(
            &checked, &ui, paths, &glyphs, es, false, false));
        CHECK(ui.toast_frames == 0);
    }

    /* The next boot asks again while the catalog is still missing. */
    checked = false;
    CHECK(psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, false));
    (void) press(&ui, PSP_UI_BUTTON_CANCEL, 16);

    /* Nothing missing: checked once, nothing shown. */
    checked = false;
    CHECK(!psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, TILEFINCH_UI_LANGUAGE_ENGLISH,
        false, false));
    CHECK(checked && ui.toast_frames == 0);
    CHECK(install_catalog(budget, paths, es));
    checked = false;
    CHECK(!psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, es, false, false));
    CHECK(checked && ui.toast_frames == 0);

    /* Hindi's old pack names the font pack; X opens its language row. */
    CHECK(install_catalog(budget, paths, hi));
    checked = false;
    CHECK(psp_ui_language_repair_offer_once(
        &checked, &ui, paths, &glyphs, hi, false, false));
    CHECK(strcmp(ui.status, "Needs a font pack update") == 0
          && ui.toast_payload == hi);
    for (int frame = 0; frame < 7; frame++) (void) press(&ui, 0, 100);
    PspUiIntent intent = press(&ui, PSP_UI_BUTTON_CONFIRM, 16);
    CHECK(ui.screen == PSP_UI_SCREEN_GLYPH_OPTIONS
          && ui.glyph_options_selection == 6u && ui.ui_language == hi
          && intent.glyph_component_probe_requested);
    intent = press(&ui, PSP_UI_BUTTON_CONFIRM, 16);
    CHECK(intent.setting.id == PSP_UI_SETTING_UI_LANGUAGE
          && intent.setting.value.unsigned_value == hi);
    CHECK(remove_catalog(paths, es, tilefinch_ui_language_spec(es)->version)
          && remove_catalog(paths, es, tilefinch_ui_language_spec(es)->version - 1u)
          && remove_catalog(paths, hi, tilefinch_ui_language_spec(hi)->version));
    return true;
}

int main(void)
{
    static Budget budget;
    budget_init(&budget, 1024u * 1024u);
    char root[] = "/tmp/tilefinch-ui-language-repair-XXXXXX";
    if (mkdtemp(root) == NULL) return 1;
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s/data", root);
    if (mkdir(paths.data_dir, 0700) != 0) return 1;
    tilefinch_memory_stick_sync_set_host_result(
        TILEFINCH_MEMORY_STICK_SYNC_SKIPPED);
    bool okay = test_detection_matrix(&budget, &paths)
        && test_offer_once_per_boot(&budget, &paths)
        && budget.current == 0;
    char directory[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(directory, sizeof(directory), "%s/ui-language", paths.data_dir);
    (void) rmdir(directory);
    (void) rmdir(paths.data_dir);
    (void) rmdir(root);
    if (!okay) return 1;
    puts("psp-ui-language-repair-tests: ok");
    return 0;
}
