#include "tilefinch/ui_language.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/file_download.h"
#include "../src/tilefinch_test_faults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int translated_download_pages(Budget *budget)
{
    char root[] = "/tmp/tilefinch-translated-download-XXXXXX";
    CHECK(mkdtemp(root));
    OfflineLibrary library;
    offline_library_init(&library, budget, root);
    library.loaded = true;
    library.count = 1;
    library.items[0] = (OfflineLibraryItem){.id=1, .type=OFFLINE_ITEM_FILE,
        .state=OFFLINE_ITEM_READY, .content_bytes=4};
    strcpy(library.items[0].title, "Settings.zip");
    char path[512];
    CHECK(file_download_path(&library, 1, path, sizeof(path)));
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite("PK\003\004", 1, 4, file)==4 && !fclose(file));
    size_t retained = budget->current, length = 0;
    char *html = NULL;
    CHECK(offline_library_build_file_page(&library, budget, 1, &html, &length));
    char heading[512];
    snprintf(heading, sizeof(heading), "<h1>%s</h1>", tilefinch_ui_logical_text("File saved"));
    CHECK(strstr(html, heading) && strstr(html, "Settings.zip")
        && strstr(html, tilefinch_ui_logical_text("Install homebrew?"))
        && strstr(html, "https://tilefinch.local/offline/extract?id=1"));
    budget_free(budget, html);
    CHECK(offline_library_build_homebrew_page(budget, root, &html, &length));
    snprintf(heading, sizeof(heading), "<h1>%s</h1>", tilefinch_ui_logical_text("Installed homebrew"));
    CHECK(strstr(html, heading) && strstr(html, tilefinch_ui_logical_text("No installed homebrew found.")));
    budget_free(budget, html);
    CHECK(budget->current == retained && !unlink(path) && !rmdir(root));
    return 0;
}

static bool touch_file(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "wb");
    return file && fputs("TFUI1\tes\nSettings\tAjustes\n", file) >= 0 && fclose(file) == 0;
}

static bool present(const char *directory, const char *name)
{
    char path[1024];
    struct stat information;
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    return lstat(path, &information) == 0;
}

/* A verified install removes that language's older catalog versions and
   nothing else: not the current file, other languages, temporaries,
   look-alike names, directories, or symlinks (nor their targets). */
static int install_prunes_older_versions(Budget *budget)
{
    static unsigned char bytes[TILEFINCH_UI_TRANSLATION_BYTES + 1];
    const unsigned es = TILEFINCH_UI_LANGUAGE_SPANISH;
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(es);
    char source[1024];
    snprintf(source, sizeof(source), "%s/%s", TILEFINCH_UI_RESOURCE_ROOT, spec->resource_key);
    FILE *file = fopen(source, "rb");
    CHECK(file);
    size_t length = fread(bytes, 1, sizeof(bytes), file);
    CHECK(!ferror(file) && fclose(file) == 0 && length < sizeof(bytes));
    char root[] = "/tmp/tilefinch-ui-prune-XXXXXX";
    CHECK(mkdtemp(root));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s/data", root);
    char directory[1024], name[64], target[1024], link_path[1024];
    snprintf(directory, sizeof(directory), "%s/ui-language", paths.data_dir);
    CHECK(mkdir(paths.data_dir, 0700) == 0 && mkdir(directory, 0700) == 0);
    unsigned current = spec->version;
    CHECK(current >= 4);
    static const char *const kept[] = {
        "fr-v11.tful", "fr-v12.tful", "es-v11.tful.tmp", "es-v011.tful",
        "es-v11.tful.bak", "xes-v11.tful", "es-v11-tful", "es-v11.tfu",
        "es-v.tful", "es-11.tful", "notes.txt", "es-v99.tful"};
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        CHECK(touch_file(directory, kept[i]));
    for (unsigned version = 1; version < current; version++) {
        if (version == current - 2u || version == current - 3u) continue;
        snprintf(name, sizeof(name), "es-v%u.tful", version);
        CHECK(touch_file(directory, name));
    }
    /* An older name held by a directory, and one by a symlink to a file
       outside the catalog directory. */
    snprintf(name, sizeof(name), "%s/es-v%u.tful", directory, current - 2u);
    CHECK(mkdir(name, 0700) == 0);
    snprintf(target, sizeof(target), "%s/outside.tful", root);
    CHECK(touch_file(root, "outside.tful"));
    snprintf(link_path, sizeof(link_path), "%s/es-v%u.tful", directory, current - 3u);
    CHECK(symlink(target, link_path) == 0);

    CHECK(tilefinch_ui_translation_install(budget, &paths, es, bytes, length));
    snprintf(name, sizeof(name), "es-v%u.tful", current);
    CHECK(present(directory, name) && tilefinch_ui_translation_installed(&paths, es));
    for (unsigned version = 1; version < current - 3u; version++) {
        snprintf(name, sizeof(name), "es-v%u.tful", version);
        CHECK(!present(directory, name));
    }
    snprintf(name, sizeof(name), "es-v%u.tful", current - 1u);
    CHECK(!present(directory, name));
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        if (!present(directory, kept[i])) fprintf(stderr, "removed %s\n", kept[i]);
        CHECK(present(directory, kept[i]));
    }
    struct stat information;
    CHECK(lstat(link_path, &information) == 0 && S_ISLNK(information.st_mode));
    CHECK(present(root, "outside.tful"));
    snprintf(name, sizeof(name), "%s/es-v%u.tful", directory, current - 2u);
    CHECK(lstat(name, &information) == 0 && S_ISDIR(information.st_mode));

    /* Clean up the scratch install. */
    CHECK(rmdir(name) == 0 && unlink(link_path) == 0);
    snprintf(name, sizeof(name), "es-v%u.tful", current);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    CHECK(unlink(path) == 0);
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, kept[i]);
        CHECK(unlink(path) == 0);
    }
    CHECK(unlink(target) == 0 && rmdir(directory) == 0 && rmdir(paths.data_dir) == 0
          && rmdir(root) == 0);
    return 0;
}

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
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_HINDI)->minimum_glyph_sequence == 7);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_BENGALI)->minimum_glyph_sequence == 3);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ASSAMESE)->minimum_glyph_sequence == 3);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ARABIC)->minimum_glyph_sequence == 1);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ARABIC)->glyphs
        == BROWSER_GLYPH_LANGUAGE_ARABIC);
    CHECK(TILEFINCH_UI_LANGUAGE_ARABIC == 10);
    CHECK(TILEFINCH_UI_LANGUAGE_PORTUGUESE == 11);
    CHECK(TILEFINCH_UI_LANGUAGE_BENGALI == 12 && TILEFINCH_UI_LANGUAGE_ASSAMESE == 13);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_BENGALI)->glyphs
        == BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_ASSAMESE)->glyphs
        == BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE);
    CHECK(!strcmp(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_PORTUGUESE)->code, "pt"));
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_PORTUGUESE)->glyphs
        == BROWSER_GLYPH_LANGUAGE_EMBEDDED);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_PORTUGUESE)->version == 12);
    CHECK(tilefinch_ui_language_spec(TILEFINCH_UI_LANGUAGE_SPANISH)->version == 12);
    char url[256];
    CHECK(!tilefinch_ui_language_url(0, url, sizeof(url)));
    CHECK(!tilefinch_ui_language_url(1, url, 4));
    CHECK(!tilefinch_ui_language_url(1, NULL, 256));
    /* The configured models repository (the default here), not a copy. */
    CHECK(tilefinch_ui_language_url(TILEFINCH_UI_LANGUAGE_SPANISH, url, sizeof(url))
          && !strcmp(url, "https://raw.githubusercontent.com/stjanovitz/"
                          "tilefinch-models/main/translations/ui/v12/es.tful"));
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
        size_t before = budget.current;
        TilefinchUiTranslation *t = tilefinch_ui_translation_create(&budget, language, bytes, length);
        CHECK(t);
        /* A loaded catalog holds its file's hashes, key lengths and values
           plus a 16-bit offset per row it actually has (and, for Arabic,
           the display text): nothing scales with the row bound. */
        const unsigned char *count_at = memchr(bytes, '\n', length);
        CHECK(count_at && count_at + 2 < bytes + length);
        size_t rows = (size_t) count_at[1] << 8 | count_at[2];
        CHECK(rows > 0 && rows <= TILEFINCH_UI_TRANSLATION_ROWS);
        if (language != TILEFINCH_UI_LANGUAGE_ARABIC)
            CHECK(budget.current - before <= length + 2u * rows + 256u);
        if (language == TILEFINCH_UI_LANGUAGE_PORTUGUESE) {
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Settings"), "Definições"));
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Portuguese"), "Português"));
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Sign in"), "Entrar"));
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Subtitles"), "Legendas"));
        }
        if (language == TILEFINCH_UI_LANGUAGE_BENGALI || language == TILEFINCH_UI_LANGUAGE_ASSAMESE) {
            CHECK(tilefinch_ui_language_spec(language)->version == 12);
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Bengali"), "বাংলা"));
            CHECK(!strcmp(tilefinch_ui_translation_text(t, "Assamese"), "অসমীয়া"));
        }
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
        /* Hashes like the key "queued" but is longer: stays English. */
        const char *collision = "Untranslated probe 3466157";
        CHECK(tilefinch_ui_translation_text(t, collision) == collision);
        const char *queued = "QUEUED";
        CHECK(tilefinch_ui_translation_text(t, queued) != queued);
        tilefinch_ui_translation_bind(t);
        CHECK(translated_download_pages(&budget) == 0);
        if (language == TILEFINCH_UI_LANGUAGE_ARABIC)
            CHECK(!strcmp(tilefinch_ui_logical_text("Back"), "رجوع"));
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
    CHECK(install_prunes_older_versions(&budget) == 0);
    CHECK(budget.current == 0);
    char root[] = "/tmp/tilefinch-ui-language-XXXXXX";
    CHECK(mkdtemp(root));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s/data", root);
    CHECK(mkdir(paths.data_dir, 0700) == 0);
    unsigned last_language = TILEFINCH_UI_LANGUAGE_COUNT - 1u;
    const TilefinchUiLanguageSpec *last = tilefinch_ui_language_spec(last_language);
    /* A rejected Memory Stick sync after the verified rename is a
       durability hint on firmware that refuses every sync, not a failure. */
    tilefinch_memory_stick_sync_set_host_result(TILEFINCH_MEMORY_STICK_SYNC_REJECTED);
    CHECK(tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    tilefinch_memory_stick_sync_set_host_result(TILEFINCH_MEMORY_STICK_SYNC_SKIPPED);
    TilefinchUiTranslation *installed = tilefinch_ui_translation_load(&budget, &paths, last_language);
    CHECK(installed);
    tilefinch_ui_translation_destroy(installed);
    bytes[0] ^= 1;
    CHECK(!tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    bytes[0] ^= 1;
    installed = tilefinch_ui_translation_load(&budget, &paths, last_language);
    CHECK(installed);
    tilefinch_ui_translation_destroy(installed);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ui-language/%s-v%u.tful", paths.data_dir,
        last->code, last->version);
    /* The language row's installed check verifies the pinned bytes without
       building a catalog: one changed byte is not installed. */
    size_t owned = budget.current;
    CHECK(tilefinch_ui_translation_installed(&paths, last_language)
          && !tilefinch_ui_translation_installed(&paths, TILEFINCH_UI_LANGUAGE_ENGLISH)
          && !tilefinch_ui_translation_installed(&paths, last_language - 1u)
          && budget.current == owned);
    FILE *changed = fopen(path, "r+b");
    CHECK(changed && fseek(changed, 8, SEEK_SET) == 0 && fputc('#', changed) == '#'
          && fclose(changed) == 0);
    CHECK(!tilefinch_ui_translation_installed(&paths, last_language));
    /* Reinstalling over the stale same-version file must replace it even
       where rename cannot replace an existing file (PSP FAT). */
    tilefinch_test_faults()->replace_file_no_replace = true;
    CHECK(tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    tilefinch_test_faults()->replace_file_no_replace = false;
    CHECK(tilefinch_ui_translation_installed(&paths, last_language));
    /* The same holds for any entry a host rename also refuses to replace:
       an empty directory squatting on the catalog's name. */
    CHECK(unlink(path) == 0 && mkdir(path, 0700) == 0);
    CHECK(tilefinch_ui_translation_install(&budget, &paths, last_language, bytes, length));
    CHECK(tilefinch_ui_translation_installed(&paths, last_language));
    CHECK(unlink(path) == 0);
    CHECK(!tilefinch_ui_translation_installed(&paths, last_language));
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
