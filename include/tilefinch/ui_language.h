#ifndef TILEFINCH_UI_LANGUAGE_H
#define TILEFINCH_UI_LANGUAGE_H

#include "tilefinch/budget.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/install_paths.h"

#define TILEFINCH_UI_TRANSLATION_BYTES (32u * 1024u)
#define TILEFINCH_UI_TRANSLATION_ROWS 256u

typedef enum {
    TILEFINCH_UI_LANGUAGE_ENGLISH,
    TILEFINCH_UI_LANGUAGE_SPANISH,
    TILEFINCH_UI_LANGUAGE_FRENCH,
    TILEFINCH_UI_LANGUAGE_GERMAN,
    TILEFINCH_UI_LANGUAGE_JAPANESE,
    TILEFINCH_UI_LANGUAGE_RUSSIAN,
    TILEFINCH_UI_LANGUAGE_UKRAINIAN,
    TILEFINCH_UI_LANGUAGE_CHINESE_SIMPLIFIED,
    TILEFINCH_UI_LANGUAGE_KOREAN,
    TILEFINCH_UI_LANGUAGE_HINDI,
    TILEFINCH_UI_LANGUAGE_ARABIC,
    TILEFINCH_UI_LANGUAGE_COUNT
} TilefinchUiLanguage;

typedef struct {
    const char *code;
    const char *label;
    BrowserGlyphLanguage glyphs;
    const char *sha256;
    unsigned version;
    /* Signed glyph revision needed by this catalog's shaped clusters. */
    unsigned minimum_glyph_sequence;
    /* Immutable data resource relative to tilefinch-models/main. */
    const char *resource_key;
} TilefinchUiLanguageSpec;

typedef struct TilefinchUiTranslation TilefinchUiTranslation;
const TilefinchUiLanguageSpec *tilefinch_ui_language_spec(unsigned language);
bool tilefinch_ui_language_url(unsigned language, char *url, size_t capacity);
TilefinchUiTranslation *tilefinch_ui_translation_create(
    Budget *budget, unsigned language, const void *bytes, size_t length);
void tilefinch_ui_translation_destroy(TilefinchUiTranslation *translation);
const char *tilefinch_ui_translation_text(
    const TilefinchUiTranslation *translation, const char *english);
TilefinchUiTranslation *tilefinch_ui_translation_load(
    Budget *budget, const TilefinchInstallPaths *paths, unsigned language);
/* Only an explicit language selection writes. Verified bytes are staged then
   renamed; a refused/corrupt download never replaces the previous file. */
bool tilefinch_ui_translation_install(
    Budget *budget, const TilefinchInstallPaths *paths, unsigned language,
    const void *bytes, size_t length);
/* Like the theme catalog, bound once at startup and unbound before teardown.
   Language changes apply after restart, never race the chrome supervisor. */
void tilefinch_ui_translation_bind(const TilefinchUiTranslation *translation);
unsigned tilefinch_ui_translation_bound_language(void);
const char *tilefinch_ui_text(const char *english);

#endif
