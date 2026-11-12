#include "tilefinch/budget.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/document.h"
#include "tilefinch/font.h"
#include "tilefinch/glyph_component.h"
#include "../src/tilefinch_test_faults.h"
#include "tilefinch/glyph_component_store.h"
#include "tilefinch/layout.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/render.h"
#include "tilefinch/sha256.h"
#include "tilefinch/style.h"
#include "tilefinch/ui_language.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

enum {
    HEADER_BYTES = 80,
    DIRECTORY_BYTES = TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2,
    PAGE_BYTES = 36,
    SEQUENCE_BYTES = 40,
    GLYPH_BYTES = 32
};

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static bool test_empty_provider_match(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL);
    size_t retained = budget.current;
    const char *samples[] = {"Menu 123", "日本語", "한국어", "ক্ষি", "ৰ্কি", "कि", "➡", "Привет"};
    for (size_t at = 0; at < sizeof(samples) / sizeof(samples[0]); at++) {
        size_t consumed = 777;
        uint32_t key = 888;
        unsigned width = 999;
        CHECK(!tilefinch_glyph_provider_match(provider, samples[at], strlen(samples[at]),
            &consumed, &key, &width));
        CHECK(consumed == 777 && key == 888 && width == 999);
        CHECK(budget.current == retained);
    }
    tilefinch_glyph_provider_destroy(provider);
    CHECK(budget.current == 0);
    return true;
}

static bool write_fixture(char path[128], const char *component_id)
{
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t sequences_offset = pages_offset + PAGE_BYTES * 2u;
    const size_t payload_offset = sequences_offset + SEQUENCE_BYTES * 2u;
    const size_t file_size = payload_offset + GLYPH_BYTES * 4u;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);

    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_MONO;
    bytes[11] = 16;
    bytes[12] = 16;
    bytes[13] = 2;
    CHECK(component_id != NULL
          && strlen(component_id) < TILEFINCH_GLYPH_COMPONENT_ID_LIMIT);
    bytes[14] = (uint8_t) strlen(component_id);
    write_u32(bytes + 16, 4);
    write_u16(bytes + 20, 2);
    write_u16(bytes + 22, 2);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) sequences_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, component_id, strlen(component_id));

    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 0x4eu * 2u, 0);
    write_u16(bytes + HEADER_BYTES + 0x1f4u * 2u, 1);

    uint8_t *first_page = bytes + pages_offset;
    write_u32(first_page, 0);
    first_page[4] = 1; /* U+4E00 */
    uint8_t *second_page = first_page + PAGE_BYTES;
    write_u32(second_page, 1);
    second_page[4 + 0x68u / 8u] = (uint8_t) (1u << (0x68u & 7u));

    uint8_t *sequence = bytes + sequences_offset;
    sequence[0] = 2;
    write_u32(sequence + 4, 2);
    write_u32(sequence + 8, 0x00a9u);
    write_u32(sequence + 12, 0xfe0fu);
    sequence += SEQUENCE_BYTES;
    sequence[0] = 3;
    write_u32(sequence + 4, 3);
    write_u32(sequence + 8, 0x1f468u);
    write_u32(sequence + 12, 0x200du);
    write_u32(sequence + 16, 0x1f469u);

    for (size_t glyph = 0; glyph < 4u; glyph++)
        memset(bytes + payload_offset + glyph * GLYPH_BYTES,
               (int) (0x31u + glyph), GLYPH_BYTES);

    snprintf(path, 128, "/tmp/tilefinch-glyph-component-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool write_color_fixture(char path[128])
{
    static const char id[] = "emoji-color-v1";
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t payload_offset = pages_offset + PAGE_BYTES;
    const size_t payload_bytes = 2u * 2u * 3u;
    const size_t file_size = payload_offset + payload_bytes;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);
    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_COLOR;
    bytes[11] = 2;
    bytes[12] = 2;
    bytes[13] = 1;
    bytes[14] = (uint8_t) strlen(id);
    write_u32(bytes + 16, 1);
    write_u16(bytes + 20, 1);
    write_u16(bytes + 22, 0);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) payload_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, id, sizeof(id));
    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 0x1f6u * 2u, 0);
    uint8_t *page = bytes + pages_offset;
    write_u32(page, 0);
    page[4] = 1; /* U+1F600 */
    for (size_t pixel = 0; pixel < 4u; pixel++) {
        bytes[payload_offset + pixel * 2u] = 0xf8u;
        bytes[payload_offset + pixel * 2u + 1u] = 0x00u;
        bytes[payload_offset + 8u + pixel] = 0xffu;
    }
    snprintf(path, 128, "/tmp/tilefinch-color-glyph-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool hash_file(const char *path, uint8_t digest[32], uint64_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    uint8_t bytes[257];
    uint64_t total = 0;
    bool ok = true;
    while (ok) {
        size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (count != 0) {
            ok = tilefinch_sha256_update(&sha, bytes, count);
            total += count;
        }
        if (count < sizeof(bytes)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (ok) ok = tilefinch_sha256_final(&sha, digest);
    if (ok) *size = total;
    return ok;
}

static bool read_bounded_file(const char *path, uint8_t **bytes,
                              size_t *length)
{
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end <= 0 || end > (long) TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    uint8_t *data = malloc((size_t) end);
    bool ok = data != NULL
        && fread(data, 1, (size_t) end, file) == (size_t) end
        && fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        free(data);
        return false;
    }
    *bytes = data;
    *length = (size_t) end;
    return true;
}

static bool copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output = input != NULL ? fopen(destination, "wb") : NULL;
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return false;
    }
    uint8_t bytes[16u * 1024u];
    bool ok = true;
    while (ok) {
        size_t count = fread(bytes, 1, sizeof(bytes), input);
        if (count != 0) ok = fwrite(bytes, 1, count, output) == count;
        if (count < sizeof(bytes)) {
            if (ferror(input)) ok = false;
            break;
        }
    }
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    return ok;
}

/* Moves a fixture to where a finished download sits; the installer takes it
   from there. */
static bool stage_fixture_download(
    const char *root, TilefinchGlyphPack pack, const char *fixture,
    char part[TILEFINCH_INSTALL_PATH_LIMIT])
{
    return tilefinch_glyph_component_prepare_download(
               root, pack, part, TILEFINCH_INSTALL_PATH_LIMIT)
        && copy_file(fixture, part) && remove(fixture) == 0;
}

static bool test_pack_catalog(void)
{
    static const TilefinchGlyphPackSpec expected[] = {
        {"glyph-ja", "Japanese", "tilefinch-glyph-ja-v1.tfgm",
         "tilefinch-glyph-ja-v1.tfgf"},
        {"glyph-zh-hans", "Simplified Chinese",
         "tilefinch-glyph-zh-hans-v1.tfgm",
         "tilefinch-glyph-zh-hans-v1.tfgf"},
        {"glyph-zh-hant", "Traditional Chinese",
         "tilefinch-glyph-zh-hant-v1.tfgm",
         "tilefinch-glyph-zh-hant-v1.tfgf"},
        {"glyph-ko", "Korean", "tilefinch-glyph-ko-v1.tfgm",
         "tilefinch-glyph-ko-v1.tfgf"},
        {"glyph-emoji-color", "Color Emoji",
         "tilefinch-glyph-emoji-color-v1.tfgm",
         "tilefinch-glyph-emoji-color-v1.tfgf"},
        {"glyph-cyrillic", "Cyrillic",
         "tilefinch-glyph-cyrillic-v1.tfgm",
         "tilefinch-glyph-cyrillic-v1.tfgf"},
        {"glyph-latin-extended", "Extended Latin",
         "tilefinch-glyph-latin-extended-v1.tfgm",
         "tilefinch-glyph-latin-extended-v1.tfgf"},
        {"glyph-arabic", "Arabic", "tilefinch-glyph-arabic-v1.tfgm",
         "tilefinch-glyph-arabic-v1.tfgf"},
        {"glyph-hebrew", "Hebrew", "tilefinch-glyph-hebrew-v1.tfgm",
         "tilefinch-glyph-hebrew-v1.tfgf"},
        {"glyph-devanagari", "Devanagari (Hindi)",
         "tilefinch-glyph-devanagari-v1.tfgm", "tilefinch-glyph-devanagari-v1.tfgf"},
        {"glyph-bengali-assamese", "Bengali-Assamese",
         "tilefinch-glyph-bengali-assamese-v1.tfgm", "tilefinch-glyph-bengali-assamese-v1.tfgf"}
    };
    CHECK(TILEFINCH_GLYPH_PACK_JAPANESE == 0
          && TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED == 1
          && TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL == 2
          && TILEFINCH_GLYPH_PACK_KOREAN == 3
          && TILEFINCH_GLYPH_PACK_COLOR_EMOJI == 4
          && TILEFINCH_GLYPH_PACK_CYRILLIC == 5
          && TILEFINCH_GLYPH_PACK_LATIN_EXTENDED == 6
          && TILEFINCH_GLYPH_PACK_ARABIC == 7
          && TILEFINCH_GLYPH_PACK_HEBREW == 8
          && TILEFINCH_GLYPH_PACK_DEVANAGARI == 9
          && TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE == 10
          && TILEFINCH_GLYPH_PACK_COUNT
                 == sizeof(expected) / sizeof(expected[0]));
    for (TilefinchGlyphPack pack = 0;
         pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        const TilefinchGlyphPackSpec *actual =
            tilefinch_glyph_pack_spec(pack);
        CHECK(actual != NULL
              && strcmp(actual->id, expected[pack].id) == 0
              && strcmp(actual->label, expected[pack].label) == 0
              && strcmp(actual->metadata_asset,
                        expected[pack].metadata_asset) == 0
              && strcmp(actual->pack_asset,
                        expected[pack].pack_asset) == 0);
    }
    CHECK(tilefinch_glyph_pack_spec(TILEFINCH_GLYPH_PACK_COUNT) == NULL);

    static const struct {
        BrowserGlyphLanguage language;
        TilefinchGlyphPack pack;
    } language_packs[] = {
        {BROWSER_GLYPH_LANGUAGE_JAPANESE,
         TILEFINCH_GLYPH_PACK_JAPANESE},
        {BROWSER_GLYPH_LANGUAGE_CHINESE_SIMPLIFIED,
         TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED},
        {BROWSER_GLYPH_LANGUAGE_CHINESE_TRADITIONAL,
         TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL},
        {BROWSER_GLYPH_LANGUAGE_KOREAN, TILEFINCH_GLYPH_PACK_KOREAN},
        {BROWSER_GLYPH_LANGUAGE_CYRILLIC,
         TILEFINCH_GLYPH_PACK_CYRILLIC},
        {BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED,
         TILEFINCH_GLYPH_PACK_LATIN_EXTENDED},
        {BROWSER_GLYPH_LANGUAGE_ARABIC, TILEFINCH_GLYPH_PACK_ARABIC},
        {BROWSER_GLYPH_LANGUAGE_HEBREW, TILEFINCH_GLYPH_PACK_HEBREW},
        {BROWSER_GLYPH_LANGUAGE_DEVANAGARI, TILEFINCH_GLYPH_PACK_DEVANAGARI},
        {BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE, TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE}
    };
    for (size_t at = 0;
         at < sizeof(language_packs) / sizeof(language_packs[0]); at++) {
        TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_COLOR_EMOJI;
        CHECK(tilefinch_glyph_pack_for_language(
                  (unsigned) language_packs[at].language, &pack)
              && pack == language_packs[at].pack);
    }
    TilefinchGlyphPack unchanged = TILEFINCH_GLYPH_PACK_COLOR_EMOJI;
    CHECK(!tilefinch_glyph_pack_for_language(
              BROWSER_GLYPH_LANGUAGE_EMBEDDED, &unchanged)
          && unchanged == TILEFINCH_GLYPH_PACK_COLOR_EMOJI
          && !tilefinch_glyph_pack_for_language(
              BROWSER_GLYPH_LANGUAGE_COUNT, &unchanged)
          && !tilefinch_glyph_pack_for_language(UINT_MAX, &unchanged)
          && !tilefinch_glyph_pack_for_language(
              BROWSER_GLYPH_LANGUAGE_JAPANESE, NULL));
    return true;
}

static bool render_authored_sample(TilefinchGlyphProvider *provider,
                                   const char *sample, size_t *ink_count)
{
    uint8_t canvas[512u * 16u] = {0};
    size_t cursor = 0;
    size_t length = strlen(sample);
    size_t glyphs = 0;
    for (size_t at = 0; at < length;) {
        uint32_t codepoint = 0;
        size_t used = font_utf8_next(sample + at, length - at, &codepoint);
        if (used == 0) return false;
        if (codepoint < 0x80u) {
            at += used;
            cursor += codepoint == ' ' ? 4u : 8u;
            continue;
        }
        uint32_t key = 0;
        unsigned width = 0;
        if (!tilefinch_glyph_provider_match(
                provider, sample + at, length - at, &used, &key, &width)
            || used == 0 || used > length - at) {
            fprintf(stderr, "sample missing U+%04X\n", (unsigned) codepoint);
            return false;
        }
        at += used;
        TilefinchGlyphSource source = {0};
        for (size_t pump = 0;
             !tilefinch_glyph_provider_source(provider, key, &source)
                 && pump < 8u;
             pump++) {
            bool changed = false;
            size_t bytes_read = 0;
            if (!tilefinch_glyph_provider_pump(
                    provider, 16u * 1024u, &changed, &bytes_read)) {
                return false;
            }
        }
        if (!tilefinch_glyph_provider_source(provider, key, &source)
            || source.kind != TILEFINCH_GLYPH_COMPONENT_MONO
            || source.pixels == NULL || source.width == 0
            || source.width > 16u || source.height == 0
            || source.height > 16u) {
            fprintf(stderr, "sample source failed U+%04X\n",
                    (unsigned) codepoint);
            return false;
        }
        if (cursor >= 512u) break;
        size_t drawable = source.width;
        if (cursor + drawable > 512u) drawable = 512u - cursor;
        /* Mono pack cells are 16-bit rows, whatever the glyph's own width. */
        for (size_t y = 0; y < source.height; y++) {
            for (size_t x = 0; x < drawable; x++) {
                size_t column = source.first_column + x;
                canvas[y * 512u + cursor + x] =
                    source.pixels[y * 2u + column / 8u]
                        & (0x80u >> (column & 7u)) ? 255u : 0u;
            }
        }
        cursor += width;
        glyphs++;
        if (cursor >= 512u) break;
    }
    size_t ink = 0;
    for (size_t pixel = 0; pixel < sizeof(canvas); pixel++)
        if (canvas[pixel] != 0) ink++;
    if (ink_count != NULL) *ink_count = ink;
    return glyphs >= 2u && ink >= glyphs * 8u;
}

static bool test_authored_sample_packed_pixels(void)
{
    char path[128];
    CHECK(write_fixture(path, "glyph-ja"));
    Budget budget;
    budget_init(&budget, 512u * 1024u);
    TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(provider, path, "glyph-ja"));
    size_t ink = 0;
    CHECK(render_authored_sample(provider, "一一", &ink));
    /* The fixture's first cell is 32 bytes of 0x31: three pixels per byte.
       Counting bytes as pixels both produces the wrong ink and overreads. */
    CHECK(ink == 2u * GLYPH_BYTES * 3u);
    tilefinch_glyph_provider_destroy(provider);
    CHECK(budget.current == 0 && unlink(path) == 0);
    return true;
}

/*
 * Optional release-ceremony proof. Ordinary builds do not embed the offline
 * update root, and ordinary test runs do not have release artifacts, so this
 * stays dormant unless TILEFINCH_GLYPH_PROOF_DIR is supplied. A release build
 * configured with the real public root then verifies the exact signed files
 * through the same consumer and pack parser used on the PSP.
 */
static bool test_release_artifacts(void)
{
    const char *directory = getenv("TILEFINCH_GLYPH_PROOF_DIR");
    const char *stage_root = getenv("TILEFINCH_GLYPH_STAGE_ROOT");
    const char *stage_selection = getenv("TILEFINCH_GLYPH_STAGE_LANGUAGE");
    const char *stage_all_value = getenv("TILEFINCH_GLYPH_STAGE_ALL");
    bool stage_all = stage_all_value != NULL
        && stage_all_value[0] != '\0' && strcmp(stage_all_value, "0") != 0;
    if (directory == NULL || directory[0] == '\0') return true;
    if (stage_root != NULL && stage_root[0] != '\0')
        CHECK(mkdir(stage_root, 0777) == 0);
    CHECK(tilefinch_update_root_is_configured());
    TilefinchUpdateRoot root = {0};
    CHECK(tilefinch_update_embedded_root(&root));
    BrowserGlyphLanguage staged_language = BROWSER_GLYPH_LANGUAGE_JAPANESE;
    TilefinchGlyphPack staged_pack = TILEFINCH_GLYPH_PACK_JAPANESE;
    if (stage_selection != NULL
        && strcmp(stage_selection, "cyrillic") == 0) {
        staged_language = BROWSER_GLYPH_LANGUAGE_CYRILLIC;
        staged_pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
    } else if (stage_selection != NULL
               && strcmp(stage_selection, "latin-extended") == 0) {
        staged_language = BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED;
        staged_pack = TILEFINCH_GLYPH_PACK_LATIN_EXTENDED;
    } else if (stage_selection != NULL
               && strcmp(stage_selection, "arabic") == 0) {
        staged_language = BROWSER_GLYPH_LANGUAGE_ARABIC;
        staged_pack = TILEFINCH_GLYPH_PACK_ARABIC;
    } else if (stage_selection != NULL
               && strcmp(stage_selection, "hebrew") == 0) {
        staged_language = BROWSER_GLYPH_LANGUAGE_HEBREW;
        staged_pack = TILEFINCH_GLYPH_PACK_HEBREW;
    } else if (stage_selection != NULL
               && strcmp(stage_selection, "devanagari") == 0) {
        staged_language = BROWSER_GLYPH_LANGUAGE_DEVANAGARI;
        staged_pack = TILEFINCH_GLYPH_PACK_DEVANAGARI;
    } else if (stage_selection != NULL
               && (strcmp(stage_selection, "bengali-assamese") == 0
                   || strcmp(stage_selection, "bengali") == 0
                   || strcmp(stage_selection, "assamese") == 0)) {
        staged_language = BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE;
        staged_pack = TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE;
    }
    static const unsigned probes[] = {
        0x65e5u, 0x6c49u, 0x6f22u, 0xac00u, 0x1f600u,
        0x0490u, 0x1ed9u, 0xfeb3u, 0x05e9u, 0x0939u, 0x09f0u
    };
    static const char *const samples[] = {
        NULL, NULL, NULL, NULL, NULL,
        "Привет мир Привіт, Україно",
        "Tiếng Việt",
        "مرحبا فارسی اردو",
        "שלום עולם",
        "नमस्ते हिन्दी",
        "বাংলা অসমীয়া ৰ ৱ । ॥"
    };
    _Static_assert(sizeof(probes) / sizeof(probes[0])
                       == TILEFINCH_GLYPH_PACK_COUNT,
                   "every release pack needs a nonzero glyph probe");
    _Static_assert(sizeof(samples) / sizeof(samples[0])
                       == TILEFINCH_GLYPH_PACK_COUNT,
                   "every release pack needs an explicit sample entry");
    for (TilefinchGlyphPack pack = 0;
         pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
        CHECK(spec != NULL);
        char envelope_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        char package_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        int envelope_written = snprintf(
            envelope_path, sizeof(envelope_path), "%s/%s",
            directory, spec->metadata_asset);
        int package_written = snprintf(
            package_path, sizeof(package_path), "%s/%s",
            directory, spec->pack_asset);
        CHECK(envelope_written > 0
              && (size_t) envelope_written < sizeof(envelope_path));
        CHECK(package_written > 0
              && (size_t) package_written < sizeof(package_path));
        uint8_t *envelope = NULL;
        size_t envelope_length = 0;
        CHECK(read_bounded_file(
            envelope_path, &envelope, &envelope_length));
        TilefinchUpdateVerifiedEnvelope verified = {0};
        TilefinchUpdateStatus status = tilefinch_update_verify_glyph_envelope(
            envelope, envelope_length,
            &(TilefinchUpdateVerifyOptions) {
                .embedded_root = &root,
                .crypto = tilefinch_update_default_crypto(),
                .now_unix = UINT64_C(1786600000),
                .clock_valid = true,
                .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL
            }, &verified);
        CHECK(status == TILEFINCH_UPDATE_OK);
        CHECK(verified.manifest.package_format
              == TILEFINCH_UPDATE_PACKAGE_GLYPH);
        bool revised_hindi = pack == TILEFINCH_GLYPH_PACK_DEVANAGARI
            && (verified.manifest.release_sequence == 2u
                || verified.manifest.release_sequence == 3u
                || verified.manifest.release_sequence == 4u
                || verified.manifest.release_sequence == 5u
                || verified.manifest.release_sequence == 6u
                || verified.manifest.release_sequence == 7u
                || verified.manifest.release_sequence == 8u
                || verified.manifest.release_sequence == 9u);
        bool revised_bengali = pack == TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE
            && (verified.manifest.release_sequence == 2u
                || verified.manifest.release_sequence == 3u
                || verified.manifest.release_sequence == 4u
                || verified.manifest.release_sequence == 5u);
        /* components-v9 rebuilt every pack but color emoji with real metrics. */
        bool rebuilt_scalar = pack != TILEFINCH_GLYPH_PACK_DEVANAGARI
            && pack != TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE
            && pack != TILEFINCH_GLYPH_PACK_COLOR_EMOJI
            && verified.manifest.release_sequence == 2u;
        CHECK(revised_hindi || revised_bengali || rebuilt_scalar
              || verified.manifest.release_sequence == 1u);
        CHECK(strcmp(verified.manifest.tag,
                     pack == TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE
                        ? (verified.manifest.release_sequence == 5u
                            ? "components-v9"
                            : verified.manifest.release_sequence == 4u
                            ? "components-v8"
                            : verified.manifest.release_sequence == 3u
                            ? "components-v7"
                            : revised_bengali ? "components-v6"
                                           : "glyph-bengali-assamese-v1")
                        : revised_hindi
                        ? (verified.manifest.release_sequence == 9u
                            ? "components-v9"
                            : verified.manifest.release_sequence == 8u
                            ? "components-v8"
                            : verified.manifest.release_sequence == 7u
                            ? "components-v7"
                            : verified.manifest.release_sequence == 6u
                            ? "components-v6"
                            : verified.manifest.release_sequence == 5u
                            ? "components-v5"
                            : verified.manifest.release_sequence == 4u
                            ? "components-v4"
                            : verified.manifest.release_sequence == 3u
                                ? "components-v3" : "components-v2")
                        : rebuilt_scalar ? "components-v9"
                        : "components-v1") == 0);
        CHECK(strcmp(verified.manifest.asset, spec->pack_asset) == 0);
        if (pack == TILEFINCH_GLYPH_PACK_CYRILLIC
            || pack == TILEFINCH_GLYPH_PACK_LATIN_EXTENDED
            || pack == TILEFINCH_GLYPH_PACK_ARABIC
            || pack == TILEFINCH_GLYPH_PACK_HEBREW) {
            CHECK(verified.manifest.root_version == 1u
                  && verified.manifest.expires_unix
                         == (rebuilt_scalar ? UINT64_C(1825632000)
                                            : UINT64_C(1818115200)));
        }
        uint8_t package_digest[32];
        uint64_t package_size = 0;
        CHECK(hash_file(package_path, package_digest, &package_size));
        CHECK(package_size == verified.manifest.package_size);
        CHECK(memcmp(package_digest,
                     verified.manifest.package_sha256, 32) == 0);

        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        TilefinchGlyphProvider *provider =
            tilefinch_glyph_provider_create(&budget);
        CHECK(provider != NULL);
        CHECK(tilefinch_glyph_provider_attach(
            provider, package_path, spec->id));
        uint32_t key = 0;
        unsigned width = 0;
        CHECK(tilefinch_glyph_provider_has_codepoint(
            provider, probes[pack], &key, &width));
        CHECK(width > 0u);
        if (pack == TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE) {
            static const char *const clusters[] = {
                "ক্ষি", "ৰ্কি", "ৱা", "ড়ো"
            };
            for (size_t row = 0;
                 row < sizeof(clusters) / sizeof(clusters[0]); row++) {
                size_t used = 0;
                CHECK(tilefinch_glyph_provider_match(
                    provider, clusters[row], strlen(clusters[row]),
                    &used, &key, &width));
                /* Metric packs advance by the cluster's own width, never past a cell. */
                CHECK(used == strlen(clusters[row])
                      && width > 0u && width <= 16u);
            }
        }
        if (samples[pack] != NULL
            && !render_authored_sample(provider, samples[pack], NULL)) {
            fprintf(stderr, "authored sample failed for %s\n", spec->id);
            return false;
        }
        tilefinch_glyph_provider_destroy(provider);
        CHECK(budget.current == 0);

        if (stage_root != NULL && stage_root[0] != '\0'
            && (stage_all || pack == staged_pack
                || pack == TILEFINCH_GLYPH_PACK_COLOR_EMOJI)) {
            char staged_package[TILEFINCH_INSTALL_PATH_LIMIT];
            CHECK(tilefinch_glyph_component_prepare_download(
                      stage_root, pack, staged_package,
                      sizeof(staged_package))
                  && copy_file(package_path, staged_package));
            TilefinchGlyphComponentInstall *install =
                tilefinch_glyph_component_install_create(
                    &budget, &(TilefinchGlyphComponentInstallOptions) {
                        .envelope = envelope,
                        .envelope_length = envelope_length,
                        .manifest = &verified.manifest,
                        .manifest_digest = verified.manifest_digest,
                        .install_root = stage_root,
                        .pack = pack
                    });
            CHECK(install != NULL);
            TilefinchUpdateInstallSnapshot snapshot = {0};
            for (size_t pump = 0; pump < 4096u; pump++) {
                CHECK(tilefinch_glyph_component_install_pump(
                    install, 16u * 1024u));
                CHECK(tilefinch_glyph_component_install_snapshot(
                    install, &snapshot));
                if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
                    break;
            }
            CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
                  && snapshot.status == TILEFINCH_UPDATE_OK);
            tilefinch_glyph_component_install_destroy(install);
            TilefinchInstallPaths paths = {.slotted = true};
            snprintf(paths.install_root, sizeof(paths.install_root), "%s",
                     stage_root);
            uint64_t installed_sequence = 0;
            uint8_t installed_sha256[32];
            CHECK(tilefinch_glyph_component_installed_identity(
                &budget, &paths, pack, &root, &installed_sequence,
                installed_sha256));
            CHECK(installed_sequence == verified.manifest.release_sequence
                  && memcmp(installed_sha256,
                            package_digest, sizeof(package_digest)) == 0);
            CHECK(budget.current == 0);
        }
        free(envelope);
    }
    if (stage_root != NULL && stage_root[0] != '\0') {
        Budget profile_budget;
        budget_init(&profile_budget, 2u * 1024u * 1024u);
        BrowserProfile *profile = browser_profile_create(&profile_budget);
        CHECK(profile != NULL);
        browser_profile_set_glyph_language(
            profile, staged_language);
        if (staged_language == BROWSER_GLYPH_LANGUAGE_DEVANAGARI)
            browser_profile_set_ui_language(profile, TILEFINCH_UI_LANGUAGE_HINDI);
        else if (staged_language == BROWSER_GLYPH_LANGUAGE_ARABIC)
            browser_profile_set_ui_language(profile, TILEFINCH_UI_LANGUAGE_ARABIC);
        else if (staged_language == BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE)
            browser_profile_set_ui_language(profile,
                stage_selection != NULL && strcmp(stage_selection, "assamese") == 0
                    ? TILEFINCH_UI_LANGUAGE_ASSAMESE
                    : TILEFINCH_UI_LANGUAGE_BENGALI);
        browser_profile_set_color_emoji(profile, true);
        const char *bookmark_title = staged_language
                == BROWSER_GLYPH_LANGUAGE_CYRILLIC
            ? "Привет мир — Привіт, Україно — Tiếng Việt"
            : staged_language == BROWSER_GLYPH_LANGUAGE_LATIN_EXTENDED
                ? "Tiếng Việt — Привет мир"
            : staged_language == BROWSER_GLYPH_LANGUAGE_ARABIC
                ? "مرحبا — فارسی — اردو"
            : staged_language == BROWSER_GLYPH_LANGUAGE_HEBREW
                ? "שלום עולם"
            : staged_language == BROWSER_GLYPH_LANGUAGE_DEVANAGARI
                ? "नमस्ते हिन्दी"
            : staged_language == BROWSER_GLYPH_LANGUAGE_BENGALI_ASSAMESE
                ? "বাংলা — অসমীয়া"
                : "日本語  漢字  한국어  😀";
        CHECK(browser_profile_add_bookmark(
            profile, "https://example.com/", bookmark_title));
        char profile_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        int profile_written = snprintf(
            profile_path, sizeof(profile_path), "%s/profile.cfg", stage_root);
        CHECK(profile_written > 0
              && (size_t) profile_written < sizeof(profile_path));
        CHECK(browser_profile_save(profile, profile_path));
        browser_profile_destroy(profile);
        CHECK(profile_budget.current == 0);
        puts("glyph-component-tests ppsspp-stage=PASS");
    }
    puts("glyph-component-tests release-proof=PASS");
    return true;
}

static bool test_script_sequence_matching(void)
{
    static const struct {
        const char *id;
        const char *text[2];
        uint32_t codepoints[2][4];
        uint8_t count[2];
    } cases[] = {
        {"glyph-ja", {"がx", "ｶﾞx"}, {{0x304b, 0x3099}, {0xff76, 0xff9e}}, {2, 2}},
        {"glyph-bengali-assamese", {"কিx", "ৰ্কিx"},
            {{0x0995, 0x09bf}, {0x09f0, 0x09cd, 0x0995, 0x09bf}}, {2, 4}}
    };
    for (size_t test = 0; test < sizeof(cases) / sizeof(cases[0]); test++) {
        char path[128];
        CHECK(write_fixture(path, cases[test].id));
        FILE *file = fopen(path, "r+b");
        CHECK(file != NULL);
        uint8_t sequences[SEQUENCE_BYTES * 2u] = {0};
        for (size_t row = 0; row < 2; row++) {
            uint8_t *entry = sequences + row * SEQUENCE_BYTES;
            entry[0] = cases[test].count[row];
            write_u32(entry + 4, 2u + row);
            for (size_t cp = 0; cp < cases[test].count[row]; cp++)
                write_u32(entry + 8 + cp * 4, cases[test].codepoints[row][cp]);
        }
        CHECK(fseek(file, HEADER_BYTES + DIRECTORY_BYTES + PAGE_BYTES * 2u, SEEK_SET) == 0);
        CHECK(fwrite(sequences, 1, sizeof(sequences), file) == sizeof(sequences));
        CHECK(fclose(file) == 0);
        Budget budget;
        budget_init(&budget, 512u * 1024u);
        TilefinchGlyphProvider *provider = tilefinch_glyph_provider_create(&budget);
        CHECK(provider && tilefinch_glyph_provider_attach(provider, path, cases[test].id));
        for (size_t row = 0; row < 2; row++) {
            size_t used = 0;
            uint32_t key = 0;
            unsigned width = 0;
            const char *text = cases[test].text[row];
            size_t expected_bytes = strlen(text) - 1u;
            CHECK(tilefinch_glyph_provider_match(provider, text, strlen(text),
                                                &used, &key, &width));
            CHECK(used == expected_bytes && width == 16u);
            /* A partial UTF-8 mark cannot be consumed as a complete cluster. */
            CHECK(!tilefinch_glyph_provider_match(provider, text, expected_bytes - 1u,
                                                 &used, &key, &width));
        }
        tilefinch_glyph_provider_destroy(provider);
        CHECK(budget.current == 0);
        CHECK(unlink(path) == 0);
    }
    return true;
}

/* Two Cyrillic cells drawn as vertical one-pixel stems. U+0430 also has ink in
   column 1, outside the column box its table entry gives it. */
enum { SCALAR_STEM_FIRST_ROW = 3, SCALAR_STEM_LAST_ROW = 12 };

static bool write_scalar_fixture_second(
    char path[128], const char *component_id, unsigned baseline_rows,
    const uint8_t columns[2], size_t notice_bytes, unsigned second_low)
{
    static const unsigned narrow_stems[] = {1, 6, 10};
    static const unsigned wide_stems[] = {3, 6, 10, 13};
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t payload_offset = pages_offset + PAGE_BYTES;
    const size_t file_size = payload_offset + GLYPH_BYTES * 2u + notice_bytes;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);
    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_MONO;
    bytes[11] = 16;
    bytes[12] = 16;
    bytes[13] = 2;
    bytes[14] = (uint8_t) strlen(component_id);
    bytes[15] = (uint8_t) baseline_rows;
    write_u32(bytes + 16, 2);
    write_u16(bytes + 20, 1);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) payload_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, component_id, strlen(component_id));
    write_u32(bytes + 72, (uint32_t) notice_bytes);
    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 4u * 2u, 0);
    uint8_t *page = bytes + pages_offset;
    page[4 + 0x30u / 8u] |= (uint8_t) (1u << (0x30u & 7u));
    page[4 + second_low / 8u] |= (uint8_t) (1u << (second_low & 7u));
    for (unsigned y = SCALAR_STEM_FIRST_ROW; y <= SCALAR_STEM_LAST_ROW; y++) {
        uint8_t *narrow = bytes + payload_offset + y * 2u;
        for (size_t at = 0; at < 3u; at++)
            narrow[narrow_stems[at] / 8u] |=
                (uint8_t) (0x80u >> (narrow_stems[at] & 7u));
        uint8_t *wide = bytes + payload_offset + GLYPH_BYTES + y * 2u;
        for (size_t at = 0; at < 4u; at++)
            wide[wide_stems[at] / 8u] |=
                (uint8_t) (0x80u >> (wide_stems[at] & 7u));
    }
    if (columns != NULL)
        memcpy(bytes + file_size - 2u, columns, 2u);
    snprintf(path, 128, "/tmp/tilefinch-glyph-component-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool write_scalar_fixture(
    char path[128], const char *component_id, unsigned baseline_rows,
    const uint8_t columns[2], size_t notice_bytes)
{
    return write_scalar_fixture_second(
        path, component_id, baseline_rows, columns, notice_bytes, 0x48u);
}

/* The ink of one mid-height row of U+0448, one total per stem, cut at source
   columns that lie between its stems. */
static void stem_weights(const FontGlyph *glyph, unsigned weights[4])
{
    static const int cuts[] = {0, 5, 8, 12, 16};
    const unsigned char *row =
        glyph->pixels + (size_t) (glyph->height / 2) * (size_t) glyph->width;
    for (size_t stem = 0; stem < 4u; stem++) {
        weights[stem] = 0;
        for (int x = cuts[stem] * glyph->width / 16;
             x < cuts[stem + 1u] * glyph->width / 16; x++)
            weights[stem] += row[x];
    }
}

static bool settled_scalar_glyph(
    TilefinchGlyphProvider *provider, const FontFace *face, unsigned codepoint,
    int pixel_height, bool bold, FontGlyph *glyph)
{
    uint32_t key = 0;
    TilefinchGlyphSource source;
    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, codepoint, &key, NULL));
    if (!tilefinch_glyph_provider_source(provider, key, &source)) {
        bool changed = false;
        CHECK(tilefinch_glyph_provider_pump(
            provider, GLYPH_BYTES * 2u, &changed, NULL) && changed);
    }
    CHECK(font_glyph_load(face, codepoint, pixel_height, bold, glyph));
    CHECK(!glyph->provider_pending);
    return true;
}

/* Published packs are square cells drawn at many sizes. However a stem falls
   against the output grid, it must keep the same weight, or the letters of a
   word look like different fonts. */
static bool test_mono_cell_scaling_keeps_stem_weight(void)
{
    char path[128];
    CHECK(write_scalar_fixture(path, "glyph-cyrillic", 0, NULL, 0));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(provider, path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    CHECK(face != NULL);
    static const int sizes[] = {12, 13, 20, 24, 28};
    for (size_t at = 0; at < sizeof(sizes) / sizeof(sizes[0]); at++) {
        FontGlyph glyph;
        CHECK(settled_scalar_glyph(
            provider, face, 0x0448u, sizes[at], false, &glyph));
        CHECK(glyph.width == sizes[at] && glyph.height == sizes[at]);
        unsigned weights[4];
        stem_weights(&glyph, weights);
        unsigned expected = 255u * (unsigned) sizes[at] / 16u;
        for (size_t stem = 0; stem < 4u; stem++) {
            if (weights[stem] + 4u < expected
                || weights[stem] > expected + 4u) {
                fprintf(stderr, "size %d stem %zu: weight %u, expected %u\n",
                        sizes[at], stem, weights[stem], expected);
                return false;
            }
        }
        font_glyph_destroy(face, &glyph);
    }
    /* Synthetic bold widens a stem by a whole pixel, including over the
       partly covered pixel at its edge. */
    FontGlyph regular, bold;
    CHECK(settled_scalar_glyph(provider, face, 0x0448u, 28, false, &regular));
    CHECK(settled_scalar_glyph(provider, face, 0x0448u, 28, true, &bold));
    unsigned regular_weights[4], bold_weights[4];
    stem_weights(&regular, regular_weights);
    stem_weights(&bold, bold_weights);
    for (size_t stem = 0; stem < 4u; stem++)
        CHECK(bold_weights[stem] >= regular_weights[stem] + 250u);
    font_glyph_destroy(face, &regular);
    font_glyph_destroy(face, &bold);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    (void) remove(path);
    return true;
}

/* A combining mark sits on the letter before it, so the two measure and paint as
   one letter, in bold too. A pack without column boxes keeps its spacing mark. */
static bool test_metric_pack_marks_sit_on_the_letter(void)
{
    /* U+0430: 5 columns from column 6. U+0483: 4 columns from column 4. */
    static const uint8_t columns[2] = {(4u << 4) | 6u, (3u << 4) | 4u};
    static const char marked[] = "\xd0\xb0\xd2\x83";
    char metric_path[128], legacy_path[128];
    CHECK(write_scalar_fixture_second(
        metric_path, "glyph-cyrillic", 13, columns, 32, 0x83u));
    CHECK(write_scalar_fixture_second(
        legacy_path, "glyph-cyrillic", 0, NULL, 0, 0x83u));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    CHECK(face != NULL);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(
              provider, metric_path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    FontGlyph mark, bold, base;
    CHECK(settled_scalar_glyph(provider, face, 0x0430u, 16, false, &base));
    CHECK(settled_scalar_glyph(provider, face, 0x0483u, 16, false, &mark));
    CHECK(settled_scalar_glyph(provider, face, 0x0483u, 16, true, &bold));
    CHECK(mark.advance_fixed == 0 && mark.advance == 0);
    CHECK(mark.width == 4 && mark.x_offset == -4 && mark.y_offset == -13);
    CHECK(bold.advance_fixed == 0 && bold.width == 5 && bold.x_offset == -5);
    CHECK(base.advance_fixed == 5 * 64 && base.x_offset == 0);
    CHECK(font_text_width_at_size_fixed(
              face, marked, strlen(marked), 16 * 64, false) == 5 * 64);
    CHECK(font_text_width_for_family_at_size_fixed(
              face, FONT_SYSTEM_SANS, marked, strlen(marked), 16 * 64,
              false, true) == 5 * 64);
    font_glyph_destroy(face, &base);
    font_glyph_destroy(face, &mark);
    font_glyph_destroy(face, &bold);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);

    provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(
              provider, legacy_path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    CHECK(settled_scalar_glyph(provider, face, 0x0483u, 16, false, &mark));
    CHECK(mark.advance_fixed == 16 * 64 && mark.x_offset == 0
          && mark.width == 16);
    font_glyph_destroy(face, &mark);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    (void) remove(metric_path);
    (void) remove(legacy_path);
    return true;
}

/* One Devanagari pack: U+0915, U+0924, U+093F, U+094D and the shaped clusters
   U+0915 U+093F and U+0924 U+094D U+0915. Consonants ink low rows, the vowel
   high ones. */
static bool write_indic_fixture(char path[128])
{
    enum { GLYPHS = 6, NOTICE = 32 };
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t sequences_offset = pages_offset + PAGE_BYTES;
    const size_t payload_offset = sequences_offset + SEQUENCE_BYTES * 2u;
    const size_t file_size = payload_offset + GLYPH_BYTES * GLYPHS + NOTICE;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);
    static const char id[] = "glyph-devanagari";
    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_MONO;
    bytes[11] = 16;
    bytes[12] = 16;
    bytes[13] = 2;
    bytes[14] = (uint8_t) strlen(id);
    bytes[15] = 13;
    write_u32(bytes + 16, GLYPHS);
    write_u16(bytes + 20, 1);
    write_u16(bytes + 22, 2);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) sequences_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, id, strlen(id));
    write_u32(bytes + 72, NOTICE);
    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 9u * 2u, 0);
    uint8_t *page = bytes + pages_offset;
    static const unsigned lows[] = {0x15u, 0x24u, 0x3fu, 0x4du};
    for (size_t at = 0; at < 4u; at++)
        page[4 + lows[at] / 8u] |= (uint8_t) (1u << (lows[at] & 7u));
    uint8_t *sequence = bytes + sequences_offset;
    sequence[0] = 2;
    write_u32(sequence + 4, 4);
    write_u32(sequence + 8, 0x0915u);
    write_u32(sequence + 12, 0x093fu);
    sequence += SEQUENCE_BYTES;
    sequence[0] = 3;
    write_u32(sequence + 4, 5);
    write_u32(sequence + 8, 0x0924u);
    write_u32(sequence + 12, 0x094du);
    write_u32(sequence + 16, 0x0915u);
    uint8_t *payload = bytes + payload_offset;
    for (unsigned glyph = 0; glyph < GLYPHS; glyph++) {
        bool vowel = glyph == 2u;
        unsigned first = vowel ? 0u : 8u;
        for (unsigned y = first; y < first + 4u; y++)
            payload[glyph * GLYPH_BYTES + y * 2u] = vowel ? 0xf0u : 0xfcu;
    }
    uint8_t *columns = bytes + file_size - GLYPHS;
    columns[0] = columns[1] = columns[4] = columns[5] = 5u << 4;
    columns[2] = 3u << 4;
    columns[3] = 1u << 4;
    snprintf(path, 128, "/tmp/tilefinch-glyph-component-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool paint_page_text(Budget *budget, FontSet *fonts, const char *text,
                            uint16_t frame[64u * 32u])
{
    char page[256];
    int length = snprintf(page, sizeof(page),
        "<!doctype html><html><body style=\"margin:0;background:#fff;color:#000;"
        "font:16px sans-serif\">%s</body></html>", text);
    CHECK(length > 0 && (size_t) length < sizeof(page));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, (size_t) length, 24)
          && stylesheet_build(&sheet, budget, &document, 64));
    CHECK(layout_build(&layout, budget, &document, &sheet, fonts, NULL, 64));
    TileCache cache = {0};
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, 64u * 32u)
          && tile_cache_render_frame(&cache, 0, 64, 32, NULL));
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    return true;
}

/* The vowels U+093F and U+09BF, U+09C7, U+09C8 read before the consonants they
   follow in memory. Without a shaped cluster for them the vowel still has to
   land in front, at the same total width. */
static bool test_prebase_vowel_leads_an_unlisted_cluster(void)
{
    static const char ti[] = "\xe0\xa4\xa4\xe0\xa4\xbf";
    static const char ti_in_visual_order[] = "\xe0\xa4\xbf\xe0\xa4\xa4";
    char path[128];
    CHECK(write_indic_fixture(path));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(
              provider, path, "glyph-devanagari")
          && font_optional_glyph_provider_install(provider));
    FontGlyph glyph;
    static const unsigned settled[] = {0x0924u, 0x093fu};
    for (size_t at = 0; at < 2u; at++) {
        CHECK(settled_scalar_glyph(provider, face, settled[at], 16, false, &glyph));
        font_glyph_destroy(face, &glyph);
    }

    /* U+0924 + U+093F: nothing covers it. */
    CHECK(font_indic_prebase_vowel(ti, 6u) == 3u);
    /* U+0915 + U+093F: the pack's own cluster. */
    CHECK(font_indic_prebase_vowel("\xe0\xa4\x95\xe0\xa4\xbf", 6u) == 0u);
    /* U+0924 U+094D U+0924 + U+093F: past both consonants. */
    CHECK(font_indic_prebase_vowel(
              "\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa4\xbf", 12u) == 9u);
    /* U+0915 U+094D U+0915 + U+093F: the last part draws the vowel itself. */
    CHECK(font_indic_prebase_vowel(
              "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbf", 12u) == 0u);
    /* U+0924 U+094D U+0915 + U+093F: a cluster that ends on the vowel takes
       the last consonant away from the sequence that would have drawn it. */
    CHECK(font_indic_prebase_vowel(
              "\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbf", 12u) == 9u);
    /* Bengali U+0995 + U+09C7. */
    CHECK(font_indic_prebase_vowel("\xe0\xa6\x95\xe0\xa7\x87", 6u) == 3u);
    CHECK(font_indic_prebase_vowel("\xe0\xa4\xa4x", 4u) == 0u);
    CHECK(font_indic_prebase_vowel("\xe0\xa4\xa4\xe0\xa4", 5u) == 0u);
    CHECK(font_indic_prebase_vowel("abc", 3u) == 0u);

    static uint16_t logical[64u * 32u], visual[64u * 32u];
    CHECK(paint_page_text(&budget, &fonts, ti, logical));
    CHECK(paint_page_text(&budget, &fonts, ti_in_visual_order, visual));
    size_t ink = 0;
    for (size_t at = 0; at < 64u * 32u; at++) ink += logical[at] != 0xffffu;
    CHECK(ink != 0);
    CHECK(memcmp(logical, visual, sizeof(logical)) == 0);

    /* The chrome painter draws the same way. */
    psp_ui_set_chrome_fonts(face, face, 2u);
    static uint16_t chrome_logical[480u * 272u], chrome_visual[480u * 272u];
    static PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_show_status(&ui, ti, 240);
    psp_ui_composite(&ui, chrome_logical, 480, 272, 480);
    psp_ui_init(&ui);
    psp_ui_show_status(&ui, ti_in_visual_order, 240);
    psp_ui_composite(&ui, chrome_visual, 480, 272, 480);
    CHECK(memcmp(chrome_logical, chrome_visual, sizeof(chrome_logical)) == 0);
    ink = 0;
    for (size_t at = 0; at < 480u * 272u; at++) ink += chrome_logical[at] != 0;
    CHECK(ink != 0);
    psp_ui_clear_chrome_font();

    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    (void) remove(path);
    return true;
}

/* U+0410 and U+0411 as scalars, and U+0410 U+0301 as a shaped cluster whose
   cell holds exactly what U+0411's does. */
static bool write_capital_mark_fixture(char path[128])
{
    enum { GLYPHS = 3, NOTICE = 32 };
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t sequences_offset = pages_offset + PAGE_BYTES;
    const size_t payload_offset = sequences_offset + SEQUENCE_BYTES;
    const size_t file_size = payload_offset + GLYPH_BYTES * GLYPHS + NOTICE;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);
    static const char id[] = "glyph-cyrillic";
    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_MONO;
    bytes[11] = 16;
    bytes[12] = 16;
    bytes[13] = 2;
    bytes[14] = (uint8_t) strlen(id);
    bytes[15] = 13;
    write_u32(bytes + 16, GLYPHS);
    write_u16(bytes + 20, 1);
    write_u16(bytes + 22, 1);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) sequences_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, id, strlen(id));
    write_u32(bytes + 72, NOTICE);
    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 4u * 2u, 0);
    uint8_t *page = bytes + pages_offset;
    page[4 + 0x10u / 8u] |= (uint8_t) (1u << (0x10u & 7u));
    page[4 + 0x11u / 8u] |= (uint8_t) (1u << (0x11u & 7u));
    uint8_t *sequence = bytes + sequences_offset;
    sequence[0] = 2;
    write_u32(sequence + 4, 2);
    write_u32(sequence + 8, 0x0410u);
    write_u32(sequence + 12, 0x0301u);
    uint8_t *payload = bytes + payload_offset;
    for (unsigned y = 8; y < 12u; y++)
        payload[y * 2u] = 0xf0u;
    for (unsigned y = 0; y < 12u; y++) {
        payload[GLYPH_BYTES + y * 2u] = 0xfcu;
        payload[GLYPH_BYTES * 2u + y * 2u] = 0xfcu;
    }
    uint8_t *columns = bytes + file_size - GLYPHS;
    columns[0] = 3u << 4;
    columns[1] = columns[2] = 5u << 4;
    snprintf(path, 128, "/tmp/tilefinch-glyph-component-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

/* A zero-width mark sits at the height of a small letter, so an accent on a
   capital collides with it. A pack can list the pair as a shaped cluster. */
static bool test_capital_and_accent_reach_a_shaped_cluster(void)
{
    static const char marked[] = "\xd0\x90\xcc\x81";
    static const char same_cell[] = "\xd0\x91";
    char path[128];
    CHECK(write_capital_mark_fixture(path));
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(provider, path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    FontGlyph glyph;
    CHECK(settled_scalar_glyph(provider, face, 0x0411u, 16, false, &glyph));
    font_glyph_destroy(face, &glyph);
    size_t used = 0;
    unsigned key = 0;
    CHECK(font_optional_glyph_match_sequence(marked, 4u, &used, &key)
          && used == 4u);
    /* The cluster's cell sits in its own block. */
    TilefinchGlyphSource source;
    if (!tilefinch_glyph_provider_source(provider, key, &source)) {
        bool changed = false;
        CHECK(tilefinch_glyph_provider_pump(
            provider, GLYPH_BYTES * 2u, &changed, NULL) && changed);
    }
    /* Other accents and a capital alone keep the ordinary path. */
    CHECK(!font_optional_glyph_match_sequence("\xd0\x90\xcc\x80", 4u, &used, &key));
    CHECK(!font_optional_glyph_match_sequence("\xd0\x90x", 3u, &used, &key));
    CHECK(font_text_width_at_size_fixed(face, marked, 4u, 16 * 64, false)
          == font_text_width_at_size_fixed(face, same_cell, 2u, 16 * 64, false));

    static uint16_t page[64u * 32u], cell[64u * 32u];
    CHECK(paint_page_text(&budget, &fonts, marked, page));
    CHECK(paint_page_text(&budget, &fonts, same_cell, cell));
    CHECK(memcmp(page, cell, sizeof(page)) == 0);

    psp_ui_set_chrome_fonts(face, face, 2u);
    static uint16_t chrome_marked[480u * 272u], chrome_cell[480u * 272u];
    static PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_show_status(&ui, marked, 240);
    psp_ui_composite(&ui, chrome_marked, 480, 272, 480);
    psp_ui_init(&ui);
    /* A toast's box counts characters, so match the count with a space. */
    psp_ui_show_status(&ui, "\xd0\x91 ", 240);
    psp_ui_composite(&ui, chrome_cell, 480, 272, 480);
    CHECK(memcmp(chrome_marked, chrome_cell, sizeof(chrome_cell)) == 0);
    psp_ui_clear_chrome_font();

    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    (void) remove(path);
    return true;
}

/* A pack has one weight. Bold text reaches it through a real bold face, which
   needs no synthesis for its own glyphs, so the pack glyph has to be emboldened
   there, and measuring must agree with painting. Nothing else may change. */
static bool test_bold_face_emboldens_pack_glyphs(void)
{
    /* U+0430: 5 columns from column 6. U+0448: 12 columns from column 2. */
    static const uint8_t columns[2] = {(4u << 4) | 6u, (11u << 4) | 2u};
    static const char wide[] = "\xd1\x88";
    static const char word[] = "\xd1\x88\xd0\xb0\xd1\x88";
    static const char latin[] = "Bold Latin";
    static const char emoji[] = "\xf0\x9f\x98\x80";
    char metric_path[128], legacy_path[128], color_path[128];
    CHECK(write_scalar_fixture(metric_path, "glyph-cyrillic", 13, columns, 32));
    CHECK(write_scalar_fixture(legacy_path, "glyph-cyrillic", 0, NULL, 0));
    CHECK(write_color_fixture(color_path));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL,
        TILEFINCH_TEST_SANS_BOLD_FONT, NULL, NULL, NULL,
        4u * 1024u * 1024u));
    const FontFace *regular = font_set_face_variant(
        &fonts, FONT_SANS, false, false);
    const FontFace *heavy = font_set_face_variant(
        &fonts, FONT_SANS, false, true);
    CHECK(regular != NULL && heavy != NULL && regular != heavy);
    const int latin_regular = font_text_width_at_size_fixed(
        regular, latin, strlen(latin), 16 * 64, false);
    const int latin_heavy = font_text_width_at_size_fixed(
        heavy, latin, strlen(latin), 16 * 64, false);
    FontGlyph latin_before;
    CHECK(font_glyph_load(heavy, 'H', 16, false, &latin_before));

    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(
              provider, metric_path, "glyph-cyrillic")
          && tilefinch_glyph_provider_attach(
              provider, color_path, "emoji-color-v1")
          && font_optional_glyph_provider_install(provider));

    /* The stems at columns 3, 6, 10 and 13 each gain one column. */
    FontGlyph plain, bold;
    CHECK(settled_scalar_glyph(provider, regular, 0x0448u, 16, false, &plain));
    CHECK(settled_scalar_glyph(provider, heavy, 0x0448u, 16, false, &bold));
    CHECK(plain.width == 12 && bold.width == 13);
    CHECK(plain.y_offset == -13 && bold.y_offset == -13);
    const unsigned char *plain_row = plain.pixels + 8u * 12u;
    const unsigned char *bold_row = bold.pixels + 8u * 13u;
    unsigned plain_ink = 0, bold_ink = 0;
    for (int x = 0; x < plain.width; x++) plain_ink += plain_row[x];
    for (int x = 0; x < bold.width; x++) bold_ink += bold_row[x];
    CHECK(plain_ink == 4u * 255u && bold_ink == 8u * 255u);
    CHECK(bold_row[1] == 255u && bold_row[2] == 255u && bold_row[3] == 0);

    /* Measuring agrees with the advance that painting uses, at any size and on
       both width paths, and a real bold face matches synthetic bold. */
    CHECK(plain.advance_fixed == 12 * 64 && bold.advance_fixed == 13 * 64);
    CHECK(font_text_width_at_size_fixed(regular, wide, 2, 16 * 64, false)
          == 12 * 64);
    CHECK(font_text_width_at_size_fixed(heavy, wide, 2, 16 * 64, false)
          == 13 * 64);
    CHECK(font_text_width_at_size_fixed(regular, wide, 2, 16 * 64, true)
          == 13 * 64);
    font_glyph_destroy(regular, &plain);
    font_glyph_destroy(heavy, &bold);
    FontGlyph narrow;
    CHECK(settled_scalar_glyph(provider, heavy, 0x0430u, 28, false, &narrow));
    CHECK(settled_scalar_glyph(provider, heavy, 0x0448u, 28, false, &bold));
    const int painted = bold.advance_fixed * 2 + narrow.advance_fixed;
    const int measured = font_text_width_at_size_fixed(
        heavy, word, strlen(word), 28 * 64, false);
    CHECK(measured >= painted - 32 && measured <= painted + 32);
    static const FontFamily families[] = {FONT_SYSTEM_SANS, FONT_HUMANIST_SANS};
    for (size_t at = 0; at < sizeof(families) / sizeof(families[0]); at++) {
        const int family = font_text_width_for_family_at_size_fixed(
            heavy, families[at], word, strlen(word), 28 * 64, false, true);
        if (family < painted - 32 || family > painted + 32) {
            fprintf(stderr, "family %d measures %d, paints %d\n",
                    (int) families[at], family, painted);
            return false;
        }
    }
    font_glyph_destroy(heavy, &narrow);
    font_glyph_destroy(heavy, &bold);

    /* Latin keeps its own faces, and color glyphs have no weight to add. */
    CHECK(font_text_width_at_size_fixed(
              regular, latin, strlen(latin), 16 * 64, false) == latin_regular);
    CHECK(font_text_width_at_size_fixed(
              heavy, latin, strlen(latin), 16 * 64, false) == latin_heavy);
    FontGlyph latin_after;
    CHECK(font_glyph_load(heavy, 'H', 16, false, &latin_after));
    CHECK(latin_after.width == latin_before.width
          && latin_after.height == latin_before.height
          && latin_after.advance_fixed == latin_before.advance_fixed
          && memcmp(latin_after.pixels, latin_before.pixels,
                    (size_t) latin_after.width
                        * (size_t) latin_after.height) == 0);
    font_glyph_destroy(heavy, &latin_after);
    FontGlyph color_plain, color_bold;
    CHECK(settled_scalar_glyph(
        provider, regular, 0x1f600u, 16, false, &color_plain));
    CHECK(settled_scalar_glyph(
        provider, heavy, 0x1f600u, 16, false, &color_bold));
    CHECK(color_plain.colors != NULL && color_bold.colors != NULL);
    CHECK(color_plain.width == color_bold.width
          && color_plain.advance_fixed == color_bold.advance_fixed
          && memcmp(color_plain.pixels, color_bold.pixels,
                    (size_t) color_plain.width
                        * (size_t) color_plain.height) == 0);
    CHECK(font_text_width_at_size_fixed(heavy, emoji, 4, 16 * 64, false)
          == color_plain.advance_fixed);
    font_glyph_destroy(regular, &color_plain);
    font_glyph_destroy(heavy, &color_bold);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);

    /* A pack with no metrics keeps its one-em advance and still gets heavier. */
    provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(
              provider, legacy_path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    CHECK(settled_scalar_glyph(provider, regular, 0x0448u, 16, false, &plain));
    CHECK(settled_scalar_glyph(provider, heavy, 0x0448u, 16, false, &bold));
    CHECK(plain.advance_fixed == 16 * 64
          && bold.advance_fixed == 16 * 64 + 22);
    CHECK(font_text_width_at_size_fixed(heavy, wide, 2, 16 * 64, false)
          == 16 * 64 + 22);
    CHECK(plain.pixels[8u * (size_t) plain.width + 4u] == 0
          && bold.pixels[8u * (size_t) bold.width + 4u] == 255u);
    font_glyph_destroy(regular, &plain);
    font_glyph_destroy(heavy, &bold);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_glyph_destroy(heavy, &latin_before);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    (void) remove(metric_path);
    (void) remove(legacy_path);
    (void) remove(color_path);
    return true;
}

static bool utf8_is_whole(const char *text)
{
    for (size_t at = 0, bytes = strlen(text); at < bytes;) {
        unsigned value = 0;
        size_t used = font_utf8_next(text + at, bytes - at, &value);
        if (used == 0 || value == 0xfffdu) return false;
        at += used;
    }
    return true;
}

/* Two-byte letters get half as many characters from a byte bound, and a cut
   that falls inside one draws as a missing glyph at the end of the line. */
static bool test_player_text_keeps_whole_letters(void)
{
    static PspUiMediaState media;
    static PspUiMediaPresentation presentation;
    psp_ui_media_init(&media);
    psp_ui_media_bind_presentation(&media, &presentation);
    char text[1024];
    for (size_t odd = 0; odd < 2u; odd++) {
        size_t at = 0;
        if (odd != 0) text[at++] = 'x';
        while (at + 2u < sizeof(text)) {
            text[at++] = '\xd1';
            text[at++] = '\x88';
        }
        text[at] = '\0';
        psp_ui_media_set_subtitle(&media, text);
        CHECK(utf8_is_whole(presentation.subtitle_text));
        CHECK(strlen(presentation.subtitle_text)
              >= PSP_UI_MEDIA_SUBTITLE_TEXT_CAPACITY - 2u);
    }
    /* Two rows of about 48 Cyrillic letters survive whole. */
    CHECK(PSP_UI_MEDIA_SUBTITLE_TEXT_CAPACITY > 96u * 2u);

    char *title = text;
    memset(title, 'x', 300);
    title[1] = '\xd1'; title[2] = '\x88';
    title[300] = '\0';
    title[PSP_UI_MEDIA_TITLE_CAPACITY - 2] = '\xd1';
    title[PSP_UI_MEDIA_TITLE_CAPACITY - 1] = '\x88';
    psp_ui_media_set(&media, true, true, false, 0, 0, title);
    CHECK(utf8_is_whole(media.title));
    return true;
}

/* A scalar script's letters share a baseline and keep their own widths. */
static bool test_scalar_pack_declares_baseline_and_widths(void)
{
    /* U+0430: 5 columns from column 6. U+0448: 12 columns from column 2. */
    static const uint8_t columns[2] = {(4u << 4) | 6u, (11u << 4) | 2u};
    char path[128];
    CHECK(write_scalar_fixture(path, "glyph-cyrillic", 13, columns, 32));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(provider, path, "glyph-cyrillic")
          && font_optional_glyph_provider_install(provider));
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    CHECK(face != NULL);

    uint32_t key = 0;
    unsigned width = 0, height = 0;
    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x0430u, &key, &width) && width == 5u);
    CHECK(tilefinch_glyph_provider_metrics(
        provider, key, &width, &height, NULL) && width == 5u && height == 16u);
    static const char narrow[] = "\xd0\xb0";
    static const char wide[] = "\xd1\x88";
    /* Measuring may not read the file, and nothing has been pumped yet. */
    CHECK(font_text_width_at_size_fixed(face, narrow, 2, 16 * 64, false)
          == 5 * 64);
    CHECK(font_text_width_at_size_fixed(face, wide, 2, 16 * 64, false)
          == 12 * 64);
    CHECK(font_text_width_at_size_fixed(face, wide, 2, 28 * 64, false)
          == 21 * 64);

    FontGlyph glyph;
    CHECK(settled_scalar_glyph(provider, face, 0x0430u, 16, false, &glyph));
    CHECK(glyph.width == 5 && glyph.height == 16 && glyph.y_offset == -13
          && glyph.advance == 5);
    /* Stems at columns 6 and 10 land at the box's edges. Column 1 is cut. */
    const unsigned char *row = glyph.pixels + 8u * 5u;
    CHECK(row[0] == 255u && row[1] == 0 && row[2] == 0 && row[3] == 0
          && row[4] == 255u);
    font_glyph_destroy(face, &glyph);
    CHECK(settled_scalar_glyph(provider, face, 0x0430u, 32, false, &glyph));
    CHECK(glyph.width == 10 && glyph.height == 32 && glyph.y_offset == -26);
    font_glyph_destroy(face, &glyph);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    font_set_destroy(&fonts);
    (void) remove(path);
    CHECK(budget.current == 0);

    /* The same bytes under a pack with no declared baseline stay square. */
    CHECK(write_scalar_fixture(path, "glyph-cyrillic", 0, columns, 32));
    provider = tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL && tilefinch_glyph_provider_attach(
        provider, path, "glyph-cyrillic"));
    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x0430u, &key, &width) && width == 16u);
    tilefinch_glyph_provider_destroy(provider);
    (void) remove(path);

    /* Malformed declarations are refused and leave nothing allocated. */
    static const uint8_t past_cell[2] = {(15u << 4) | 1u, 0u};
    static const struct {
        unsigned baseline_rows;
        const uint8_t *columns;
        size_t notice_bytes;
    } refused[] = {
        {16, columns, 32},   /* baseline outside the cell */
        {13, past_cell, 32}, /* a glyph's box runs past the cell */
        {13, NULL, 1}        /* no room for one byte per glyph */
    };
    for (size_t at = 0; at < sizeof(refused) / sizeof(refused[0]); at++) {
        CHECK(write_scalar_fixture(
            path, "glyph-cyrillic", refused[at].baseline_rows,
            refused[at].columns, refused[at].notice_bytes));
        provider = tilefinch_glyph_provider_create(&budget);
        CHECK(provider != NULL);
        size_t retained = budget.current;
        if (tilefinch_glyph_provider_attach(provider, path, "glyph-cyrillic")) {
            fprintf(stderr, "refused case %zu was accepted\n", at);
            return false;
        }
        CHECK(budget.current == retained);
        tilefinch_glyph_provider_destroy(provider);
        (void) remove(path);
    }
    CHECK(budget.current == 0);
    return true;
}

static bool test_bounded_pack_provider(void)
{
    char path[128];
    char color_path[128];
    char extra_path[128];
    char fourth_path[128];
    char overflow_path[128];
    CHECK(write_fixture(path, "glyph-ja"));
    CHECK(write_color_fixture(color_path));
    CHECK(write_fixture(extra_path, "glyph-cyrillic"));
    CHECK(write_fixture(fourth_path, "glyph-latin-extended"));
    CHECK(write_fixture(overflow_path, "glyph-ko"));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL);
    CHECK(!tilefinch_glyph_provider_attach(provider, path, "wrong-id"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, path, "glyph-ja"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, color_path, "emoji-color-v1"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, extra_path, "glyph-cyrillic"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, fourth_path, "glyph-latin-extended"));
    CHECK(tilefinch_glyph_provider_pack_count(provider) == 4
          && !tilefinch_glyph_provider_attach(
                 provider, overflow_path, "glyph-ko"));
    CHECK(font_optional_glyph_provider_install(provider));
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    CHECK(face != NULL);

    uint32_t key = 0;
    unsigned width = 0;
    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x4e00u, &key, &width));
    CHECK(width == 16);
    TilefinchGlyphSource source;
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_key_pending(provider, key));
    FontGlyph glyph;
    CHECK(font_glyph_load(face, 0x4e00u, 16, false, &glyph));
    CHECK(glyph.provider_pending && glyph.colors == NULL);
    font_glyph_destroy(face, &glyph);
    /* A chrome label can be painted before its pack's queued block arrives.
       Repainting after the pump must replace that placeholder, not cache it
       for the rest of the session. */
    static uint16_t pending_pixels[480 * 272];
    static uint16_t ready_pixels[480 * 272];
    static uint16_t fresh_pixels[480 * 272];
    PspUiState ui;
    psp_ui_init(&ui);
    psp_ui_show_status(&ui, "\xe4\xb8\x80", 180);
    psp_ui_set_chrome_fonts(face, NULL, 1);
    psp_ui_composite(&ui, pending_pixels, 480, 272, 480);
    bool changed = true;
    size_t bytes_read = 99;
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES - 1u, &changed, &bytes_read));
    CHECK(!changed && bytes_read == 0);
    CHECK(tilefinch_glyph_provider_key_pending(provider, key));
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES * 2u, &changed, &bytes_read));
    CHECK(changed && bytes_read == GLYPH_BYTES * 2u);
    CHECK(tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(source.kind == TILEFINCH_GLYPH_COMPONENT_MONO);
    CHECK(source.width == 16 && source.height == 16);
    CHECK(source.pixels[0] == 0x31u);
    psp_ui_composite(&ui, ready_pixels, 480, 272, 480);
    psp_ui_clear_chrome_font();
    psp_ui_set_chrome_fonts(face, NULL, 1);
    psp_ui_composite(&ui, fresh_pixels, 480, 272, 480);
    CHECK(memcmp(ready_pixels, fresh_pixels, sizeof(ready_pixels)) == 0);
    CHECK(memcmp(pending_pixels, ready_pixels, sizeof(ready_pixels)) != 0);
    psp_ui_clear_chrome_font();
    CHECK(font_glyph_load(face, 0x4e00u, 16, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors == NULL
          && glyph.width == 16 && glyph.height == 16);
    font_glyph_destroy(face, &glyph);
    CHECK(font_glyph_load(face, 0x4e00u, 8, false, &glyph));
    /* The source has ink at x=7, between the old nearest samples 6 and 8. */
    CHECK(!glyph.provider_pending && glyph.width == 8 && glyph.height == 8
          && glyph.pixels[3] > 0 && glyph.pixels[3] < 255);
    font_glyph_destroy(face, &glyph);

    static const char family[] =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9!";
    size_t used = 0;
    CHECK(tilefinch_glyph_provider_match(
        provider, family, sizeof(family) - 1u, &used, &key, &width));
    CHECK(used == 11u && width == 16);
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES * 2u, &changed, &bytes_read));
    CHECK(tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(source.pixels[0] == 0x34u);
    unsigned sequence_key = 0;
    size_t font_used = 0;
    CHECK(font_optional_glyph_match_sequence(
        family, sizeof(family) - 1u, &font_used, &sequence_key));
    CHECK(font_used == 11u && sequence_key == key);
    CHECK(font_face_has_codepoint(face, sequence_key));
    size_t sequence_start = 99u;
    CHECK(font_optional_glyph_match_sequence_ending_at(
        family, font_used, &sequence_start, &sequence_key));
    CHECK(sequence_start == 0 && sequence_key == key);
    CHECK(font_text_width_at_size_fixed(
        face, family, font_used, 16 * 64, false) == 16 * 64);
    CHECK(font_glyph_load_at_size(
        face, sequence_key, 16 * 64, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors == NULL
          && glyph.width == 16 && glyph.height == 16);
    font_glyph_destroy(face, &glyph);

    static const char copyright_emoji[] = "\xc2\xa9\xef\xb8\x8f";
    CHECK(font_optional_glyph_match_sequence(
        copyright_emoji, sizeof(copyright_emoji) - 1u,
        &font_used, &sequence_key));
    CHECK(font_used == sizeof(copyright_emoji) - 1u);
    CHECK(font_optional_glyph_match_sequence_ending_at(
        copyright_emoji, font_used, &sequence_start, &key));
    CHECK(sequence_start == 0 && key == sequence_key);

    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x1f600u, &key, &width));
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_pump(
        provider, 12u, &changed, &bytes_read));
    CHECK(changed && bytes_read == 12u);
    CHECK(font_glyph_load(face, 0x1f600u, 16, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors != NULL
          && glyph.pixels != NULL && glyph.width == 16 && glyph.height == 16
          && glyph.colors[0] == UINT16_C(0xf800)
          && glyph.pixels[0] == 255u);
    font_glyph_destroy(face, &glyph);

    font_set_destroy(&fonts);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    CHECK(budget.current == 0);

    uint8_t package_digest[32];
    uint64_t package_size = 0;
    CHECK(hash_file(path, package_digest, &package_size));
    char root[] = "/tmp/tilefinch-glyph-store-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char staged[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(stage_fixture_download(
        root, TILEFINCH_GLYPH_PACK_JAPANESE, path, staged));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    TilefinchUpdateManifest manifest = {
        .package_format = TILEFINCH_UPDATE_PACKAGE_GLYPH,
        .package_size = package_size
    };
    memcpy(manifest.package_sha256, package_digest, 32);
    snprintf(manifest.asset, sizeof(manifest.asset), "%s",
             tilefinch_glyph_pack_spec(TILEFINCH_GLYPH_PACK_JAPANESE)
                 ->pack_asset);
    uint8_t envelope[16] = {0};
    uint8_t manifest_digest[32];
    memset(manifest_digest, 0x5a, sizeof(manifest_digest));
    TilefinchGlyphComponentInstall *install =
        tilefinch_glyph_component_install_create(
            &budget, &(TilefinchGlyphComponentInstallOptions) {
                .envelope = envelope,
                .envelope_length = sizeof(envelope),
                .manifest = &manifest,
                .manifest_digest = manifest_digest,
                .install_root = root,
                .pack = TILEFINCH_GLYPH_PACK_JAPANESE
            });
    CHECK(install != NULL);
    TilefinchUpdateInstallSnapshot snapshot = {0};
    for (size_t pump = 0; pump < 4096u; pump++) {
        CHECK(tilefinch_glyph_component_install_pump(install, 7u));
        CHECK(tilefinch_glyph_component_install_snapshot(install, &snapshot));
        if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE);
    tilefinch_glyph_component_install_destroy(install);
    char installed[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(tilefinch_glyph_component_resolve(
              &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
              installed, sizeof(installed))
          && strstr(installed, "/glyph-ja/active/pack.tfgf") != NULL);
    CHECK(tilefinch_glyph_component_remove(
              &paths, TILEFINCH_GLYPH_PACK_JAPANESE)
          && !tilefinch_glyph_component_resolve(
                 &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                 installed, sizeof(installed)));
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup),
             "%s/components/glyph-ja/UNINSTALLED", root);
    CHECK(unlink(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-ja", root);
    CHECK(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
    CHECK(remove(color_path) == 0);
    CHECK(remove(extra_path) == 0);
    CHECK(remove(fourth_path) == 0);
    CHECK(remove(overflow_path) == 0);
    return true;
}

static bool install_test_generation(
    Budget *budget, const char *root, TilefinchGlyphPack pack,
    bool cancel_early)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char fixture[128];
    char package[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(spec != NULL && write_fixture(fixture, spec->id));
    uint8_t package_digest[32];
    uint64_t package_size = 0;
    CHECK(hash_file(fixture, package_digest, &package_size));
    CHECK(stage_fixture_download(root, pack, fixture, package));
    TilefinchUpdateManifest manifest = {
        .package_format = TILEFINCH_UPDATE_PACKAGE_GLYPH,
        .package_size = package_size
    };
    memcpy(manifest.package_sha256, package_digest, 32);
    snprintf(manifest.asset, sizeof(manifest.asset), "%s", spec->pack_asset);
    uint8_t envelope[16] = {0};
    uint8_t manifest_digest[32];
    memset(manifest_digest, 0x6b, sizeof(manifest_digest));
    TilefinchGlyphComponentInstall *install =
        tilefinch_glyph_component_install_create(
            budget, &(TilefinchGlyphComponentInstallOptions) {
                .envelope = envelope,
                .envelope_length = sizeof(envelope),
                .manifest = &manifest,
                .manifest_digest = manifest_digest,
                .install_root = root,
                .pack = pack
            });
    CHECK(install != NULL);
    TilefinchUpdateInstallSnapshot snapshot = {0};
    if (cancel_early) {
        CHECK(tilefinch_glyph_component_install_pump(install, 1u)
              && tilefinch_glyph_component_install_cancel(install)
              && tilefinch_glyph_component_install_pump(install, 1u)
              && tilefinch_glyph_component_install_snapshot(
                     install, &snapshot)
              && snapshot.phase == TILEFINCH_UPDATE_INSTALL_CANCELLED);
    } else {
        for (size_t pump = 0; pump < 4096u; pump++) {
            CHECK(tilefinch_glyph_component_install_pump(install, 7u)
                  && tilefinch_glyph_component_install_snapshot(
                         install, &snapshot));
            if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
        }
        CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
              && snapshot.status == TILEFINCH_UPDATE_OK);
    }
    tilefinch_glyph_component_install_destroy(install);
    (void) remove(package);
    return true;
}

static bool test_new_pack_store_lifecycle(void)
{
    static const TilefinchGlyphPack packs[] = {
        TILEFINCH_GLYPH_PACK_CYRILLIC,
        TILEFINCH_GLYPH_PACK_LATIN_EXTENDED,
        TILEFINCH_GLYPH_PACK_ARABIC,
        TILEFINCH_GLYPH_PACK_HEBREW,
        TILEFINCH_GLYPH_PACK_BENGALI_ASSAMESE
    };
    for (size_t at = 0; at < sizeof(packs) / sizeof(packs[0]); at++) {
        const TilefinchGlyphPack pack = packs[at];
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
        char root[] = "/tmp/tilefinch-glyph-new-store-XXXXXX";
        CHECK(spec != NULL && mkdtemp(root) != NULL);
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        TilefinchInstallPaths paths = {.slotted = true};
        snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);

        /* Cancellation before promotion leaves no generation to resolve. */
        CHECK(install_test_generation(&budget, root, pack, true));
        char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
        CHECK(!tilefinch_glyph_component_resolve(
            &paths, pack, resolved, sizeof(resolved)));

        /* Two promotions exercise active-to-previous rotation generically. */
        CHECK(install_test_generation(&budget, root, pack, false)
              && install_test_generation(&budget, root, pack, false)
              && tilefinch_glyph_component_resolve(
                     &paths, pack, resolved, sizeof(resolved)));
        TilefinchGlyphProvider *provider =
            tilefinch_glyph_provider_create(&budget);
        CHECK(provider != NULL
              && tilefinch_glyph_provider_attach(
                     provider, resolved, spec->id));
        tilefinch_glyph_provider_destroy(provider);

        char marker[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(marker, sizeof(marker), "%s/components/%s/UNINSTALLED",
                 root, spec->id);
        FILE *file = fopen(marker, "wb");
        CHECK(file != NULL && fputs("TFGRv1\n", file) >= 0
              && fclose(file) == 0
              && !tilefinch_glyph_component_resolve(
                     &paths, pack, resolved, sizeof(resolved)));
        CHECK(unlink(marker) == 0
              && tilefinch_glyph_component_resolve(
                     &paths, pack, resolved, sizeof(resolved))
              && tilefinch_glyph_component_remove(&paths, pack)
              && !tilefinch_glyph_component_resolve(
                     &paths, pack, resolved, sizeof(resolved)));
        CHECK(unlink(marker) == 0);
        char component[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(component, sizeof(component), "%s/components/%s",
                 root, spec->id);
        CHECK(rmdir(component) == 0);
        snprintf(component, sizeof(component), "%s/components", root);
        CHECK(rmdir(component) == 0 && rmdir(root) == 0);
        CHECK(budget.current == 0);
    }
    return true;
}

#include "suites/glyph_pack_offer.inc"
#include "suites/glyph_session_reattach.inc"

typedef struct {
    TilefinchGlyphInstallFaultPoint point;
    bool fired;
    bool persistent;
} GlyphInstallFault;

static bool glyph_install_fault_once(
    void *opaque, TilefinchGlyphInstallFaultPoint point)
{
    GlyphInstallFault *fault = opaque;
    if (fault == NULL || fault->point != point
        || (fault->fired && !fault->persistent)) return false;
    fault->fired = true;
    return true;
}

static TilefinchGlyphComponentInstall *create_fixture_install(
    Budget *budget, const char *root, const char *package,
    TilefinchGlyphInstallFaultHook fault, void *fault_opaque)
{
    uint8_t digest[32];
    uint64_t size = 0;
    if (!hash_file(package, digest, &size)) return NULL;
    TilefinchUpdateManifest manifest = {
        .package_format = TILEFINCH_UPDATE_PACKAGE_GLYPH,
        .package_size = size
    };
    memcpy(manifest.package_sha256, digest, sizeof(digest));
    snprintf(manifest.asset, sizeof(manifest.asset), "%s",
             tilefinch_glyph_pack_spec(TILEFINCH_GLYPH_PACK_JAPANESE)
                 ->pack_asset);
    static const uint8_t envelope[16] = {0};
    static const uint8_t manifest_digest[32] = {0x6d};
    return tilefinch_glyph_component_install_create(
        budget, &(TilefinchGlyphComponentInstallOptions) {
            .envelope = envelope,
            .envelope_length = sizeof(envelope),
            .manifest = &manifest,
            .manifest_digest = manifest_digest,
            .install_root = root,
            .pack = TILEFINCH_GLYPH_PACK_JAPANESE,
            .fault = fault,
            .fault_opaque = fault_opaque
        });
}

static bool pump_fixture_install(
    TilefinchGlyphComponentInstall *install,
    TilefinchUpdateInstallSnapshot *snapshot)
{
    for (size_t pump = 0; pump < 4096u; pump++) {
        CHECK(tilefinch_glyph_component_install_pump(install, 7u)
              && tilefinch_glyph_component_install_snapshot(
                  install, snapshot));
        if (snapshot->phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) return true;
    }
    return false;
}

static bool test_phase_aware_install_faults(void)
{
    static const TilefinchGlyphInstallFaultPoint faults[] = {
        TILEFINCH_GLYPH_INSTALL_FAULT_SELF_CHECK,
        TILEFINCH_GLYPH_INSTALL_FAULT_REMOVE_PREVIOUS,
        TILEFINCH_GLYPH_INSTALL_FAULT_ACTIVATE_CANDIDATE,
        TILEFINCH_GLYPH_INSTALL_FAULT_TOMBSTONE_UNLINK
    };
    const size_t fault_count = sizeof(faults) / sizeof(faults[0]);
    /* The extra last pass has no fault hook. Every Memory Stick sync is
       rejected instead, as some real firmware/storage-driver combinations
       do after checked writes and renames succeeded (PPSSPP accepts it). */
    for (size_t at = 0; at <= fault_count; at++) {
        bool reject_sync = at == fault_count;
        char root[] = "/tmp/tilefinch-glyph-fault-XXXXXX";
        CHECK(mkdtemp(root) != NULL);
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        /* Two generations ensure previous-generation cleanup is real rather
           than an ENOENT-only control. */
        CHECK(install_test_generation(
                  &budget, root, TILEFINCH_GLYPH_PACK_JAPANESE, false)
              && install_test_generation(
                  &budget, root, TILEFINCH_GLYPH_PACK_JAPANESE, false));
        TilefinchInstallPaths paths = {.slotted = true};
        snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
        char old_active[TILEFINCH_INSTALL_PATH_LIMIT];
        CHECK(tilefinch_glyph_component_resolve(
            &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
            old_active, sizeof(old_active)));

        if (!reject_sync
            && faults[at] == TILEFINCH_GLYPH_INSTALL_FAULT_TOMBSTONE_UNLINK) {
            char tombstone[TILEFINCH_INSTALL_PATH_LIMIT];
            snprintf(tombstone, sizeof(tombstone),
                     "%s/components/glyph-ja/UNINSTALLED", root);
            FILE *file = fopen(tombstone, "wb");
            CHECK(file != NULL && fputs("TFGRv1\n", file) >= 0
                  && fclose(file) == 0);
        }

        char fixture[128];
        char package[TILEFINCH_INSTALL_PATH_LIMIT];
        CHECK(write_fixture(fixture, "glyph-ja"));
        CHECK(stage_fixture_download(
            root, TILEFINCH_GLYPH_PACK_JAPANESE, fixture, package));
        bool pre_activation = !reject_sync && faults[at]
            <= TILEFINCH_GLYPH_INSTALL_FAULT_ACTIVATE_CANDIDATE;
        GlyphInstallFault fault = {
            .point = reject_sync ? 0 : faults[at],
            .persistent = !pre_activation
        };
        if (reject_sync)
            tilefinch_memory_stick_sync_set_host_result(
                TILEFINCH_MEMORY_STICK_SYNC_REJECTED);
        TilefinchGlyphComponentInstall *install = create_fixture_install(
            &budget, root, package,
            reject_sync ? NULL : glyph_install_fault_once,
            reject_sync ? NULL : &fault);
        CHECK(install != NULL);
        TilefinchUpdateInstallSnapshot snapshot = {0};
        if (pre_activation) {
            CHECK(pump_fixture_install(install, &snapshot) && fault.fired);
            struct stat information;
            char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
            CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
                  && !tilefinch_glyph_component_install_activated(install)
                  && stat(package, &information) == 0
                  && tilefinch_glyph_component_resolve(
                      &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                      resolved, sizeof(resolved))
                  && strcmp(resolved, old_active) == 0);
            /* The retry stages from this directory: only the package stays. */
            char ready[TILEFINCH_INSTALL_PATH_LIMIT];
            snprintf(ready, sizeof(ready),
                     "%s/components/glyph-ja/candidate.tmp/READY", root);
            CHECK(access(ready, F_OK) != 0);
            tilefinch_glyph_component_install_destroy(install);
            install = create_fixture_install(
                &budget, root, package, NULL, NULL);
            memset(&snapshot, 0, sizeof(snapshot));
            CHECK(install != NULL && pump_fixture_install(install, &snapshot)
                  && snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE);
        } else if (reject_sync) {
            /* The sync is a durability hint, so the verified pack is
               installed, not reported as failed. */
            char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
            CHECK(pump_fixture_install(install, &snapshot)
                  && snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
                  && tilefinch_glyph_component_install_activated(install)
                  && access(package, F_OK) != 0
                  && tilefinch_glyph_component_resolve(
                      &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                      resolved, sizeof(resolved)));
        } else {
            char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
            CHECK(pump_fixture_install(install, &snapshot) && fault.fired);
            /* Reconciliation exhausted its bounded retries. Active—not the
               vanished download path—still owns the verified pack. This is
               also the power-loss boundary the session re-probes. */
            CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR
                  && tilefinch_glyph_component_install_activated(install)
                  && access(package, F_OK) != 0
                  && tilefinch_glyph_component_resolve(
                      &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                      resolved, sizeof(resolved)));
            tilefinch_glyph_component_install_destroy(install);
            install = NULL;
            CHECK(tilefinch_glyph_component_resolve(
                &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                resolved, sizeof(resolved)));
        }
        tilefinch_glyph_component_install_destroy(install);
        /* Removal still succeeds when its syncs are rejected. */
        CHECK(tilefinch_glyph_component_remove(
            &paths, TILEFINCH_GLYPH_PACK_JAPANESE));
        tilefinch_memory_stick_sync_set_host_result(
            TILEFINCH_MEMORY_STICK_SYNC_SKIPPED);
        char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(cleanup, sizeof(cleanup),
                 "%s/components/glyph-ja/UNINSTALLED", root);
        CHECK(unlink(cleanup) == 0);
        snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-ja", root);
        CHECK(rmdir(cleanup) == 0);
        snprintf(cleanup, sizeof(cleanup), "%s/components", root);
        CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
        CHECK(budget.current == 0);
    }
    return true;
}

/* IS-18: a block whose read fails leaves the queue (it used to stay at its
   head, retried every frame ahead of every other block), and a pack whose
   reads keep failing stops queueing blocks. */
static bool test_failed_block_read_is_dropped(void)
{
    char path[128];
    CHECK(write_scalar_fixture(path, "glyph-cyrillic", 0, NULL, 0));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL
          && tilefinch_glyph_provider_attach(provider, path, "glyph-cyrillic"));
    uint32_t key = 0;
    TilefinchGlyphSource source;
    CHECK(tilefinch_glyph_provider_has_codepoint(provider, 0x0448u, &key,
                                                 NULL));
    /* Every payload read fails as a card read error would. */
    tilefinch_test_faults()->fail_glyph_block_reads = 8u;
    for (unsigned attempt = 0; attempt < 4u; attempt++) {
        CHECK(!tilefinch_glyph_provider_source(provider, key, &source)
              && tilefinch_glyph_provider_key_pending(provider, key));
        bool changed = true;
        CHECK(!tilefinch_glyph_provider_pump(
                  provider, GLYPH_BYTES * 2u, &changed, NULL)
              && !changed);
        CHECK(!tilefinch_glyph_provider_key_pending(provider, key));
        bool idle = tilefinch_glyph_provider_pump(
            provider, GLYPH_BYTES * 2u, &changed, NULL);
        CHECK(idle && !changed);
    }
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source)
          && !tilefinch_glyph_provider_key_pending(provider, key)
          && tilefinch_test_faults()->fail_glyph_block_reads == 4u);
    tilefinch_test_faults()->fail_glyph_block_reads = 0;
    tilefinch_glyph_provider_destroy(provider);
    CHECK(remove(path) == 0 && budget.current == 0);
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--session-remove-error-only") == 0)
        return !test_glyph_session_remove_retirement(true);
    if (argc == 2 && strcmp(argv[1], "--session-remove-success-only") == 0)
        return !test_glyph_session_remove_retirement(false);
    if (argc == 2 && strcmp(argv[1], "--session-isolation-only") == 0)
        return !test_glyph_session_install_retirement(0u);
    if (argc == 2 && strcmp(argv[1], "--session-cancel-only") == 0)
        return !test_glyph_session_install_retirement(1u);
    if (argc == 2 && strcmp(argv[1], "--session-error-only") == 0)
        return !test_glyph_session_install_retirement(2u);
    /* Before any test installs an optional provider: the offer checks
       the embedded faces alone. */
    if (!test_glyph_offer_detection()) return 1;
    if (!test_empty_provider_match()) return 1;
    if (!test_glyph_offer_session_memory()) return 1;
    if (!test_glyph_offer_decline_persists()) return 1;
    if (!test_glyph_session_install_reattaches()) return 1;
    if (!test_glyph_session_remove_retirement(true)
        || !test_glyph_session_remove_retirement(false)) return 1;
    for (unsigned mode = 0; mode < 3u; mode++)
        if (!test_glyph_session_install_retirement(mode)) return 1;
    for (unsigned mode = 0; mode < 3u; mode++)
        if (!test_glyph_session_lazy_attach(mode)) {
            fprintf(stderr, "lazy attach mode %u failed\n", mode);
            return 1;
        }
    if (!test_glyph_session_installed_status()) return 1;
    if (!test_glyph_session_client_failure_reported()) return 1;
    if (!test_glyph_session_install_refusal_reported()) return 1;
    if (!test_glyph_session_select_failure_reasons()) return 1;
    if (!test_pack_catalog()) return 1;
    if (!test_authored_sample_packed_pixels()) return 1;
    if (!test_script_sequence_matching()) return 1;
    if (!test_bounded_pack_provider()) return 1;
    if (!test_mono_cell_scaling_keeps_stem_weight()) return 1;
    if (!test_failed_block_read_is_dropped()) return 1;
    if (!test_scalar_pack_declares_baseline_and_widths()) return 1;
    if (!test_bold_face_emboldens_pack_glyphs()) return 1;
    if (!test_metric_pack_marks_sit_on_the_letter()) return 1;
    if (!test_prebase_vowel_leads_an_unlisted_cluster()) return 1;
    if (!test_capital_and_accent_reach_a_shaped_cluster()) return 1;
    if (!test_player_text_keeps_whole_letters()) return 1;
    if (!test_new_pack_store_lifecycle()) return 1;
    if (!test_phase_aware_install_faults()) return 1;
    if (!test_release_artifacts()) return 1;
    puts("glyph-component-tests status=PASS");
    return 0;
}
