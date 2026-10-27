#include "tilefinch/ui_language.h"
#include "tilefinch/sha256.h"
#include "tilefinch/text_bidi.h"
#include "tilefinch/platform.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "generated/ui_languages.inc"
#include "fnv1a.h"

/* Interface translations live in the models repository beside the signed
   glyph packs, so a build that overrides that repository fetches both from
   it (cmake/TilefinchCore.cmake). */
#ifndef TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER
#define TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER "stjanovitz"
#endif
#ifndef TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME
#define TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME "tilefinch-models"
#endif

/* A catalog file ("TFUI2", tools/generate_ui_translations.py) stores no
   English keys. Its integers are big-endian:
     "TFUI2\t<code>\n"
     u16 row count, 1..TILEFINCH_UI_TRANSLATION_ROWS
     u32 key hash per row, strictly increasing: FNV-1a of the ASCII-lowered
         English key, which is exactly what strcasecmp matched
     u8  English key byte length per row, nonzero
     the rows' values in order, each 1..TILEFINCH_UI_TRANSLATION_VALUE_BYTES
         printable bytes (no control byte or '%') ending in a line feed,
         and nothing after the last one.
   One allocation holds this header, the decoded hashes, the value offsets,
   the key lengths and the values. */
struct TilefinchUiTranslation {
    Budget *budget;
    const uint32_t *hashes;
    const uint16_t *values;
    const uint8_t *key_lengths;
    const char *text;
    /* Arabic only: one allocation of a display offset per row (UINT16_MAX
       when the value is used as is), followed by the display text. */
    uint16_t *visual;
    const char *visual_text;
    size_t count;
    unsigned language;
};

static const TilefinchUiTranslation *bound_translation;

/* Shaping maps a two-byte Arabic letter to a three-byte presentation form,
   and mirroring keeps ASCII brackets ASCII, so a character's display bytes
   never exceed max(its own bytes, 3). The fill below still checks. */
static size_t translation_visual_bound(const char *value)
{
    size_t bytes = 1; /* NUL */
    for (const unsigned char *c = (const unsigned char *) value; *c; c++)
        bytes += *c < 0x80u ? 1u : (*c < 0xc0u ? 0u : (*c < 0xf0u ? 3u : 4u));
    return bytes;
}

/* Shape and order fixed RTL labels once while loading, never in the painter
   or its supervisor thread. Logical catalog bytes remain available for
   verification; the bounded display cache is immutable after creation. Its
   allocation is the bound from the value bytes, trimmed to what was used. */
static bool translation_prepare_visual_text(TilefinchUiTranslation *t)
{
    size_t bound = 0;
    for (size_t row = 0; row < t->count; row++) {
        const char *value = t->text + t->values[row];
        if (text_bidi_maybe_needed(value, strlen(value)))
            bound += translation_visual_bound(value);
    }
    if (bound == 0) return true;
    /* Display offsets are 16-bit, like the catalog's own. */
    if (bound > TILEFINCH_UI_TRANSLATION_BYTES) bound = TILEFINCH_UI_TRANSLATION_BYTES;
    size_t offsets = t->count * sizeof(uint16_t);
    uint16_t *visual = budget_malloc(t->budget, offsets + bound);
    if (!visual) return false;
    char *text = (char *) visual + offsets;
    size_t at = 0;
    bool okay = true;
    for (size_t row = 0; okay && row < t->count; row++) {
        visual[row] = UINT16_MAX;
        const char *value = t->text + t->values[row];
        size_t length = strlen(value);
        if (!text_bidi_maybe_needed(value, length)) continue;
        TextBidiStatus status;
        TextBidiParagraph *p = text_bidi_paragraph_create(
            t->budget, value, length, TEXT_BIDI_BASE_RTL, &status);
        if (!p) { okay = false; break; }
        size_t count = text_bidi_paragraph_count(p);
        uint16_t order[256];
        okay = count <= sizeof(order) / sizeof(order[0])
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
            if (bytes >= bound - at) { okay = false; break; }
            memcpy(text + at, out, bytes);
            at += bytes;
        }
        text_bidi_paragraph_destroy(p);
        if (!okay) break;
        text[at++] = 0;
        visual[row] = (uint16_t)start;
    }
    if (!okay) {
        budget_free(t->budget, visual);
        return false;
    }
    if (at < bound) {
        uint16_t *smaller = budget_realloc(t->budget, visual, offsets + at);
        if (smaller) visual = smaller;
    }
    t->visual = visual;
    t->visual_text = (const char *) visual + offsets;
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
        "https://raw.githubusercontent.com/" TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER
        "/" TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME "/main/%s",
        spec->resource_key);
    return n > 0 && (size_t)n < capacity;
}

static TilefinchUiTranslation *translation_parse(
    Budget *budget, unsigned language, const char *code,
    const unsigned char *bytes, size_t length);

TilefinchUiTranslation *tilefinch_ui_translation_create(
    Budget *budget, unsigned language, const void *bytes, size_t length)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    if (!budget || !bytes || !spec || !spec->sha256 || !length
        || length > TILEFINCH_UI_TRANSLATION_BYTES) return NULL;
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
    return translation_parse(budget, language, spec->code, bytes, length);
}

/* The pinned bytes' structure is still checked in full: a pin only says
   which bytes were published, not that the generator was right. */
static TilefinchUiTranslation *translation_parse(
    Budget *budget, unsigned language, const char *code,
    const unsigned char *bytes, size_t length)
{
    char header[32];
    int header_length = snprintf(header, sizeof(header), "TFUI2\t%s\n", code);
    const unsigned char *in = bytes;
    if (header_length <= 0 || length < (size_t)header_length + 2u
        || memcmp(in, header, (size_t)header_length)) return NULL;
    in += header_length;
    size_t count = (size_t)in[0] << 8 | in[1];
    in += 2;
    size_t remaining = length - (size_t)header_length - 2u;
    if (!count || count > TILEFINCH_UI_TRANSLATION_ROWS
        || remaining <= count * (sizeof(uint32_t) + 1u)) return NULL;
    const unsigned char *lengths = in + count * sizeof(uint32_t);
    const unsigned char *values = lengths + count;
    size_t value_bytes = remaining - count * (sizeof(uint32_t) + 1u);
    if (values[value_bytes - 1] != '\n') return NULL;
    size_t header_bytes = sizeof(TilefinchUiTranslation);
    TilefinchUiTranslation *t = budget_malloc(budget, header_bytes
        + count * (sizeof(uint32_t) + sizeof(uint16_t) + 1u) + value_bytes);
    if (!t) return NULL;
    uint32_t *hashes = (uint32_t *)((char *)t + header_bytes);
    uint16_t *offsets = (uint16_t *)(hashes + count);
    uint8_t *key_lengths = (uint8_t *)(offsets + count);
    char *text = (char *)(key_lengths + count);
    *t = (TilefinchUiTranslation){.budget = budget, .hashes = hashes,
        .values = offsets, .key_lengths = key_lengths, .text = text,
        .count = count, .language = language};
    for (size_t row = 0; row < count; row++) {
        const unsigned char *h = in + row * sizeof(uint32_t);
        hashes[row] = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16
            | (uint32_t)h[2] << 8 | h[3];
        if ((row && hashes[row] <= hashes[row-1]) || !lengths[row]) goto invalid;
        key_lengths[row] = lengths[row];
    }
    /* Display strings, not author-controlled printf programs: no '%', NUL,
       separator or other control byte. */
    size_t at = 0, row = 0;
    for (; at < value_bytes && row < count; row++) {
        offsets[row] = (uint16_t)at;
        size_t end = at;
        while (values[end] != '\n') {
            if (values[end] < 0x20u || values[end] == '%' || values[end] == 0x7fu)
                goto invalid;
            text[end] = (char)values[end];
            end++;
        }
        if (end == at || end - at > TILEFINCH_UI_TRANSLATION_VALUE_BYTES) goto invalid;
        text[end] = 0;
        at = end + 1;
    }
    if (row != count || at != value_bytes) goto invalid;
    if (language == TILEFINCH_UI_LANGUAGE_ARABIC
        && !translation_prepare_visual_text(t)) goto invalid;
    return t;
invalid:
    budget_free(budget, t);
    return NULL;
}

void tilefinch_ui_translation_destroy(TilefinchUiTranslation *t)
{
    if (t) {
        budget_free(t->budget, t->visual);
        budget_free(t->budget, t);
    }
}

/* A row matches when the hash of the case-folded string and its byte length
   both agree: a non-key string whose hash collides with a key must also
   have that key's length, so a stray translation needs both a 32-bit hash
   collision and an equal length. */
static const char *translation_text(const TilefinchUiTranslation *t,
                                   const char *english, bool visual)
{
    if (!t || !english) return english;
    size_t length = strlen(english);
    uint32_t hash = style_fnv1a(STYLE_FNV1A_SEED, english, length, true);
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        if (t->hashes[mid] == hash) {
            if (t->key_lengths[mid] != length) return english;
            return visual && t->visual && t->visual[mid] != UINT16_MAX
                ? t->visual_text + t->visual[mid] : t->text + t->values[mid];
        }
        if (t->hashes[mid] < hash) lo = mid + 1; else hi = mid;
    }
    return english;
}

const char *tilefinch_ui_translation_text(const TilefinchUiTranslation *t, const char *english)
{ return translation_text(t, english, true); }

const char *tilefinch_ui_logical_text(const char *english)
{ return translation_text(bound_translation, english, false); }

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

bool tilefinch_ui_translation_installed(
    const TilefinchInstallPaths *paths, unsigned language)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    struct stat information;
    if (!spec || !translation_path(paths, language, path, sizeof(path))
        || lstat(path, &information) != 0 || !S_ISREG(information.st_mode)
        || information.st_size <= 0
        || (uint64_t)information.st_size > TILEFINCH_UI_TRANSLATION_BYTES) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    unsigned char chunk[512];
    size_t total = 0, count;
    bool okay = true;
    while (okay && (count = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        total += count;
        okay = total <= TILEFINCH_UI_TRANSLATION_BYTES
            && tilefinch_sha256_update(&sha, chunk, count);
    }
    if (ferror(file)) okay = false;
    if (fclose(file) != 0) okay = false;
    uint8_t digest[32];
    if (!okay || !tilefinch_sha256_final(&sha, digest)) return false;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        if (spec->sha256[2*i] != digits[digest[i] >> 4]
            || spec->sha256[2*i+1] != digits[digest[i] & 15]) return false;
    }
    return true;
}

/* After a verified install, remove this language's catalogs of older
   versions, which this build never reads. Only the exact names an older
   build wrote ("<code>-v<N>.tful", N below the current version, in
   ui-language/) are probed, and only regular files are removed: symlinks,
   directories, temporaries and every other name stay. Bounded to the most
   recent versions; a failed removal is harmless and ignored. */
#define TRANSLATION_PRUNE_VERSIONS 32u
static void translation_prune_older(const TilefinchInstallPaths *paths,
                                    const TilefinchUiLanguageSpec *spec)
{
    unsigned oldest = spec->version > TRANSLATION_PRUNE_VERSIONS
        ? spec->version - TRANSLATION_PRUNE_VERSIONS : 1u;
    for (unsigned version = oldest; version < spec->version; version++) {
        char relative[96], path[TILEFINCH_INSTALL_PATH_LIMIT];
        int n = snprintf(relative, sizeof(relative), "ui-language/%s-v%u.tful",
                         spec->code, version);
        struct stat information;
        if (n > 0 && (size_t)n < sizeof(relative)
            && tilefinch_install_data_path(paths, relative, path, sizeof(path))
            && lstat(path, &information) == 0 && S_ISREG(information.st_mode))
            (void)unlink(path);
    }
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
    /* PSP FAT rename refuses to replace an existing file, and a stale or
       damaged catalog of the same version keeps this exact name. */
    if (okay) okay = tilefinch_platform_replace_file(temp, path, NULL);
    if (okay) translation_prune_older(paths, tilefinch_ui_language_spec(language));
    /* After the rename the verified file is already published, so the sync
       is only a durability hint (tilefinch_memory_stick_sync). */
    if (okay) (void)tilefinch_memory_stick_sync();
    if (!okay) (void)remove(temp);
    return okay;
}

void tilefinch_ui_translation_bind(const TilefinchUiTranslation *translation)
{ bound_translation = translation; }
unsigned tilefinch_ui_translation_bound_language(void)
{ return bound_translation ? bound_translation->language : TILEFINCH_UI_LANGUAGE_ENGLISH; }
const char *tilefinch_ui_text(const char *english)
{ return tilefinch_ui_translation_text(bound_translation, english); }

const char *tilefinch_ui_display_name(const char *logical)
{
    const TilefinchUiTranslation *t = bound_translation;
    if (t != NULL && t->visual != NULL && logical != NULL)
        for (size_t i = 0; i < t->count; i++)
            if (t->visual[i] != UINT16_MAX
                && !strcmp(logical, t->text + t->values[i]))
                return t->visual_text + t->visual[i];
    return logical;
}
