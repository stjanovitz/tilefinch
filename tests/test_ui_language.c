#include "tilefinch/ui_language.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    static unsigned char bytes[TILEFINCH_UI_TRANSLATION_BYTES + 1];
    CHECK(!tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_COUNT));
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_JAPANESE)->glyphs
        == BROWSER_GLYPH_LANGUAGE_JAPANESE);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_RUSSIAN)->glyphs
        == BROWSER_GLYPH_LANGUAGE_CYRILLIC);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_UKRAINIAN)->glyphs
        == BROWSER_GLYPH_LANGUAGE_CYRILLIC);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_CHINESE_SIMPLIFIED)->glyphs
        == BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_KOREAN)->glyphs
        == BROWSER_GLYPH_LANGUAGE_KOREAN);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_HINDI)->glyphs
        == BROWSER_GLYPH_LANGUAGE_DEVANAGARI);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_HINDI)->minimum_glyph_sequence == 2);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ARABIC)->minimum_glyph_sequence == 1);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ARABIC)->glyphs
        == BROWSER_GLYPH_LANGUAGE_ARABIC);
    char url[256];
    CHECK(!tilefinch_ui_language_url(0, url, sizeof(url)));
    CHECK(!tilefinch_ui_language_url(1, url, 4));
    CHECK(!tilefinch_ui_language_url(1, NULL, 256));
    size_t length = 0;
    for (unsigned language = 1; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
        char source[1024];
        char expected_key[128];
        snprintf(expected_key, sizeof(expected_key), "translations/ui/v%u/%s.tful",
            spec->version, spec->code);
        CHECK(spec->resource_key && !strcmp(spec->resource_key, expected_key));
        snprintf(source, sizeof(source), "%s/%s",
            TILEFINCH_UI_RESOURCE_ROOT, spec->resource_key);
        CHECK(tilefinch_ui_language_url(language, url, sizeof(url))
            && strstr(url, spec->code));
        char expected_url[256];
        snprintf(expected_url, sizeof(expected_url),
            "https://raw.githubusercontent.com/stjanovitz/tilefinch-models/main/translations/ui/v%u/%s.tful",
            spec->version, spec->code);
        CHECK(strcmp(url, expected_url) == 0);
        FILE *file = fopen(source, "rb");
        CHECK(file);
        length = fread(bytes, 1, sizeof(bytes), file);
        CHECK(!ferror(file) && fclose(file) == 0 && length < sizeof(bytes));
        bytes[length] = 0;
        TilefinchUiTranslation *t = tilefinch_ui_translation_create(&budget, language, bytes, length);
        CHECK(t);
        if (language == TILEFINCH_UI_LANGUAGE_ARABIC) {
            /* Arabic labels are shaped and visually ordered exactly once. */
            const char *settings = tilefinch_ui_translation_text(t, "Settings");
            CHECK(strcmp(settings, "الإعدادات") != 0);
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Back"), "ﻉﻮﺟﺭ"));
            size_t owned = budget.current;
            for (unsigned read = 0; read < 1000; read++)
                CHECK(tilefinch_ui_translation_text(t, "Settings") == settings);
            CHECK(budget.current == owned);
        }
        CHECK(strcmp(tilefinch_ui_translation_text(t, "Settings"), "Settings"));
        CHECK(!strcmp(tilefinch_ui_translation_text(t, "settings"), tilefinch_ui_translation_text(t, "Settings")));
        CHECK(!strcmp(tilefinch_ui_translation_text(t, "Unknown advanced message"), "Unknown advanced message"));
        tilefinch_ui_translation_bind(t);
        CHECK(!strcmp(tilefinch_ui_text("Settings"), tilefinch_ui_translation_text(t, "Settings")));
        bytes[length-2] ^= 1;
        CHECK(!tilefinch_ui_translation_create(&budget, language, bytes, length));
        bytes[length-2] ^= 1;
        CHECK(!tilefinch_ui_translation_create(&budget, language, bytes, length-1));
        CHECK(!tilefinch_ui_translation_create(&budget, language, bytes, sizeof(bytes)));
        size_t limit = budget.limit;
        budget.limit = budget.current;
        CHECK(!tilefinch_ui_translation_create(&budget, language, bytes, length));
        budget.limit = limit;
        tilefinch_ui_translation_bind(NULL);
        tilefinch_ui_translation_destroy(t);
        CHECK(budget.current == 0);
        if (language == TILEFINCH_UI_LANGUAGE_ARABIC) {
            /* Refuse the extra display cache after admitting the catalog. */
            budget.limit = length + 4096;
            CHECK(!tilefinch_ui_translation_create(&budget, language, bytes, length));
            CHECK(budget.current == 0);
            budget.limit = limit;
        }
    }
    char root[] = "/tmp/tilefinch-ui-language-XXXXXX";
    CHECK(mkdtemp(root));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s/data", root);
    CHECK(mkdir(paths.data_dir, 0700) == 0);
    unsigned last_language = TILEFINCH_UI_LANGUAGE_COUNT - 1u;
    const TilefinchUiLanguageSpec *last = tilefinch_ui_language_spec(last_language);
    CHECK(tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    TilefinchUiTranslation *installed = tilefinch_ui_translation_load(&budget, &paths, last_language);
    CHECK(installed);
    tilefinch_ui_translation_destroy(installed);
    bytes[0] ^= 1;
    CHECK(!tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    installed = tilefinch_ui_translation_load(&budget, &paths, last_language);
    CHECK(installed);
    tilefinch_ui_translation_destroy(installed);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ui-language/%s-v%u.tful", paths.data_dir,
        last->code, last->version);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/ui-language", paths.data_dir);
    CHECK(rmdir(path) == 0);
    BrowserProfile *profile = browser_profile_create(&budget);
    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(profile && loaded && browser_profile_ui_language(profile) == 0);
    snprintf(path, sizeof(path), "%s/profile", root);
    for (unsigned language = 0; language < TILEFINCH_UI_LANGUAGE_COUNT; language++) {
        browser_profile_set_ui_language(profile, language);
        browser_profile_set_ui_language(profile, UINT32_MAX);
        CHECK(browser_profile_ui_language(profile) == language);
        CHECK(browser_profile_save(profile, path) && browser_profile_load(loaded, path));
        CHECK(browser_profile_ui_language(loaded) == language);
    }
    browser_profile_destroy(profile);
    browser_profile_destroy(loaded);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/profile.bak", root);
    CHECK(unlink(path) == 0 && rmdir(paths.data_dir) == 0 && rmdir(root) == 0);
    CHECK(budget.current == 0);
    CHECK(!strcmp(tilefinch_ui_text("Settings"), "Settings"));
    return 0;
}
