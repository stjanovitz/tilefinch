#include "tilefinch/ui_language.h"
#include "tilefinch/sha256.h"
#include "tilefinch/text_bidi.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef __PSP__
#include <pspiofilemgr.h>
#endif

#include "generated/ui_languages.inc"

struct TilefinchUiTranslation {
    Budget *budget;
    size_t count;
    unsigned language;
    char *visual_text;
    struct { uint16_t key, value, visual; } rows[TILEFINCH_UI_TRANSLATION_ROWS];
    char text[];
};

static const TilefinchUiTranslation *bound_translation;

/* Shape and order fixed RTL labels once while loading, never in the painter
   or its supervisor thread. Logical catalog bytes remain available for keys
   and verification; the bounded display cache is immutable after creation. */
static bool translation_prepare_visual_text(TilefinchUiTranslation *t)
{
    t->visual_text = budget_malloc(t->budget, TILEFINCH_UI_TRANSLATION_BYTES);
    if (!t->visual_text) return false;
    size_t at = 0;
    for (size_t row = 0; row < t->count; row++) {
        const char *value = t->text + t->rows[row].value;
        size_t length = strlen(value);
        if (!text_bidi_maybe_needed(value, length)) continue;
        TextBidiStatus status;
        TextBidiParagraph *p = text_bidi_paragraph_create(
            t->budget, value, length, TEXT_BIDI_BASE_RTL, &status);
        if (!p) return false;
        size_t count = text_bidi_paragraph_count(p);
        uint16_t order[256];
        bool okay = count <= sizeof(order) / sizeof(order[0])
            && text_bidi_line_visual_order(p, 0, count, order,
                                          sizeof(order) / sizeof(order[0]), NULL);
        const TextBidiUnit *units = text_bidi_paragraph_units(p);
        size_t start = at;
        for (size_t i = 0; okay && i < count; i++) {
            uint32_t cp = units[order[i]].shaped_codepoint;
            unsigned char out[4];
            size_t bytes;
            if (cp < 0x80u) { out[0] = (unsigned char)cp; bytes = 1; }
            else if (cp < 0x800u) {
                out[0] = 0xc0u | (cp >> 6); out[1] = 0x80u | (cp & 63u); bytes = 2;
            } else if (cp < 0x10000u) {
                out[0] = 0xe0u | (cp >> 12); out[1] = 0x80u | ((cp >> 6) & 63u);
                out[2] = 0x80u | (cp & 63u); bytes = 3;
            } else {
                out[0] = 0xf0u | (cp >> 18); out[1] = 0x80u | ((cp >> 12) & 63u);
                out[2] = 0x80u | ((cp >> 6) & 63u); out[3] = 0x80u | (cp & 63u); bytes = 4;
            }
            if (bytes >= TILEFINCH_UI_TRANSLATION_BYTES - at) { okay = false; break; }
            memcpy(t->visual_text + at, out, bytes);
            at += bytes;
        }
        text_bidi_paragraph_destroy(p);
        if (!okay) return false;
        t->visual_text[at++] = 0;
        t->rows[row].visual = (uint16_t)start;
    }
    if (at) {
        char *smaller = budget_realloc(t->budget, t->visual_text, at);
        if (smaller) t->visual_text = smaller;
    }
    return true;
}

const TilefinchUiLanguageSpec *tilefinch_ui_language_spec(unsigned language)
{
    return language < TILEFINCH_UI_LANGUAGE_COUNT ? &ui_languages[language] : NULL;
}

bool tilefinch_ui_language_url(unsigned language, char *url, size_t capacity)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    if (spec == NULL || !spec->resource_key || !url || !capacity) return false;
    int n = snprintf(url, capacity,
        "https://raw.githubusercontent.com/stjanovitz/tilefinch-models/main/%s",
        spec->resource_key);
    return n > 0 && (size_t)n < capacity;
}

TilefinchUiTranslation *tilefinch_ui_translation_create(
    Budget *budget, unsigned language, const void *bytes, size_t length)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    if (!budget || !bytes || !spec || !spec->sha256 || !length
        || length > TILEFINCH_UI_TRANSLATION_BYTES || memchr(bytes, 0, length)) return NULL;
    uint8_t digest[32];
    char hex[65];
    if (!tilefinch_sha256_digest(bytes, length, digest)) return NULL;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        hex[2*i] = digits[digest[i] >> 4];
        hex[2*i+1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    if (strcmp(hex, spec->sha256)) return NULL;
    TilefinchUiTranslation *t = budget_malloc(budget, sizeof(*t) + length + 1);
    if (!t) return NULL;
    t->budget = budget;
    t->count = 0;
    t->language = language;
    t->visual_text = NULL;
    memcpy(t->text, bytes, length);
    t->text[length] = 0;
    char header[32];
    int header_length = snprintf(header, sizeof(header), "TFUI1\t%s\n", spec->code);
    if (header_length <= 0 || (size_t)header_length >= length
        || memcmp(t->text, header, (size_t)header_length)) goto invalid;
    size_t at = (size_t)header_length;
    while (at < length) {
        char *key = t->text + at;
        char *end = memchr(key, '\n', length - at);
        if (!end || t->count == TILEFINCH_UI_TRANSLATION_ROWS) goto invalid;
        char *tab = memchr(key, '\t', (size_t)(end-key));
        if (!tab || tab == key || tab + 1 == end
            || memchr(tab + 1, '\t', (size_t)(end-tab-1))) goto invalid;
        *tab = 0; *end = 0;
        /* These are display strings, not author-controlled printf programs. */
        if (strchr(key, '%') || strchr(tab + 1, '%')) goto invalid;
        if (t->count && strcasecmp(t->text + t->rows[t->count-1].key, key) >= 0) goto invalid;
        t->rows[t->count].key = (uint16_t)at;
        t->rows[t->count].visual = UINT16_MAX;
        t->rows[t->count++].value = (uint16_t)(tab + 1 - t->text);
        at = (size_t)(end - t->text) + 1;
    }
    if (!t->count) goto invalid;
    if (language == TILEFINCH_UI_LANGUAGE_ARABIC
        && !translation_prepare_visual_text(t)) goto invalid;
    return t;
invalid:
    budget_free(budget, t->visual_text);
    budget_free(budget, t);
    return NULL;
}

void tilefinch_ui_translation_destroy(TilefinchUiTranslation *t)
{
    if (t) {
        budget_free(t->budget, t->visual_text);
        budget_free(t->budget, t);
    }
}

const char *tilefinch_ui_translation_text(const TilefinchUiTranslation *t, const char *english)
{
    if (!t || !english) return english;
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        int order = strcasecmp(english, t->text + t->rows[mid].key);
        if (!order) return t->rows[mid].visual != UINT16_MAX
            ? t->visual_text + t->rows[mid].visual : t->text + t->rows[mid].value;
        if (order < 0) hi = mid; else lo = mid + 1;
    }
    return english;
}

static bool translation_path(const TilefinchInstallPaths *paths, unsigned language,
                             char *output, size_t capacity)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    char relative[96];
    if (!paths || !paths->slotted || !spec || !spec->sha256) return false;
    int n = snprintf(relative, sizeof(relative), "ui-language/%s-v%u.tful", spec->code, spec->version);
    return n > 0 && (size_t)n < sizeof(relative)
        && tilefinch_install_data_path(paths, relative, output, capacity);
}

TilefinchUiTranslation *tilefinch_ui_translation_load(
    Budget *budget, const TilefinchInstallPaths *paths, unsigned language)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!translation_path(paths, language, path, sizeof(path))) return NULL;
    struct stat information;
    if (lstat(path, &information) != 0 || !S_ISREG(information.st_mode)
        || information.st_size <= 0
        || (uint64_t)information.st_size > TILEFINCH_UI_TRANSLATION_BYTES) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = (size_t)information.st_size + 1;
    unsigned char *bytes = budget_malloc(budget, capacity);
    size_t length = bytes ? fread(bytes, 1, capacity, file) : 0;
    bool okay = !ferror(file);
    if (fclose(file) != 0) okay = false;
    TilefinchUiTranslation *t = okay && bytes
        ? tilefinch_ui_translation_create(budget, language, bytes, length) : NULL;
    budget_free(budget, bytes);
    return t;
}

bool tilefinch_ui_translation_install(Budget *budget, const TilefinchInstallPaths *paths,
    unsigned language, const void *bytes, size_t length)
{
    TilefinchUiTranslation *t = tilefinch_ui_translation_create(budget, language, bytes, length);
    if (!t) return false;
    tilefinch_ui_translation_destroy(t);
    char path[TILEFINCH_INSTALL_PATH_LIMIT], temp[TILEFINCH_INSTALL_PATH_LIMIT];
    char directory[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!translation_path(paths, language, path, sizeof(path))
        || !tilefinch_install_data_path(paths, "ui-language", directory, sizeof(directory))) return false;
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) return false;
    struct stat information;
    if (lstat(directory, &information) != 0 || !S_ISDIR(information.st_mode)) return false;
    int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(temp)) return false;
    /* Discard only our fixed temporary name. Exclusive creation cannot
       follow an attacker-replaced symlink on host development installs. */
    if (unlink(temp) != 0 && errno != ENOENT) return false;
    int fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    if (!file) { close(fd); (void)remove(temp); return false; }
    bool okay = fwrite(bytes, 1, length, file) == length && fflush(file) == 0;
#ifndef __PSP__
    if (okay) okay = fsync(fileno(file)) == 0;
#endif
    if (fclose(file) != 0) okay = false;
    if (okay) okay = rename(temp, path) == 0;
#ifdef __PSP__
    if (okay) okay = sceIoSync("ms0:", 0) >= 0;
#endif
    if (!okay) (void)remove(temp);
    return okay;
}

void tilefinch_ui_translation_bind(const TilefinchUiTranslation *translation)
{ bound_translation = translation; }
unsigned tilefinch_ui_translation_bound_language(void)
{ return bound_translation ? bound_translation->language : TILEFINCH_UI_LANGUAGE_ENGLISH; }
const char *tilefinch_ui_text(const char *english)
{ return tilefinch_ui_translation_text(bound_translation, english); }
