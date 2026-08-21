#include "tilefinch/font.h"
#include "tilefinch/glyph_component.h"
#include "tilefinch/platform.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

#define STBTT_malloc(size, user) budget_malloc((Budget *) (user), (size))
#define STBTT_free(pointer, user) budget_free((Budget *) (user), (pointer))
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#ifdef TILEFINCH_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYSTEM_H
#endif

#define WOFF_HEADER_BYTES 44u
#define SFNT_TRUETYPE_SIGNATURE UINT32_C(0x00010000)
#define WOFF_SIGNATURE UINT32_C(0x774f4646)

#include "generated/font_fallback_bitmaps.inc"

static uint32_t read_be32(const unsigned char *data)
{
    return (uint32_t) data[0] << 24 | (uint32_t) data[1] << 16
        | (uint32_t) data[2] << 8 | data[3];
}

static bool face_adopt_trusted(FontFace *face, Budget *budget,
                               unsigned char *data, size_t data_length)
{
    if (face == NULL || budget == NULL || data == NULL || data_length == 0) {
        return false;
    }
    stbtt_fontinfo *info = budget_calloc(budget, 1, sizeof(*info));
    if (info == NULL) return false;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(info, data, offset)) {
        budget_free(budget, info);
        return false;
    }
    info->userdata = budget;
    *face = (FontFace) {
        .budget = budget,
        .data = data,
        .data_length = data_length,
        .implementation = info,
        .backend = FONT_FACE_BACKEND_STB_TRUSTED,
        .loaded = true
    };
    return true;
}

static bool face_load(FontFace *face, Budget *budget, const char *path,
                      size_t remaining)
{
    memset(face, 0, sizeof(*face));
    if (path == NULL || path[0] == '\0') return false;
    unsigned char *data = NULL;
    size_t data_length = 0;
    if (!tilefinch_platform_read_asset(budget, path, remaining, &data,
                                    &data_length)) return false;
    if (face_adopt_trusted(face, budget, data, data_length)) return true;
    budget_free(budget, data);
    return false;
}

#ifdef TILEFINCH_HAVE_FREETYPE
typedef struct {
    Budget *budget;
    size_t current_bytes;
    size_t maximum_bytes;
    struct FT_MemoryRec_ memory;
    FT_Library library;
    FT_Face face;
} FreeTypeWebFace;

static void *freetype_allocate(FT_Memory memory, long size)
{
    FreeTypeWebFace *state = memory == NULL ? NULL : memory->user;
    if (state == NULL || size <= 0 || (size_t) size > state->maximum_bytes
        || state->current_bytes > state->maximum_bytes - (size_t) size) {
        return NULL;
    }
    void *allocation = budget_malloc(state->budget, (size_t) size);
    if (allocation != NULL) state->current_bytes += (size_t) size;
    return allocation;
}

static void freetype_release(FT_Memory memory, void *block)
{
    FreeTypeWebFace *state = memory == NULL ? NULL : memory->user;
    if (state == NULL || block == NULL) return;
    size_t size = budget_usable_size(block);
    if (size <= state->current_bytes) state->current_bytes -= size;
    else state->current_bytes = 0;
    budget_free(state->budget, block);
}

static void *freetype_reallocate(FT_Memory memory, long current_size,
                                 long requested_size, void *block)
{
    (void) current_size;
    FreeTypeWebFace *state = memory == NULL ? NULL : memory->user;
    if (state == NULL || requested_size <= 0) {
        if (state != NULL && block != NULL) freetype_release(memory, block);
        return NULL;
    }
    size_t previous = block == NULL ? 0 : budget_usable_size(block);
    size_t requested = (size_t) requested_size;
    size_t retained = previous <= state->current_bytes
                      ? state->current_bytes - previous : 0;
    if (requested > state->maximum_bytes
        || retained > state->maximum_bytes - requested) return NULL;
    void *replacement = budget_realloc(state->budget, block, requested);
    if (replacement != NULL) state->current_bytes = retained + requested;
    return replacement;
}

static void freetype_web_destroy(FreeTypeWebFace *state)
{
    if (state == NULL) return;
    Budget *budget = state->budget;
    if (state->face != NULL) FT_Done_Face(state->face);
    if (state->library != NULL) FT_Done_Library(state->library);
    budget_free(budget, state);
}

static bool face_adopt_web(FontFace *output, Budget *budget,
                           unsigned char *data, size_t data_length,
                           size_t maximum_backend_bytes)
{
    FreeTypeWebFace *state = budget_calloc(budget, 1, sizeof(*state));
    if (state == NULL) return false;
    state->budget = budget;
    state->maximum_bytes = maximum_backend_bytes;
    state->memory.user = state;
    state->memory.alloc = freetype_allocate;
    state->memory.free = freetype_release;
    state->memory.realloc = freetype_reallocate;
    if (FT_New_Library(&state->memory, &state->library) != 0) {
        freetype_web_destroy(state);
        return false;
    }
    FT_Add_Default_Modules(state->library);
    if (FT_New_Memory_Face(state->library, data, (FT_Long) data_length, 0,
                           &state->face) != 0
        || !FT_IS_SCALABLE(state->face) || !FT_HAS_HORIZONTAL(state->face)
        || state->face->num_glyphs <= 0 || state->face->num_glyphs > 65535
        || state->face->units_per_EM < 16
        || state->face->units_per_EM > 16384
        || FT_Select_Charmap(state->face, FT_ENCODING_UNICODE) != 0) {
        freetype_web_destroy(state);
        return false;
    }
    *output = (FontFace) {
        .budget = budget,
        .data = data,
        .data_length = data_length,
        .implementation = state,
        .backend = FONT_FACE_BACKEND_FREETYPE_WEB,
        .loaded = true
    };
    return true;
}
#endif

bool font_face_load_encoded(FontFace *face, Budget *budget,
                            const unsigned char *data, size_t length,
                            size_t maximum_backend_bytes)
{
    if (face == NULL || budget == NULL || data == NULL || length < 4
        || length > (size_t) LONG_MAX
        || maximum_backend_bytes == 0 || face->loaded
        || face->data != NULL || face->implementation != NULL) return false;
    uint32_t signature = read_be32(data);
    if (signature == WOFF_SIGNATURE) {
        if (length < WOFF_HEADER_BYTES
            || read_be32(data + 4) != SFNT_TRUETYPE_SIGNATURE
            || read_be32(data + 8) != length
            || read_be32(data + 16) > maximum_backend_bytes) return false;
    } else if (signature != SFNT_TRUETYPE_SIGNATURE
               || length > maximum_backend_bytes) {
        return false;
    }
#ifdef TILEFINCH_HAVE_FREETYPE
    unsigned char *encoded = budget_malloc(budget, length);
    if (encoded == NULL) return false;
    memcpy(encoded, data, length);
    if (face_adopt_web(face, budget, encoded, length,
                       maximum_backend_bytes)) return true;
    budget_free(budget, encoded);
#endif
    return false;
}

bool font_set_load(FontSet *fonts, Budget *budget,
                   const char *sans_path, const char *serif_path,
                   const char *sans_italic_path, const char *sans_bold_path,
                   const char *serif_bold_path,
                   const char *metric_sans_path,
                   const char *metric_sans_bold_path,
                   size_t max_total_bytes)
{
    if (fonts == NULL || budget == NULL || max_total_bytes == 0) return false;
    memset(fonts, 0, sizeof(*fonts));
    bool sans = face_load(&fonts->sans, budget, sans_path, max_total_bytes);
    size_t remaining = sans
        ? (fonts->sans.data_length < max_total_bytes
           ? max_total_bytes - fonts->sans.data_length : 0)
        : max_total_bytes;
    bool serif = remaining != 0
                 && face_load(&fonts->serif, budget, serif_path, remaining);
    if (serif && fonts->serif.data_length < remaining) {
        remaining -= fonts->serif.data_length;
    } else if (serif) {
        remaining = 0;
    }
    if (remaining != 0) {
        (void) face_load(&fonts->sans_italic, budget, sans_italic_path,
                         remaining);
        if (fonts->sans_italic.loaded) remaining -= fonts->sans_italic.data_length;
    }
    if (remaining != 0) {
        (void) face_load(&fonts->sans_bold, budget, sans_bold_path, remaining);
        if (fonts->sans_bold.loaded) remaining -= fonts->sans_bold.data_length;
    }
    if (remaining != 0) {
        (void) face_load(&fonts->serif_bold, budget, serif_bold_path, remaining);
        if (fonts->serif_bold.loaded) remaining -= fonts->serif_bold.data_length;
    }
    if (remaining != 0) {
        (void) face_load(&fonts->metric_sans, budget, metric_sans_path,
                         remaining);
        if (fonts->metric_sans.loaded) {
            remaining -= fonts->metric_sans.data_length;
        }
    }
    if (remaining != 0) {
        (void) face_load(&fonts->metric_sans_bold, budget,
                         metric_sans_bold_path, remaining);
    }
    if (!sans && !serif && !fonts->sans_italic.loaded
        && !fonts->sans_bold.loaded && !fonts->serif_bold.loaded
        && !fonts->metric_sans.loaded
        && !fonts->metric_sans_bold.loaded) return false;
    return true;
}

void font_face_destroy(FontFace *face)
{
    if (face == NULL) return;
    if (face->budget != NULL) {
        if (face->backend == FONT_FACE_BACKEND_FREETYPE_WEB) {
#ifdef TILEFINCH_HAVE_FREETYPE
            freetype_web_destroy((FreeTypeWebFace *) face->implementation);
#endif
        } else {
            budget_free(face->budget, face->implementation);
        }
        budget_free(face->budget, face->data);
    }
    memset(face, 0, sizeof(*face));
}

void font_set_destroy(FontSet *fonts)
{
    if (fonts == NULL) return;
    font_face_destroy(&fonts->metric_sans_bold);
    font_face_destroy(&fonts->metric_sans);
    font_face_destroy(&fonts->serif_bold);
    font_face_destroy(&fonts->sans_bold);
    font_face_destroy(&fonts->sans_italic);
    font_face_destroy(&fonts->serif);
    font_face_destroy(&fonts->sans);
}

void web_font_set_destroy(WebFontSet *fonts)
{
    if (fonts == NULL) return;
    for (size_t i = 0; i < TILEFINCH_WEB_FONT_FAMILY_LIMIT; i++) {
        font_face_destroy(&fonts->families[i].bold);
        font_face_destroy(&fonts->families[i].regular);
    }
    memset(fonts, 0, sizeof(*fonts));
}

bool font_family_web(unsigned slot, FontFamily fallback, FontFamily *family)
{
    if (family == NULL || slot >= TILEFINCH_WEB_FONT_FAMILY_LIMIT
        || fallback > FONT_HUMANIST_SANS) return false;
    *family = (FontFamily) (TILEFINCH_WEB_FONT_BASE
               + slot * TILEFINCH_WEB_FONT_FAMILY_STRIDE + (unsigned) fallback);
    return true;
}

bool font_family_is_web(FontFamily family)
{
    unsigned value = (unsigned) family;
    return value >= TILEFINCH_WEB_FONT_BASE
        && value < TILEFINCH_WEB_FONT_BASE
                   + TILEFINCH_WEB_FONT_FAMILY_LIMIT
                     * TILEFINCH_WEB_FONT_FAMILY_STRIDE;
}

unsigned font_family_web_slot(FontFamily family)
{
    return font_family_is_web(family)
        ? ((unsigned) family - TILEFINCH_WEB_FONT_BASE)
            / TILEFINCH_WEB_FONT_FAMILY_STRIDE
        : TILEFINCH_WEB_FONT_FAMILY_LIMIT;
}

FontFamily font_family_web_fallback(FontFamily family)
{
    return font_family_is_web(family)
        ? (FontFamily) (((unsigned) family - TILEFINCH_WEB_FONT_BASE)
                         % TILEFINCH_WEB_FONT_FAMILY_STRIDE)
        : family;
}

FontFamily font_context_metric_family(
    const WebFontSet *web_fonts, FontFamily family, const FontFace *face)
{
    if (!font_family_is_web(family)) return family;
    unsigned slot = font_family_web_slot(family);
    if (web_fonts != NULL && slot < TILEFINCH_WEB_FONT_FAMILY_LIMIT) {
        const WebFontFamilyFaces *faces = &web_fonts->families[slot];
        if (face == &faces->regular || face == &faces->bold) {
            return FONT_SANS;
        }
    }
    return font_family_web_fallback(family);
}

const FontFace *font_context_face_variant(
    const FontSet *fonts, const WebFontSet *web_fonts, FontFamily family,
    bool italic, bool bold)
{
    if (font_family_is_web(family)) {
        unsigned slot = font_family_web_slot(family);
        if (web_fonts != NULL && slot < TILEFINCH_WEB_FONT_FAMILY_LIMIT) {
            const WebFontFamilyFaces *faces = &web_fonts->families[slot];
            if (bold && faces->bold.loaded) return &faces->bold;
            if (faces->regular.loaded) return &faces->regular;
            if (faces->bold.loaded) return &faces->bold;
        }
        family = font_family_web_fallback(family);
    }
    return font_set_face_variant(fonts, family, italic, bold);
}

const FontFace *font_context_face(
    const FontSet *fonts, const WebFontSet *web_fonts, FontFamily family)
{
    return font_context_face_variant(fonts, web_fonts, family, false, false);
}

bool font_context_face_is_bold(
    const FontSet *fonts, const WebFontSet *web_fonts,
    const FontFace *face)
{
    if (face == NULL) return false;
    if (web_fonts != NULL) {
        for (size_t i = 0; i < TILEFINCH_WEB_FONT_FAMILY_LIMIT; i++) {
            if (face == &web_fonts->families[i].bold) return true;
        }
    }
    return font_set_face_is_bold(fonts, face);
}

const FontFace *font_set_face(const FontSet *fonts, FontFamily family)
{
    if (fonts == NULL) return NULL;
    if (family == FONT_METRIC_SANS && fonts->metric_sans.loaded) {
        return &fonts->metric_sans;
    }
    if (family == FONT_SERIF && fonts->serif.loaded) return &fonts->serif;
    if (fonts->sans.loaded) return &fonts->sans;
    return fonts->serif.loaded ? &fonts->serif : NULL;
}

const FontFace *font_set_face_style(const FontSet *fonts, FontFamily family,
                                    bool italic)
{
    return font_set_face_variant(fonts, family, italic, false);
}

const FontFace *font_set_face_variant(const FontSet *fonts, FontFamily family,
                                      bool italic, bool bold)
{
    if (fonts != NULL && bold && !italic) {
        if (family == FONT_METRIC_SANS && fonts->metric_sans_bold.loaded) {
            return &fonts->metric_sans_bold;
        }
        if (family == FONT_SERIF && fonts->serif_bold.loaded) {
            return &fonts->serif_bold;
        }
        if (family != FONT_SERIF && fonts->sans_bold.loaded) {
            return &fonts->sans_bold;
        }
    }
    if (fonts != NULL
        && (family == FONT_SANS || family == FONT_HUMANIST_SANS) && italic
        && fonts->sans_italic.loaded) return &fonts->sans_italic;
    return font_set_face(fonts, family);
}

bool font_set_face_is_bold(const FontSet *fonts, const FontFace *face)
{
    return fonts != NULL && face != NULL
           && (face == &fonts->sans_bold || face == &fonts->serif_bold
               || face == &fonts->metric_sans_bold);
}

size_t font_utf8_next(const char *text, size_t remaining,
                      unsigned *codepoint)
{
    if (text == NULL || remaining == 0 || codepoint == NULL) return 0;
    const unsigned char *bytes = (const unsigned char *) text;
    unsigned value = bytes[0];
    size_t count = 1;
    if ((bytes[0] & 0xe0u) == 0xc0u) { value = bytes[0] & 0x1fu; count = 2; }
    else if ((bytes[0] & 0xf0u) == 0xe0u) { value = bytes[0] & 0x0fu; count = 3; }
    else if ((bytes[0] & 0xf8u) == 0xf0u) { value = bytes[0] & 0x07u; count = 4; }
    if (count > remaining) { *codepoint = 0xfffdu; return 1; }
    for (size_t i = 1; i < count; i++) {
        if ((bytes[i] & 0xc0u) != 0x80u) {
            *codepoint = 0xfffdu;
            return 1;
        }
        value = (value << 6) | (bytes[i] & 0x3fu);
    }
    if ((count == 2 && value < 0x80u) || (count == 3 && value < 0x800u)
        || (count == 4 && value < 0x10000u) || value > 0x10ffffu
        || (value >= 0xd800u && value <= 0xdfffu)) {
        *codepoint = 0xfffdu;
        return 1;
    }
    *codepoint = value;
    return count;
}

static const stbtt_fontinfo *font_info(const FontFace *face)
{
    return face != NULL && face->loaded
           && face->backend == FONT_FACE_BACKEND_STB_TRUSTED
           ? (const stbtt_fontinfo *) face->implementation : NULL;
}

#ifdef TILEFINCH_HAVE_FREETYPE
static FreeTypeWebFace *freetype_info(const FontFace *face)
{
    return face != NULL && face->loaded
           && face->backend == FONT_FACE_BACKEND_FREETYPE_WEB
           ? (FreeTypeWebFace *) face->implementation : NULL;
}

static bool freetype_set_pixel_height_fixed(FreeTypeWebFace *state,
                                            int pixel_height_fixed)
{
    if (state == NULL || state->face == NULL || pixel_height_fixed <= 0) {
        return false;
    }
    return FT_Set_Char_Size(state->face, 0, (FT_F26Dot6) pixel_height_fixed,
                            72, 72) == 0;
}

static FT_Int32 freetype_outline_load_flags(void)
{
    return FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_BITMAP
           | FT_LOAD_NO_SVG;
}

static bool freetype_outline_is_axis_aligned_rectangle(
    const FT_GlyphSlot slot)
{
    if (slot == NULL || slot->format != FT_GLYPH_FORMAT_OUTLINE
        || slot->outline.n_contours != 1 || slot->outline.n_points != 4) {
        return false;
    }
    FT_Pos minimum_x = slot->outline.points[0].x;
    FT_Pos maximum_x = minimum_x;
    FT_Pos minimum_y = slot->outline.points[0].y;
    FT_Pos maximum_y = minimum_y;
    for (short i = 0; i < 4; i++) {
        if (FT_CURVE_TAG(slot->outline.tags[i]) != FT_CURVE_TAG_ON) {
            return false;
        }
        FT_Pos x = slot->outline.points[i].x;
        FT_Pos y = slot->outline.points[i].y;
        if (x < minimum_x) minimum_x = x;
        if (x > maximum_x) maximum_x = x;
        if (y < minimum_y) minimum_y = y;
        if (y > maximum_y) maximum_y = y;
    }
    if (minimum_x == maximum_x || minimum_y == maximum_y) return false;
    for (short i = 0; i < 4; i++) {
        FT_Pos x = slot->outline.points[i].x;
        FT_Pos y = slot->outline.points[i].y;
        if ((x != minimum_x && x != maximum_x)
            || (y != minimum_y && y != maximum_y)) return false;
    }
    return true;
}

static int fixed_26_6_to_int(FT_Pos value)
{
    if ((int64_t) value > INT_MAX) return INT_MAX;
    if ((int64_t) value < INT_MIN) return INT_MIN;
    return (int) value;
}
#endif

static int bounded_pixel_height_fixed(int pixel_height_fixed)
{
    return pixel_height_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64
           ? TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64 : pixel_height_fixed;
}

static int integer_pixel_height_to_fixed(int pixel_height)
{
    if (pixel_height <= 0) return pixel_height;
    return pixel_height >= TILEFINCH_FONT_RASTER_PIXEL_LIMIT
           ? TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64 : pixel_height * 64;
}

/*
 * The PSP application fonts are deliberately Latin subsets. Loading a CJK
 * fallback face merely to show a Korean video title would retain several
 * megabytes and compete directly with the offline voice model. Modern Hangul
 * syllables are compositional, so provide a tiny last-resort raster fallback:
 * decompose each precomposed syllable into its initial, vowel, and optional
 * final jamo and draw those strokes into one em square. This is intentionally
 * a fallback, not a replacement for an authored web font.
 */
static bool builtin_hangul_syllable(unsigned codepoint)
{
    return codepoint >= 0xac00u && codepoint <= 0xd7a3u;
}

static void hangul_plot(unsigned char *pixels, int side, int x, int y,
                        int thickness)
{
    for (int yy = y; yy < y + thickness; yy++) {
        for (int xx = x; xx < x + thickness; xx++) {
            if (xx >= 0 && yy >= 0 && xx < side && yy < side)
                pixels[(size_t) yy * (size_t) side + (size_t) xx] = 255;
        }
    }
}

static void hangul_line(unsigned char *pixels, int side,
                        int x0, int y0, int x1, int y1, int thickness)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        hangul_plot(pixels, side, x0, y0, thickness);
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void hangul_box(unsigned char *pixels, int side,
                       int x, int y, int width, int height, int thickness)
{
    if (width <= 0 || height <= 0) return;
    int right = x + width - 1;
    int bottom = y + height - 1;
    hangul_line(pixels, side, x, y, right, y, thickness);
    hangul_line(pixels, side, x, y, x, bottom, thickness);
    hangul_line(pixels, side, right, y, right, bottom, thickness);
    hangul_line(pixels, side, x, bottom, right, bottom, thickness);
}

static void hangul_consonant(unsigned char *pixels, int side, unsigned jamo,
                             int x, int y, int width, int height, int thick)
{
    if (width < 3 || height < 3) return;
    int r = x + width - 1, b = y + height - 1;
    int mx = x + width / 2, my = y + height / 2;
#define HLINE(ax, ay, bx, by) \
    hangul_line(pixels, side, (ax), (ay), (bx), (by), thick)
    switch (jamo) {
        case 0: /* giyeok */
            HLINE(x, y, r, y); HLINE(r, y, r, b); break;
        case 1: /* ssanggiyeok */
            hangul_consonant(pixels, side, 0, x, y,
                             width / 2, height, thick);
            hangul_consonant(pixels, side, 0, mx, y,
                             width - width / 2, height, thick);
            break;
        case 2: /* nieun */
            HLINE(x, y, x, b); HLINE(x, b, r, b); break;
        case 3: /* digeut */
            hangul_box(pixels, side, x, y, width, height, thick); break;
        case 4: /* ssangdigeut */
            hangul_box(pixels, side, x, y, width / 2, height, thick);
            hangul_box(pixels, side, mx, y,
                       width - width / 2, height, thick); break;
        case 5: /* rieul */
            HLINE(x, y, r, y); HLINE(r, y, r, my);
            HLINE(x, my, r, my); HLINE(x, my, x, b);
            HLINE(x, b, r, b); break;
        case 6: /* mieum */
            hangul_box(pixels, side, x, y, width, height, thick); break;
        case 7: /* bieup */
            hangul_box(pixels, side, x, y, width, height, thick);
            HLINE(x, my, r, my); break;
        case 8: /* ssangbieup */
            hangul_consonant(pixels, side, 7, x, y,
                             width / 2, height, thick);
            hangul_consonant(pixels, side, 7, mx, y,
                             width - width / 2, height, thick); break;
        case 9: /* siot */
            HLINE(x, b, mx, y); HLINE(mx, y, r, b); break;
        case 10: /* ssangsiot */
            hangul_consonant(pixels, side, 9, x, y,
                             width / 2, height, thick);
            hangul_consonant(pixels, side, 9, mx, y,
                             width - width / 2, height, thick); break;
        case 11: /* ieung */
            hangul_box(pixels, side, x, y, width, height, thick);
            hangul_plot(pixels, side, x, y, thick);
            hangul_plot(pixels, side, r, y, thick);
            hangul_plot(pixels, side, x, b, thick);
            hangul_plot(pixels, side, r, b, thick);
            break;
        case 12: /* jieut */
            HLINE(x, y, r, y);
            HLINE(x, b, mx, y + thick); HLINE(mx, y + thick, r, b); break;
        case 13: /* ssangjieut */
            hangul_consonant(pixels, side, 12, x, y,
                             width / 2, height, thick);
            hangul_consonant(pixels, side, 12, mx, y,
                             width - width / 2, height, thick); break;
        case 14: /* chieut */
            HLINE(x + width / 4, y, r - width / 4, y);
            HLINE(x, y + thick + 1, r, y + thick + 1);
            HLINE(x, b, mx, y + 2 * thick);
            HLINE(mx, y + 2 * thick, r, b); break;
        case 15: /* kieuk */
            HLINE(x, y, r, y); HLINE(r, y, r, b);
            HLINE(x, my, r, my); break;
        case 16: /* tieut */
            hangul_box(pixels, side, x, y, width, height, thick);
            HLINE(x, my, r, my); break;
        case 17: /* pieup */
            HLINE(x, y, r, y); HLINE(x, b, r, b);
            HLINE(x, my, r, my); HLINE(x, y, x, b);
            HLINE(r, y, r, b); break;
        case 18: /* hieuh */
            HLINE(x + width / 4, y, r - width / 4, y);
            HLINE(x, y + thick + 1, r, y + thick + 1);
            if (height > 2 * thick + 2)
                hangul_box(pixels, side, x + 1, y + 2 * thick + 1,
                           width - 2, height - 2 * thick - 1, thick);
            break;
        default:
            hangul_box(pixels, side, x, y, width, height, thick); break;
    }
#undef HLINE
}

static void hangul_vowel(unsigned char *pixels, int side, unsigned vowel,
                         int x, int y, int width, int height, int thick)
{
    int r = x + width - 1, b = y + height - 1;
    int mx = x + width / 2, my = y + height / 2;
#define VLINE(ax, ay, bx, by) \
    hangul_line(pixels, side, (ax), (ay), (bx), (by), thick)
    switch (vowel) {
        case 0: VLINE(mx, y, mx, b); VLINE(mx, my, r, my); break; /* a */
        case 1: VLINE(x, y, x, b); VLINE(x, my, mx, my);
                VLINE(r, y, r, b); break; /* ae */
        case 2: VLINE(mx, y, mx, b); VLINE(mx, my - 2, r, my - 2);
                VLINE(mx, my + 2, r, my + 2); break; /* ya */
        case 3: VLINE(x, y, x, b); VLINE(x, my - 2, mx, my - 2);
                VLINE(x, my + 2, mx, my + 2); VLINE(r, y, r, b); break;
        case 4: VLINE(x, my, mx, my); VLINE(mx, y, mx, b); break; /* eo */
        case 5: VLINE(x, y, x, b); VLINE(x, my, mx, my);
                VLINE(r, y, r, b); break; /* e */
        case 6: VLINE(x, my - 2, mx, my - 2);
                VLINE(x, my + 2, mx, my + 2); VLINE(mx, y, mx, b); break;
        case 7: VLINE(x, y, x, b); VLINE(x, my - 2, mx, my - 2);
                VLINE(x, my + 2, mx, my + 2); VLINE(r, y, r, b); break;
        case 8: VLINE(x, b, r, b); VLINE(mx, y, mx, b); break; /* o */
        case 9: VLINE(x, b, r, b); VLINE(mx, y, mx, b);
                VLINE(r, y, r, b); VLINE(r, my, r + 1, my); break;
        case 10: VLINE(x, b, r, b); VLINE(mx, y, mx, b);
                 VLINE(r - 1, y, r - 1, b); VLINE(r, y, r, b); break;
        case 11: VLINE(x, b, r, b); VLINE(mx, y, mx, b);
                 VLINE(r, y, r, b); break;
        case 12: VLINE(x, b, r, b); VLINE(mx - 2, y, mx - 2, b);
                 VLINE(mx + 2, y, mx + 2, b); break; /* yo */
        case 13: VLINE(x, y, r, y); VLINE(mx, y, mx, b); break; /* u */
        case 14: VLINE(x, y, r, y); VLINE(mx, y, mx, b);
                 VLINE(r, y, r, b); VLINE(mx, my, r, my); break;
        case 15: VLINE(x, y, r, y); VLINE(mx, y, mx, b);
                 VLINE(r - 1, y, r - 1, b); VLINE(r, y, r, b); break;
        case 16: VLINE(x, y, r, y); VLINE(mx, y, mx, b);
                 VLINE(r, y, r, b); break;
        case 17: VLINE(x, y, r, y); VLINE(mx - 2, y, mx - 2, b);
                 VLINE(mx + 2, y, mx + 2, b); break; /* yu */
        case 18: VLINE(x, my, r, my); break; /* eu */
        case 19: VLINE(x, my, r, my); VLINE(r, y, r, b); break; /* ui */
        case 20: VLINE(mx, y, mx, b); break; /* i */
        default: VLINE(mx, y, mx, b); break;
    }
#undef VLINE
}

static void hangul_final(unsigned char *pixels, int side, unsigned final,
                         int x, int y, int width, int height, int thick)
{
    static const int8_t single[28] = {
        -1, 0, 1, -2, 2, -2, -2, 3, 5, -2, -2, -2, -2, -2,
        -2, -2, 6, 7, -2, 9, 10, 11, 12, 14, 15, 16, 17, 18
    };
    static const uint8_t pair[28][2] = {
        {0,0},{0,0},{0,0},{0,9},{0,0},{2,12},{2,18},{0,0},
        {0,0},{5,0},{5,6},{5,7},{5,9},{5,16},{5,17},{5,18},
        {0,0},{0,0},{7,9},{0,0},{0,0},{0,0},{0,0},{0,0},
        {0,0},{0,0},{0,0},{0,0}
    };
    if (final >= 28u || final == 0) return;
    if (single[final] >= 0) {
        hangul_consonant(
            pixels, side, (unsigned) single[final],
            x, y, width, height, thick);
    } else {
        int half = width / 2;
        hangul_consonant(
            pixels, side, pair[final][0], x, y, half, height, thick);
        hangul_consonant(
            pixels, side, pair[final][1], x + half, y,
            width - half, height, thick);
    }
}

static bool builtin_hangul_glyph(const FontFace *face, unsigned codepoint,
                                 int pixel_height_fixed, bool bold,
                                 FontGlyph *glyph)
{
    if (!builtin_hangul_syllable(codepoint) || face == NULL
        || glyph == NULL) return false;
    int side = (pixel_height_fixed + 63) / 64;
    if (side < 8) side = 8;
    if (side > TILEFINCH_FONT_RASTER_PIXEL_LIMIT)
        side = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
    size_t bytes = (size_t) side * (size_t) side;
    unsigned char *pixels = budget_calloc(face->budget, bytes, 1);
    if (pixels == NULL) return false;

    unsigned syllable = codepoint - 0xac00u;
    unsigned initial = syllable / (21u * 28u);
    unsigned vowel = (syllable / 28u) % 21u;
    unsigned final = syllable % 28u;
    int thick = side >= 24 ? 2 : 1;
    if (bold && thick < 3) thick++;
    int margin = side >= 12 ? 1 : 0;
    int inner = side - margin * 2;
    int final_height = final == 0 ? 0 : (inner + 3) / 4;
    int body_height = inner - final_height;
    bool horizontal = vowel == 8u || vowel == 12u || vowel == 13u
        || vowel == 17u || vowel == 18u;
    if (horizontal) {
        int initial_height = (body_height * 3) / 5;
        hangul_consonant(
            pixels, side, initial, margin, margin,
            inner, initial_height, thick);
        hangul_vowel(
            pixels, side, vowel, margin,
            margin + initial_height, inner,
            body_height - initial_height, thick);
    } else {
        int initial_width = (inner * 3) / 5;
        hangul_consonant(
            pixels, side, initial, margin, margin,
            initial_width, body_height, thick);
        hangul_vowel(
            pixels, side, vowel, margin + initial_width, margin,
            inner - initial_width, body_height, thick);
    }
    if (final != 0) {
        hangul_final(
            pixels, side, final, margin, margin + body_height,
            inner, final_height, thick);
    }
    glyph->pixels = pixels;
    glyph->budget = face->budget;
    glyph->width = side;
    glyph->height = side;
    glyph->x_offset = 0;
    glyph->y_offset = -(side * 7) / 8;
    glyph->advance_fixed = pixel_height_fixed + (bold ? 22 : 0);
    glyph->advance = (glyph->advance_fixed + 32) / 64;
    return true;
}

typedef struct {
    const unsigned char *compressed;
    const uint32_t *block_offsets;
    size_t glyph_count;
    size_t glyph_index;
    unsigned source_width;
} BuiltinBitmap;

static bool builtin_bitmap_table_lookup(
    unsigned codepoint, const void *codepoints, bool codepoints_are_16_bit,
    size_t count,
    const unsigned char *narrow_bits, const uint32_t *block_offsets,
    const unsigned char *compressed,
    BuiltinBitmap *result)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        unsigned candidate = codepoints_are_16_bit
            ? ((const uint16_t *) codepoints)[middle]
            : ((const uint32_t *) codepoints)[middle];
        if (candidate < codepoint) low = middle + 1u;
        else high = middle;
    }
    if (low >= count) return false;
    unsigned found = codepoints_are_16_bit
        ? ((const uint16_t *) codepoints)[low]
        : ((const uint32_t *) codepoints)[low];
    if (found != codepoint) return false;
    if (result != NULL) {
        result->compressed = compressed;
        result->block_offsets = block_offsets;
        result->glyph_count = count;
        result->glyph_index = low;
        result->source_width =
            (narrow_bits[low / 8u] & (1u << (low & 7u))) != 0 ? 8u : 16u;
    }
    return true;
}

static bool builtin_bitmap_lookup(unsigned codepoint, BuiltinBitmap *result)
{
    if (codepoint <= UINT16_MAX) {
        if (builtin_bitmap_table_lookup(
                codepoint, builtin_cjk_codepoints, true, BUILTIN_CJK_COUNT,
                builtin_cjk_narrow_bits, builtin_cjk_block_offsets,
                builtin_cjk_compressed, result)) {
            return true;
        }
    }
    return builtin_bitmap_table_lookup(
        codepoint, builtin_emoji_codepoints, false, BUILTIN_EMOJI_COUNT,
        builtin_emoji_narrow_bits, builtin_emoji_block_offsets,
        builtin_emoji_compressed, result);
}

static TilefinchGlyphProvider *optional_glyph_provider;

bool font_optional_glyph_provider_install(TilefinchGlyphProvider *provider)
{
    if (provider == NULL || (optional_glyph_provider != NULL
        && optional_glyph_provider != provider)) return false;
    optional_glyph_provider = provider;
    return true;
}

bool font_optional_glyph_provider_uninstall(TilefinchGlyphProvider *provider)
{
    if (provider == NULL || optional_glyph_provider != provider) return false;
    optional_glyph_provider = NULL;
    return true;
}

static bool optional_fallback_key(unsigned codepoint, uint32_t *key,
                                  unsigned *width, unsigned *height)
{
    uint32_t found = 0;
    if (optional_glyph_provider == NULL
        || !tilefinch_glyph_provider_has_codepoint(
            optional_glyph_provider, codepoint, &found, NULL)
        || !tilefinch_glyph_provider_metrics(
            optional_glyph_provider, found, width, height, NULL)) return false;
    if (key != NULL) *key = found;
    return true;
}

bool font_optional_glyph_match_sequence(
    const char *text, size_t length, size_t *used, unsigned *glyph_key)
{
    if (optional_glyph_provider == NULL || text == NULL || length == 0
        || used == NULL || glyph_key == NULL) return false;
    unsigned first = 0;
    size_t first_bytes = font_utf8_next(text, length, &first);
    size_t matched = 0;
    uint32_t key = 0;
    if (first_bytes == 0
        || !tilefinch_glyph_provider_match(
            optional_glyph_provider, text, length, &matched, &key, NULL)
        || matched <= first_bytes) return false;
    *used = matched;
    *glyph_key = key;
    return true;
}

bool font_optional_glyph_match_sequence_ending_at(
    const char *text, size_t end, size_t *start, unsigned *glyph_key)
{
    if (text == NULL || end == 0 || start == NULL || glyph_key == NULL)
        return false;
    size_t candidates[TILEFINCH_GLYPH_COMPONENT_SEQUENCE_CODEPOINT_LIMIT];
    size_t count = 0;
    size_t cursor = end;
    while (cursor > 0 && count < sizeof(candidates) / sizeof(candidates[0])) {
        cursor--;
        while (cursor > 0
               && ((unsigned char) text[cursor] & 0xc0u) == 0x80u) cursor--;
        candidates[count++] = cursor;
    }
    /* Try the longest possible suffix first. The provider's own bounded
       matcher then selects the longest catalog entry at that start. */
    while (count > 0) {
        size_t candidate = candidates[--count];
        size_t used = 0;
        unsigned key = 0;
        if (font_optional_glyph_match_sequence(
                text + candidate, end - candidate, &used, &key)
            && used == end - candidate) {
            *start = candidate;
            *glyph_key = key;
            return true;
        }
    }
    return false;
}

static bool optional_fallback_metrics(unsigned value, uint32_t *key,
                                      unsigned *width, unsigned *height)
{
    if (value <= 0x10ffffu)
        return optional_fallback_key(value, key, width, height);
    if (optional_glyph_provider == NULL
        || !tilefinch_glyph_provider_metrics(
            optional_glyph_provider, value, width, height, NULL)) return false;
    if (key != NULL) *key = value;
    return true;
}

typedef struct {
    const unsigned char *source;
    size_t block;
    size_t bytes;
    bool valid;
    unsigned char decoded[BUILTIN_BITMAP_BLOCK_GLYPHS * 32u];
} BuiltinBitmapCache;

/*
 * Font rasterization is owned by the main rendering thread. Four direct-map
 * slots retain 8 KiB total and bound every miss. This prevents common
 * alternating Kana/CJK blocks from repeatedly expanding one another after
 * the larger budget-owned raster cache is reclaimed.
 */
#define BUILTIN_BITMAP_CACHE_SLOTS 4u
_Static_assert(
    (BUILTIN_BITMAP_CACHE_SLOTS
     & (BUILTIN_BITMAP_CACHE_SLOTS - 1u)) == 0,
    "builtin bitmap cache slot count must be a power of two");
static BuiltinBitmapCache
    builtin_bitmap_cache[BUILTIN_BITMAP_CACHE_SLOTS];

static BuiltinBitmapCache *builtin_bitmap_decode_block(
    const BuiltinBitmap *bitmap)
{
    if (bitmap == NULL || bitmap->compressed == NULL
        || bitmap->block_offsets == NULL) return NULL;
    size_t block = bitmap->glyph_index / BUILTIN_BITMAP_BLOCK_GLYPHS;
    size_t slot = (block ^ ((uintptr_t) bitmap->compressed >> 4u))
        & (BUILTIN_BITMAP_CACHE_SLOTS - 1u);
    BuiltinBitmapCache *cache = &builtin_bitmap_cache[slot];
    if (cache->valid && cache->source == bitmap->compressed
        && cache->block == block) return cache;
    size_t first_glyph = block * BUILTIN_BITMAP_BLOCK_GLYPHS;
    if (first_glyph >= bitmap->glyph_count) return NULL;
    size_t glyphs = bitmap->glyph_count - first_glyph;
    if (glyphs > BUILTIN_BITMAP_BLOCK_GLYPHS)
        glyphs = BUILTIN_BITMAP_BLOCK_GLYPHS;
    size_t expected = glyphs * 32u;
    uint32_t source_at = bitmap->block_offsets[block];
    uint32_t source_end = bitmap->block_offsets[block + 1u];
    size_t output_at = 0;
    while (output_at < expected && source_at < source_end) {
        unsigned flags = bitmap->compressed[source_at++];
        for (unsigned bit = 0; bit < 8u && output_at < expected; bit++) {
            if ((flags & (1u << bit)) != 0) {
                if (source_at >= source_end) return NULL;
                cache->decoded[output_at++] =
                    bitmap->compressed[source_at++];
                continue;
            }
            if (source_end - source_at < 2u) return NULL;
            unsigned encoded =
                (unsigned) bitmap->compressed[source_at] << 8
                | bitmap->compressed[source_at + 1u];
            source_at += 2u;
            size_t distance = encoded & 0xfffu;
            size_t length = (encoded >> 12) + 3u;
            if (distance == 0 || distance > output_at
                || length > expected - output_at) return NULL;
            for (size_t copied = 0; copied < length; copied++) {
                cache->decoded[output_at] =
                    cache->decoded[output_at - distance];
                output_at++;
            }
        }
    }
    if (output_at != expected) return NULL;
    cache->source = bitmap->compressed;
    cache->block = block;
    cache->bytes = expected;
    cache->valid = true;
    return cache;
}

static bool builtin_bitmap_rows(const BuiltinBitmap *bitmap,
                                uint16_t rows[16])
{
    if (bitmap == NULL || rows == NULL) return false;
    BuiltinBitmapCache *cache = builtin_bitmap_decode_block(bitmap);
    if (cache == NULL) return false;
    size_t local = bitmap->glyph_index % BUILTIN_BITMAP_BLOCK_GLYPHS;
    size_t offset = local * 32u;
    if (offset > cache->bytes || cache->bytes - offset < 32u) return false;
    for (size_t row = 0; row < 16u; row++) {
        rows[row] =
            (uint16_t) cache->decoded[offset + row * 2u] << 8
            | cache->decoded[offset + row * 2u + 1u];
    }
    return true;
}

bool font_codepoint_default_ignorable(unsigned codepoint)
{
    return codepoint == 0x00adu || codepoint == 0x034fu
        || codepoint == 0x061cu || codepoint == 0xfeffu
        || (codepoint >= 0x180bu && codepoint <= 0x180fu)
        || (codepoint >= 0x200bu && codepoint <= 0x200fu)
        || (codepoint >= 0x202au && codepoint <= 0x202eu)
        || (codepoint >= 0x2060u && codepoint <= 0x206fu)
        || (codepoint >= 0xfe00u && codepoint <= 0xfe0fu)
        || (codepoint >= 0x1bca0u && codepoint <= 0x1bca3u)
        || (codepoint >= 0x1d173u && codepoint <= 0x1d17au)
        || (codepoint >= 0xe0100u && codepoint <= 0xe01efu);
}

static bool builtin_fallback_supported(unsigned codepoint)
{
    return font_codepoint_default_ignorable(codepoint)
        || builtin_hangul_syllable(codepoint)
        /* A tiny procedural fallback for the navigation/search subset used
           by common web icon fonts. It keeps critical mobile controls
           legible while their optional WOFF is still loading. */
        || codepoint == 0xe0d5u || codepoint == 0xe700u
        || codepoint == 0xe70du || codepoint == 0xe721u
        || builtin_bitmap_lookup(codepoint, NULL);
}

static bool builtin_navigation_icon(unsigned codepoint)
{
    return codepoint == 0xe0d5u || codepoint == 0xe700u
        || codepoint == 0xe70du || codepoint == 0xe721u;
}

static bool optional_fallback_advance_fixed(unsigned codepoint,
                                            int pixel_height_fixed,
                                            bool bold, int *advance)
{
    unsigned width = 0, height = 0;
    if (advance == NULL
        || !optional_fallback_metrics(codepoint, NULL, &width, &height)
        || height == 0) return false;
    int64_t scaled = (int64_t) pixel_height_fixed * width;
    scaled = (scaled + (int64_t) height / 2) / height;
    scaled += bold ? 22 : 0;
    *advance = scaled > INT_MAX ? INT_MAX : (int) scaled;
    return true;
}

static bool optional_sequence_advance_fixed(
    const char *text, size_t length, int pixel_height_fixed, bool bold,
    size_t *used, int *advance)
{
    unsigned key = 0;
    return font_optional_glyph_match_sequence(
               text, length, used, &key)
        && optional_fallback_advance_fixed(
               key, pixel_height_fixed, bold, advance);
}

/*
 * Whether this face can actually serve the codepoint, as opposed to being
 * willing to hand back something for it.
 *
 * font_glyph_load() reports success for a codepoint the face does not have:
 * both backends fall through to glyph index 0, and .notdef is a legal glyph.
 * DejaVuSans-Latin's is empty, so a missing character came back as a blank
 * cell with a real advance; DejaVuSans-Bold-Latin's is a box, so the same
 * character came back as tofu. Neither is distinguishable from a hit by the
 * return value, which is why the chrome's "known_missing" flag never fired on
 * a device and its designed substitutions never ran once fonts were loaded.
 * The cmap lookup is the only exact answer, so callers that need one ask
 * here.
 */
bool font_face_has_codepoint(const FontFace *face, unsigned codepoint)
{
    if (face == NULL) return false;
    if (builtin_fallback_supported(codepoint)
        || optional_fallback_key(codepoint, NULL, NULL, NULL)) return true;
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        return freetype->face != NULL
            && FT_Get_Char_Index(freetype->face, codepoint) != 0;
    }
#endif
    const stbtt_fontinfo *info = font_info(face);
    return info != NULL
        && stbtt_FindGlyphIndex(info, (int) codepoint) != 0;
}

static bool builtin_fallback_advance_fixed(unsigned codepoint,
                                           int pixel_height_fixed,
                                           bool bold, int *advance)
{
    if (advance == NULL) return false;
    if (font_codepoint_default_ignorable(codepoint)) {
        *advance = 0;
        return true;
    }
    if (builtin_hangul_syllable(codepoint)) {
        *advance = pixel_height_fixed + (bold ? 22 : 0);
        return true;
    }
    if (builtin_navigation_icon(codepoint)) {
        *advance = pixel_height_fixed + (bold ? 22 : 0);
        return true;
    }
    BuiltinBitmap bitmap;
    if (!builtin_bitmap_lookup(codepoint, &bitmap)) return false;
    *advance = (int) (((int64_t) pixel_height_fixed
                       * (int64_t) bitmap.source_width + 8) / 16)
        + (bold ? (bitmap.source_width == 8u ? 11 : 22) : 0);
    return true;
}

static bool optional_fallback_glyph(
    const FontFace *face, unsigned codepoint, int pixel_height_fixed,
    bool bold, FontGlyph *glyph, bool *pending)
{
    if (pending != NULL) *pending = false;
    uint32_t key = 0;
    unsigned source_width = 0, source_height = 0;
    if (face == NULL || glyph == NULL
        || !optional_fallback_metrics(
            codepoint, &key, &source_width, &source_height)) return false;
    TilefinchGlyphSource source;
    if (!tilefinch_glyph_provider_source(
            optional_glyph_provider, key, &source)) {
        if (pending != NULL) *pending = true;
        return false;
    }
    int height = (pixel_height_fixed + 63) / 64;
    if (height < 1) height = 1;
    if (height > TILEFINCH_FONT_RASTER_PIXEL_LIMIT)
        height = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
    int width = (int) (((uint64_t) source_width * (unsigned) height
                        + source_height / 2u) / source_height);
    if (width < 1) width = 1;
    if ((size_t) width > SIZE_MAX / (size_t) height) return false;
    size_t pixels_count = (size_t) width * (size_t) height;
    unsigned char *coverage = budget_calloc(face->budget, pixels_count, 1);
    if (coverage == NULL) return false;
    uint16_t *colors = source.kind == TILEFINCH_GLYPH_COMPONENT_COLOR
        ? budget_malloc(face->budget, pixels_count * sizeof(*colors)) : NULL;
    if (source.kind == TILEFINCH_GLYPH_COMPONENT_COLOR && colors == NULL) {
        budget_free(face->budget, coverage);
        return false;
    }
    size_t source_pixels = (size_t) source.width * source.height;
    for (int y = 0; y < height; y++) {
        unsigned source_y = (unsigned) y * source.height / (unsigned) height;
        for (int x = 0; x < width; x++) {
            unsigned source_x = (unsigned) x * source.width / (unsigned) width;
            size_t source_at = (size_t) source_y * source.width + source_x;
            size_t output_at = (size_t) y * (size_t) width + (size_t) x;
            if (source.kind == TILEFINCH_GLYPH_COMPONENT_MONO) {
                const uint8_t *row = source.pixels + source_y * 2u;
                if ((row[source_x >> 3u]
                     & (uint8_t) (0x80u >> (source_x & 7u))) != 0)
                    coverage[output_at] = 255u;
            } else {
                const uint8_t *color = source.pixels + source_at * 2u;
                colors[output_at] = (uint16_t) color[0] << 8 | color[1];
                coverage[output_at] = source.pixels[source_pixels * 2u
                                                     + source_at];
            }
        }
    }
    if (bold && colors == NULL && width > 1) {
        for (int y = 0; y < height; y++) {
            unsigned char *row = coverage + (size_t) y * (size_t) width;
            for (int x = width - 1; x > 0; x--)
                if (row[x] == 0 && row[x - 1] != 0) row[x] = row[x - 1];
        }
    }
    glyph->pixels = coverage;
    glyph->colors = colors;
    glyph->budget = face->budget;
    glyph->width = width;
    glyph->height = height;
    glyph->x_offset = 0;
    glyph->y_offset = -(height * 7) / 8;
    if (!optional_fallback_advance_fixed(
            codepoint, pixel_height_fixed, bold, &glyph->advance_fixed)) {
        budget_free(face->budget, colors);
        budget_free(face->budget, coverage);
        memset(glyph, 0, sizeof(*glyph));
        return false;
    }
    glyph->advance = (glyph->advance_fixed + 32) / 64;
    return true;
}

static bool builtin_bitmap_glyph(const FontFace *face, unsigned codepoint,
                                 int pixel_height_fixed, bool bold,
                                 FontGlyph *glyph)
{
    if (face == NULL || glyph == NULL) return false;
    if (font_codepoint_default_ignorable(codepoint)) {
        glyph->budget = face->budget;
        return true;
    }
    BuiltinBitmap bitmap;
    if (!builtin_bitmap_lookup(codepoint, &bitmap)) return false;
    uint16_t rows[16];
    if (!builtin_bitmap_rows(&bitmap, rows)) return false;
    int height = (pixel_height_fixed + 63) / 64;
    if (height < 1) height = 1;
    if (height > TILEFINCH_FONT_RASTER_PIXEL_LIMIT)
        height = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
    int width = (int) ((bitmap.source_width * (unsigned) height + 8u) / 16u);
    if (width < 1) width = 1;
    size_t bytes = (size_t) width * (size_t) height;
    unsigned char *pixels = budget_calloc(face->budget, bytes, 1);
    if (pixels == NULL) return false;
    for (int y = 0; y < height; y++) {
        unsigned source_y = (unsigned) y * 16u / (unsigned) height;
        for (int x = 0; x < width; x++) {
            unsigned source_x =
                (unsigned) x * bitmap.source_width / (unsigned) width;
            uint16_t source = rows[source_y];
            if ((source & (uint16_t) (UINT16_C(0x8000) >> source_x)) != 0)
                pixels[(size_t) y * (size_t) width + (size_t) x] = 255;
        }
    }
    if (bold && width > 1) {
        for (int y = 0; y < height; y++) {
            unsigned char *row = pixels + (size_t) y * (size_t) width;
            for (int x = width - 1; x > 0; x--) {
                if (row[x] == 0 && row[x - 1] != 0) row[x] = row[x - 1];
            }
        }
    }
    glyph->pixels = pixels;
    glyph->budget = face->budget;
    glyph->width = width;
    glyph->height = height;
    glyph->x_offset = 0;
    glyph->y_offset = -(height * 7) / 8;
    if (!builtin_fallback_advance_fixed(
            codepoint, pixel_height_fixed, bold, &glyph->advance_fixed)) {
        budget_free(face->budget, pixels);
        memset(glyph, 0, sizeof(*glyph));
        return false;
    }
    glyph->advance = (glyph->advance_fixed + 32) / 64;
    return true;
}

static bool builtin_navigation_icon_glyph(
    const FontFace *face, unsigned codepoint, int pixel_height_fixed,
    bool bold, FontGlyph *glyph)
{
    if (face == NULL || glyph == NULL
        || !builtin_navigation_icon(codepoint)) return false;
    int side = (pixel_height_fixed + 63) / 64;
    if (side < 8) side = 8;
    if (side > TILEFINCH_FONT_RASTER_PIXEL_LIMIT)
        side = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
    size_t bytes = (size_t) side * (size_t) side;
    unsigned char *pixels = budget_calloc(face->budget, bytes, 1);
    if (pixels == NULL) return false;
    int thickness = bold ? 2 : 1;
    for (int y = 0; y < side; y++) {
        int sy = y * 16 / side;
        for (int x = 0; x < side; x++) {
            int sx = x * 16 / side;
            bool mark = false;
            if (codepoint == 0xe700u) {
                mark = sx >= 2 && sx <= 13
                    && (abs(sy - 4) <= thickness - 1
                        || abs(sy - 8) <= thickness - 1
                        || abs(sy - 12) <= thickness - 1);
            } else if (codepoint == 0xe721u) {
                int dx = sx - 6;
                int dy = sy - 6;
                int distance = dx * dx + dy * dy;
                mark = (distance >= 12 && distance <= 25)
                    || (sx >= 9 && sy >= 9 && abs(sx - sy) <= thickness);
            } else if (codepoint == 0xe70du) {
                mark = sy >= 5 && sy <= 10
                    && (abs((sx - 3) - (sy - 5)) <= thickness
                        || abs((13 - sx) - (sy - 5)) <= thickness);
            } else {
                mark = sx >= 4 && sx <= 11
                    && (abs((sy - 8) - (sx - 7)) <= thickness
                        || abs((8 - sy) - (sx - 7)) <= thickness);
            }
            if (mark) pixels[(size_t) y * (size_t) side + (size_t) x] = 255;
        }
    }
    glyph->pixels = pixels;
    glyph->budget = face->budget;
    glyph->width = side;
    glyph->height = side;
    glyph->x_offset = 0;
    glyph->y_offset = -(side * 7) / 8;
    glyph->advance_fixed = pixel_height_fixed + (bold ? 22 : 0);
    glyph->advance = (glyph->advance_fixed + 32) / 64;
    return true;
}

static bool builtin_fallback_glyph(const FontFace *face, unsigned codepoint,
                                   int pixel_height_fixed, bool bold,
                                   FontGlyph *glyph)
{
    if (builtin_hangul_syllable(codepoint))
        return builtin_hangul_glyph(
            face, codepoint, pixel_height_fixed, bold, glyph);
    if (builtin_navigation_icon(codepoint))
        return builtin_navigation_icon_glyph(
            face, codepoint, pixel_height_fixed, bold, glyph);
    return builtin_bitmap_glyph(
        face, codepoint, pixel_height_fixed, bold, glyph);
}

/* CSS font-size maps the font's em square to the requested pixel size.  The
   stb pixel-height helper maps ascent minus descent instead, which makes
   common fonts materially smaller than their CSS metrics. */
static float css_font_scale_fixed(const stbtt_fontinfo *info,
                                  int pixel_height_fixed)
{
    return stbtt_ScaleForMappingEmToPixels(
        info, (float) bounded_pixel_height_fixed(pixel_height_fixed) / 64.0f);
}

static int stb_units_per_em(const stbtt_fontinfo *info)
{
    if (info == NULL || info->data == NULL) return 0;
    const unsigned char *value = info->data + info->head + 18;
    return ((int) value[0] << 8) | value[1];
}

static bool font_width_accumulate(int64_t *total, int64_t delta)
{
    if (delta > 0 && *total > INT64_MAX - delta) {
        *total = INT64_MAX;
        return false;
    }
    if (delta < 0 && *total < INT64_MIN - delta) {
        *total = INT64_MIN;
        return false;
    }
    *total += delta;
    return true;
}

static int font_width_round_ratio(int64_t numerator, int64_t denominator)
{
    if (denominator <= 0) return -1;
    /* Text commands are normally short enough that their accumulated fixed
       advance and the font's bounded em denominator both fit in 32 bits.
       Keep that overwhelmingly common path on Allegrex's hardware divider;
       retain the wide path for long unbroken runs and unusual fonts. */
    if (numerator >= INT32_MIN && numerator <= INT32_MAX
        && denominator <= INT32_MAX) {
        int32_t narrow_numerator = (int32_t) numerator;
        int32_t narrow_denominator = (int32_t) denominator;
        int32_t quotient = narrow_numerator / narrow_denominator;
        int32_t remainder = narrow_numerator % narrow_denominator;
        if (remainder < 0) remainder = -remainder;
        int32_t halfway = narrow_denominator / 2
                          + narrow_denominator % 2;
        if (remainder >= halfway) {
            quotient += narrow_numerator < 0 ? -1 : 1;
        }
        return quotient;
    }
    int64_t quotient = numerator / denominator;
    int64_t remainder = numerator % denominator;
    if (remainder < 0) remainder = -remainder;
    int64_t halfway = denominator / 2 + denominator % 2;
    if (remainder >= halfway) {
        quotient += numerator < 0 ? -1 : 1;
    }
    if (quotient > INT_MAX) return INT_MAX;
    if (quotient < INT_MIN) return INT_MIN;
    return (int) quotient;
}

int font_text_width_at_size_fixed_mode(
    const FontFace *face, const char *text, size_t length,
    int pixel_height_fixed, bool bold, bool kerning)
{
    const stbtt_fontinfo *info = font_info(face);
    if (text == NULL || pixel_height_fixed <= 0) return -1;
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
        if (!freetype_set_pixel_height_fixed(freetype, pixel_height_fixed)) {
            return -1;
        }
        int64_t width = 0;
        FT_UInt previous = 0;
        for (size_t at = 0; at < length;) {
            size_t sequence_used = 0;
            int sequence_advance = 0;
            if (optional_sequence_advance_fixed(
                    text + at, length - at, pixel_height_fixed, bold,
                    &sequence_used, &sequence_advance)) {
                width += sequence_advance;
                if (width > INT_MAX) return INT_MAX;
                previous = 0;
                at += sequence_used;
                continue;
            }
            unsigned codepoint = 0;
            size_t used = font_utf8_next(text + at, length - at,
                                         &codepoint);
            if (used == 0) break;
            if (font_codepoint_default_ignorable(codepoint)) {
                at += used;
                continue;
            }
            FT_UInt glyph = FT_Get_Char_Index(freetype->face, codepoint);
            int fallback_advance = 0;
            if (glyph == 0
                && (optional_fallback_advance_fixed(
                        codepoint, pixel_height_fixed, bold,
                        &fallback_advance)
                    || builtin_fallback_advance_fixed(
                        codepoint, pixel_height_fixed, bold,
                        &fallback_advance))) {
                width += fallback_advance;
                previous = 0;
                at += used;
                continue;
            }
            if (kerning && previous != 0 && glyph != 0
                && FT_HAS_KERNING(freetype->face)) {
                FT_Vector delta = {0};
                if (FT_Get_Kerning(freetype->face, previous, glyph,
                                   FT_KERNING_UNFITTED, &delta) == 0) {
                    width += delta.x;
                }
            }
            if (FT_Load_Glyph(freetype->face, glyph,
                              freetype_outline_load_flags()) != 0) return -1;
            width += freetype->face->glyph->advance.x + (bold ? 22 : 0);
            if (width > INT_MAX) return INT_MAX;
            if (width < INT_MIN) return INT_MIN;
            previous = glyph;
            at += used;
        }
        return (int) width;
    }
#endif
    if (info == NULL) return -1;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
    int units_per_em = stb_units_per_em(info);
    if (units_per_em <= 0) return -1;
    /* Keep fifths in the denominator so synthetic bold's 0.35 px is the
       exact 112/5 of a 26.6 pixel. Native advances and kerning remain in font
       design units; one final round preserves run-level wrapping semantics. */
    int64_t denominator = (int64_t) units_per_em * 5;
    int64_t numerator = 0;
    unsigned previous = 0;
    for (size_t at = 0; at < length;) {
        size_t sequence_used = 0;
        int sequence_advance = 0;
        if (optional_sequence_advance_fixed(
                text + at, length - at, pixel_height_fixed, bold,
                &sequence_used, &sequence_advance)) {
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) sequence_advance * denominator)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
            previous = 0;
            at += sequence_used;
            continue;
        }
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        if (font_codepoint_default_ignorable(codepoint)) {
            at += used;
            continue;
        }
        int fallback_advance = 0;
        /* STB is used only for the deliberately Latin-subset application
           faces. Prefer the built-in glyph here: format-4 cmaps can alias
           supplementary code points into a bogus non-zero glyph index. */
        if (optional_fallback_advance_fixed(
                codepoint, pixel_height_fixed, bold, &fallback_advance)
            || builtin_fallback_advance_fixed(
                codepoint, pixel_height_fixed, bold, &fallback_advance)) {
            if (!font_width_accumulate(
                    &numerator, (int64_t) fallback_advance * denominator)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
            previous = 0;
            at += used;
            continue;
        }
        if (kerning && previous != 0) {
            int kerning = stbtt_GetCodepointKernAdvance(
                info, (int) previous, (int) codepoint);
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) kerning * pixel_height_fixed * 5)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
        }
        int advance = 0;
        stbtt_GetCodepointHMetrics(info, (int) codepoint, &advance, NULL);
        int64_t delta = (int64_t) advance * pixel_height_fixed * 5
                        + (bold ? (int64_t) 112 * units_per_em : 0);
        if (!font_width_accumulate(&numerator, delta)) {
            return numerator < 0 ? INT_MIN : INT_MAX;
        }
        previous = codepoint;
        at += used;
    }
    return font_width_round_ratio(numerator, denominator);
}

int font_text_width_at_size_fixed(const FontFace *face, const char *text,
                                  size_t length, int pixel_height_fixed,
                                  bool bold)
{
    return font_text_width_at_size_fixed_mode(
        face, text, length, pixel_height_fixed, bold, true);
}

/* Advance widths in 2048-unit em space for printable ASCII.  These are a
   compact, independently measured Verdana-compatible profile, not embedded
   font software.  Reusing the bundled DejaVu outlines keeps the PSP font
   allocation unchanged while preserving the wrapping of a common legacy web
   family.  Native DejaVu kerning remains active between these advances. */
static const uint16_t humanist_sans_regular_advances[95] = {
    720, 806, 940, 1676, 1302, 2204, 1488, 550, 930, 930,
    1302, 1676, 745, 930, 745, 930, 1302, 1302, 1302, 1302,
    1302, 1302, 1302, 1302, 1302, 1302, 930, 930, 1676, 1676,
    1676, 1117, 2048, 1400, 1404, 1430, 1578, 1295, 1177, 1588,
    1539, 862, 931, 1419, 1140, 1726, 1532, 1612, 1235, 1612,
    1424, 1400, 1262, 1499, 1400, 2025, 1403, 1260, 1403, 930,
    930, 930, 1676, 1302, 1302, 1230, 1276, 1067, 1276, 1220,
    720, 1276, 1296, 562, 705, 1212, 562, 1992, 1296, 1243,
    1276, 1276, 874, 1067, 807, 1296, 1212, 1676, 1212, 1212,
    1076, 1300, 930, 1300, 1676
};

static const uint16_t humanist_sans_bold_advances[95] = {
    700, 824, 1203, 1776, 1456, 2605, 1766, 680, 1113, 1113,
    1456, 1776, 740, 983, 740, 1412, 1456, 1456, 1456, 1456,
    1456, 1456, 1456, 1456, 1456, 1456, 824, 824, 1776, 1776,
    1776, 1263, 1974, 1590, 1560, 1482, 1700, 1399, 1332, 1661,
    1715, 1118, 1137, 1579, 1305, 1941, 1734, 1741, 1501, 1741,
    1602, 1455, 1396, 1663, 1564, 2311, 1564, 1509, 1417, 1113,
    1412, 1113, 1776, 1456, 1456, 1368, 1432, 1205, 1432, 1360,
    865, 1432, 1459, 700, 825, 1374, 700, 2167, 1459, 1406,
    1432, 1432, 1018, 1215, 933, 1459, 1331, 2006, 1370, 1333,
    1222, 1456, 1113, 1456, 1776
};

static bool family_compatible_advance(FontFamily family, unsigned codepoint,
                                      bool bold, unsigned *units)
{
    if (family != FONT_HUMANIST_SANS || codepoint < 32
        || codepoint > 126 || units == NULL) return false;
    const uint16_t *advances = bold ? humanist_sans_bold_advances
                                    : humanist_sans_regular_advances;
    *units = advances[codepoint - 32];
    return true;
}

int font_text_width_for_family_at_size_fixed_mode(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int pixel_height_fixed,
    bool synthetic_bold, bool metric_bold, bool kerning)
{
    if (metric_family != FONT_HUMANIST_SANS) {
        return font_text_width_at_size_fixed_mode(
            face, text, length, pixel_height_fixed, synthetic_bold, kerning);
    }
    const stbtt_fontinfo *info = font_info(face);
    if (info == NULL) {
        return font_text_width_at_size_fixed_mode(
            face, text, length, pixel_height_fixed, synthetic_bold, kerning);
    }
    if (text == NULL || pixel_height_fixed <= 0) return -1;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
    int units_per_em = stb_units_per_em(info);
    if (units_per_em <= 0) return -1;
    /* Compatible Verdana metrics use a fixed 2048-unit em while native
       kerning stays in the loaded face's em. Their bounded common
       denominator lets both accumulate without floating point. */
    int64_t denominator = (int64_t) units_per_em * 2048 * 5;
    int64_t numerator = 0;
    unsigned previous = 0;
    for (size_t at = 0; at < length;) {
        size_t sequence_used = 0;
        int sequence_advance = 0;
        if (optional_sequence_advance_fixed(
                text + at, length - at, pixel_height_fixed,
                synthetic_bold, &sequence_used, &sequence_advance)) {
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) sequence_advance * denominator)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
            previous = 0;
            at += sequence_used;
            continue;
        }
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        if (font_codepoint_default_ignorable(codepoint)) {
            at += used;
            continue;
        }
        if (kerning && previous != 0) {
            int kerning = stbtt_GetCodepointKernAdvance(
                info, (int) previous, (int) codepoint);
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) kerning * pixel_height_fixed * 2048 * 5)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
        }
        unsigned compatible_units = 0;
        if (family_compatible_advance(
                metric_family, codepoint, metric_bold, &compatible_units)) {
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) compatible_units * pixel_height_fixed
                    * units_per_em * 5)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
        } else {
            int native_units = 0;
            stbtt_GetCodepointHMetrics(
                info, (int) codepoint, &native_units, NULL);
            if (!font_width_accumulate(
                    &numerator,
                    (int64_t) native_units * pixel_height_fixed
                    * 2048 * 5)) {
                return numerator < 0 ? INT_MIN : INT_MAX;
            }
        }
        if (synthetic_bold
            && !font_width_accumulate(
                   &numerator,
                   (int64_t) 112 * units_per_em * 2048)) {
            return numerator < 0 ? INT_MIN : INT_MAX;
        }
        previous = codepoint;
        at += used;
    }
    return font_width_round_ratio(numerator, denominator);
}

int font_text_width_for_family_at_size_fixed(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int pixel_height_fixed,
    bool synthetic_bold, bool metric_bold)
{
    return font_text_width_for_family_at_size_fixed_mode(
        face, metric_family, text, length, pixel_height_fixed,
        synthetic_bold, metric_bold, true);
}

int font_glyph_advance_for_family_at_size_fixed(
    FontFamily metric_family, unsigned codepoint, int pixel_height_fixed,
    bool synthetic_bold, bool metric_bold, int native_advance_fixed)
{
    unsigned compatible_units = 0;
    if (!family_compatible_advance(
            metric_family, codepoint, metric_bold, &compatible_units)
        || pixel_height_fixed <= 0) return native_advance_fixed;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
    int64_t advance = (int64_t) compatible_units * pixel_height_fixed;
    advance = (advance + 1024) / 2048 + (synthetic_bold ? 22 : 0);
    return advance > INT_MAX ? INT_MAX : (int) advance;
}

int font_text_width_fixed(const FontFace *face, const char *text,
                          size_t length, int pixel_height, bool bold)
{
    return font_text_width_at_size_fixed(
        face, text, length, integer_pixel_height_to_fixed(pixel_height), bold);
}

int font_text_width(const FontFace *face, const char *text, size_t length,
                    int pixel_height, bool bold)
{
    int fixed = font_text_width_fixed(
        face, text, length, pixel_height, bold);
    if (fixed < 0) return fixed;
    return fixed > INT_MAX - 32 ? INT_MAX / 64 : (fixed + 32) / 64;
}

int font_metric_height_fixed_at_size(const FontFace *face,
                                     int pixel_height_fixed)
{
    const stbtt_fontinfo *info = font_info(face);
    if (pixel_height_fixed <= 0) return -1;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        FT_Face scalable = freetype->face;
        /* size->metrics.height is grid-fitted to the nearest pixel.  That is
           useful for a terminal cell but makes CSS half-leading jump when a
           fractional design-space box happens to round down.  Scale the
           face's unhinted design metric and ceil it, matching the trusted
           stb path below. */
        int64_t design_height = scalable->height;
        if (design_height <= 0) {
            design_height = (int64_t) scalable->ascender
                            - scalable->descender;
        }
        int64_t units_per_em = scalable->units_per_EM;
        if (design_height <= 0 || units_per_em <= 0) {
            return pixel_height_fixed;
        }
        int64_t scaled = design_height * pixel_height_fixed;
        int64_t rounded = (scaled + units_per_em / 2) / units_per_em;
        if (rounded > INT_MAX) return INT_MAX;
        return rounded > 0 ? (int) rounded : 1;
    }
#endif
    if (info == NULL) return -1;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    float scale = css_font_scale_fixed(info, pixel_height_fixed);
    double fixed = (ascent - descent + gap) * (double) scale * 64.0;
    if (fixed > INT_MAX) return INT_MAX;
    return fixed > 0.0 ? (int) lround(fixed) : 1;
}

int font_metric_height_at_size(const FontFace *face,
                               int pixel_height_fixed)
{
    int fixed = font_metric_height_fixed_at_size(face, pixel_height_fixed);
    if (fixed < 0) return fixed;
    return fixed > INT_MAX - 63 ? INT_MAX / 64 : (fixed + 63) / 64;
}

int font_metric_height(const FontFace *face, int pixel_height)
{
    return font_metric_height_at_size(
        face, integer_pixel_height_to_fixed(pixel_height));
}

int font_line_height_fixed_at_size(const FontFace *face,
                                   int pixel_height_fixed)
{
    if (pixel_height_fixed <= 0) return -1;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
    int metric_height = font_metric_height_fixed_at_size(
        face, pixel_height_fixed);
    if (metric_height <= 0) return -1;
    /* CSS leaves `normal` line-height to the user agent, with roughly 1.2em
       as the interoperable expectation.  Several compact TrueType fallbacks
       expose zero lineGap and a shorter hhea box (for example 14px at a
       12px em), which otherwise packs successive lines more tightly than
       common browser fonts.  Preserve any taller face metrics while giving
       a zero-gap fallback the same bounded minimum leading. */
    int64_t scaled_floor = (int64_t) pixel_height_fixed * 6;
    int normal_floor = scaled_floor > (int64_t) INT_MAX * 5
                       ? INT_MAX : (int) ((scaled_floor + 2) / 5);
    return metric_height < normal_floor ? normal_floor : metric_height;
}

int font_line_height_at_size(const FontFace *face, int pixel_height_fixed)
{
    int fixed = font_line_height_fixed_at_size(face, pixel_height_fixed);
    if (fixed < 0) return fixed;
    return fixed > INT_MAX - 63 ? INT_MAX / 64 : (fixed + 63) / 64;
}

int font_line_height(const FontFace *face, int pixel_height)
{
    return font_line_height_at_size(
        face, integer_pixel_height_to_fixed(pixel_height));
}

int font_ascent_at_size(const FontFace *face, int pixel_height_fixed)
{
    const stbtt_fontinfo *info = font_info(face);
    if (pixel_height_fixed <= 0) return -1;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        if (!freetype_set_pixel_height_fixed(freetype, pixel_height_fixed)) {
            return -1;
        }
        FT_Pos ascent = freetype->face->size->metrics.ascender;
        int64_t rounded = ((int64_t) ascent + 63) / 64;
        if (rounded > INT_MAX) return INT_MAX;
        if (rounded < INT_MIN) return INT_MIN;
        return (int) rounded;
    }
#endif
    if (info == NULL) return -1;
    int ascent = 0;
    stbtt_GetFontVMetrics(info, &ascent, NULL, NULL);
    return (int) ceil(ascent * css_font_scale_fixed(info, pixel_height_fixed));
}

int font_ascent(const FontFace *face, int pixel_height)
{
    return font_ascent_at_size(
        face, integer_pixel_height_to_fixed(pixel_height));
}

int font_line_baseline_at_size(const FontFace *face,
                               int pixel_height_fixed,
                               int used_line_height)
{
    int ascent = font_ascent_at_size(face, pixel_height_fixed);
    if (ascent < 0) return -1;
    int metric_height = font_metric_height_at_size(face, pixel_height_fixed);
    if (used_line_height <= 0 || metric_height <= 0) return ascent;

    /* CSS distributes the difference between the used line-height and the
       font's metric box equally above and below the glyphs.  The difference
       may be negative (tight line-height), so use floor division rather than
       C's truncation toward zero.  Keep all arithmetic wide because both the
       stylesheet and a hostile font contribute to this calculation. */
    int64_t leading = (int64_t) used_line_height - metric_height;
    int64_t half_leading = leading >= 0
        ? leading / 2 : -((-leading + 1) / 2);
    int64_t baseline = (int64_t) ascent + half_leading;
    if (baseline > INT_MAX) return INT_MAX;
    if (baseline < INT_MIN) return INT_MIN;
    return (int) baseline;
}

int font_line_baseline(const FontFace *face, int pixel_height,
                       int used_line_height)
{
    return font_line_baseline_at_size(
        face, integer_pixel_height_to_fixed(pixel_height), used_line_height);
}

int font_kerning(const FontFace *face, unsigned left, unsigned right,
                 int pixel_height)
{
    const stbtt_fontinfo *info = font_info(face);
    if (pixel_height <= 0 || left == 0 || right == 0) return 0;
    int pixel_height_fixed = integer_pixel_height_to_fixed(pixel_height);
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        if (!freetype_set_pixel_height_fixed(freetype, pixel_height_fixed)
            || !FT_HAS_KERNING(freetype->face)) return 0;
        FT_Vector delta = {0};
        FT_UInt left_glyph = FT_Get_Char_Index(freetype->face, left);
        FT_UInt right_glyph = FT_Get_Char_Index(freetype->face, right);
        if (left_glyph == 0 || right_glyph == 0
            || FT_Get_Kerning(freetype->face, left_glyph, right_glyph,
                              FT_KERNING_UNFITTED, &delta) != 0) return 0;
        return fixed_26_6_to_int(delta.x) / 64;
    }
#endif
    if (info == NULL) return 0;
    float scale = css_font_scale_fixed(info, pixel_height_fixed);
    return (int) lround(stbtt_GetCodepointKernAdvance(
                            info, (int) left, (int) right) * scale);
}

int font_kerning_at_size_fixed(const FontFace *face, unsigned left,
                               unsigned right, int pixel_height_fixed)
{
    const stbtt_fontinfo *info = font_info(face);
    if (pixel_height_fixed <= 0 || left == 0 || right == 0) return 0;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        if (!freetype_set_pixel_height_fixed(freetype, pixel_height_fixed)
            || !FT_HAS_KERNING(freetype->face)) return 0;
        FT_Vector delta = {0};
        FT_UInt left_glyph = FT_Get_Char_Index(freetype->face, left);
        FT_UInt right_glyph = FT_Get_Char_Index(freetype->face, right);
        if (left_glyph == 0 || right_glyph == 0
            || FT_Get_Kerning(freetype->face, left_glyph, right_glyph,
                              FT_KERNING_UNFITTED, &delta) != 0) return 0;
        return fixed_26_6_to_int(delta.x);
    }
#endif
    if (info == NULL) return 0;
    float scale = css_font_scale_fixed(info, pixel_height_fixed);
    return (int) lround(stbtt_GetCodepointKernAdvance(
                            info, (int) left, (int) right) * scale * 64.0f);
}

int font_kerning_fixed(const FontFace *face, unsigned left, unsigned right,
                       int pixel_height)
{
    return font_kerning_at_size_fixed(
        face, left, right, integer_pixel_height_to_fixed(pixel_height));
}

bool font_glyph_load_at_size(const FontFace *face, unsigned codepoint,
                             int pixel_height_fixed, bool bold,
                             FontGlyph *glyph)
{
    if (glyph == NULL) return false;
    memset(glyph, 0, sizeof(*glyph));
    const stbtt_fontinfo *info = font_info(face);
    if (face == NULL || pixel_height_fixed <= 0) return false;
    pixel_height_fixed = bounded_pixel_height_fixed(pixel_height_fixed);
    if (codepoint > 0x10ffffu) {
        bool pending = false;
        bool loaded = optional_fallback_glyph(
            face, codepoint, pixel_height_fixed, bold, glyph, &pending);
        if (loaded) glyph->provider_pending = pending;
        return loaded;
    }
#ifdef TILEFINCH_HAVE_FREETYPE
    FreeTypeWebFace *freetype = freetype_info(face);
    if (freetype != NULL) {
        if (!freetype_set_pixel_height_fixed(freetype, pixel_height_fixed)) {
            return false;
        }
        FT_UInt index = FT_Get_Char_Index(freetype->face, codepoint);
        bool provider_pending = false;
        if (index == 0) {
            if (optional_fallback_glyph(
                    face, codepoint, pixel_height_fixed, bold, glyph,
                    &provider_pending)) return true;
            if (builtin_fallback_supported(codepoint)) {
                bool loaded = builtin_fallback_glyph(
                    face, codepoint, pixel_height_fixed, bold, glyph);
                if (loaded) glyph->provider_pending = provider_pending;
                return loaded;
            }
        }
        if (FT_Load_Glyph(freetype->face, index,
                          freetype_outline_load_flags()) != 0) return false;
        FT_GlyphSlot slot = freetype->face->glyph;
        FT_Pos metric_width = slot->metrics.width;
        FT_Pos metric_height = slot->metrics.height;
        if (metric_width < 0 || metric_height < 0) return false;
        size_t predicted_width =
            (size_t) (((int64_t) metric_width + 63) / 64);
        size_t predicted_height =
            (size_t) (((int64_t) metric_height + 63) / 64);
        size_t dimension_limit = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 4u;
        if (predicted_width > dimension_limit
            || predicted_height > dimension_limit
            || (predicted_height != 0
                && predicted_width
                       > TILEFINCH_WEB_FONT_GLYPH_PIXEL_LIMIT
                         / predicted_height)) return false;
        FT_Pos advance = slot->advance.x + (bold ? 22 : 0);
        if (freetype_outline_is_axis_aligned_rectangle(slot)) {
            size_t width = predicted_width;
            size_t height = predicted_height;
            unsigned char *pixels = NULL;
            if (width != 0 && height != 0) {
                pixels = budget_malloc(face->budget, width * height);
                if (pixels == NULL) return false;
                memset(pixels, 255, width * height);
            }
            glyph->pixels = pixels;
            glyph->budget = face->budget;
            glyph->width = (int) width;
            glyph->height = (int) height;
            glyph->x_offset = fixed_26_6_to_int(
                slot->metrics.horiBearingX) / 64;
            glyph->y_offset = -fixed_26_6_to_int(
                slot->metrics.horiBearingY) / 64;
            glyph->advance_fixed = fixed_26_6_to_int(advance);
            glyph->advance = (glyph->advance_fixed + 32) / 64;
            return true;
        }
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return false;
        size_t width = slot->bitmap.width;
        size_t height = slot->bitmap.rows;
        if (width > dimension_limit || height > dimension_limit
            || (height != 0
                && width > TILEFINCH_WEB_FONT_GLYPH_PIXEL_LIMIT / height)) {
            return false;
        }
        unsigned char *pixels = NULL;
        if (width != 0 && height != 0) {
            pixels = budget_malloc(face->budget, width * height);
            if (pixels == NULL) return false;
            int pitch = slot->bitmap.pitch;
            size_t source_pitch = (size_t) (pitch < 0 ? -pitch : pitch);
            for (size_t row = 0; row < height; row++) {
                size_t source_row = pitch < 0 ? height - row - 1 : row;
                const unsigned char *source = slot->bitmap.buffer
                    + source_row * source_pitch;
                if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                    if (source_pitch < width) {
                        budget_free(face->budget, pixels);
                        return false;
                    }
                    unsigned levels = slot->bitmap.num_grays;
                    for (size_t column = 0; column < width; column++) {
                        pixels[row * width + column] = levels > 1
                            ? (unsigned char) ((unsigned) source[column]
                                               * 255u / (levels - 1u))
                            : 0;
                    }
                } else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    if (source_pitch < (width + 7u) / 8u) {
                        budget_free(face->budget, pixels);
                        return false;
                    }
                    for (size_t column = 0; column < width; column++) {
                        pixels[row * width + column] =
                            (source[column / 8u]
                             & (unsigned char) (0x80u >> (column & 7u)))
                            ? 255 : 0;
                    }
                } else {
                    budget_free(face->budget, pixels);
                    return false;
                }
            }
        }
        glyph->pixels = pixels;
        glyph->budget = face->budget;
        glyph->width = (int) width;
        glyph->height = (int) height;
        glyph->x_offset = slot->bitmap_left;
        glyph->y_offset = -slot->bitmap_top;
        glyph->advance_fixed = fixed_26_6_to_int(advance);
        glyph->advance = (glyph->advance_fixed + 32) / 64;
        glyph->provider_pending = provider_pending;
        return true;
    }
#endif
    if (info == NULL) return false;
    bool provider_pending = false;
    if (optional_fallback_glyph(
            face, codepoint, pixel_height_fixed, bold, glyph,
            &provider_pending)) return true;
    if (builtin_fallback_supported(codepoint)) {
        bool loaded = builtin_fallback_glyph(
            face, codepoint, pixel_height_fixed, bold, glyph);
        if (loaded) glyph->provider_pending = provider_pending;
        return loaded;
    }
    float scale = css_font_scale_fixed(info, pixel_height_fixed);
    glyph->pixels = stbtt_GetCodepointBitmap(info, scale, scale,
                                              (int) codepoint,
                                              &glyph->width, &glyph->height,
                                              &glyph->x_offset,
                                              &glyph->y_offset);
    int advance = 0;
    stbtt_GetCodepointHMetrics(info, (int) codepoint, &advance, NULL);
    float scaled_advance = advance * scale + (bold ? 0.35f : 0.0f);
    glyph->advance = (int) lround(scaled_advance);
    glyph->advance_fixed = (int) lround(scaled_advance * 64.0f);
    glyph->budget = face->budget;
    glyph->provider_pending = provider_pending;
    return glyph->pixels != NULL || (glyph->width == 0 && glyph->height == 0);
}

bool font_glyph_load(const FontFace *face, unsigned codepoint,
                     int pixel_height, bool bold, FontGlyph *glyph)
{
    return font_glyph_load_at_size(
        face, codepoint, integer_pixel_height_to_fixed(pixel_height), bold,
        glyph);
}

void font_glyph_destroy(const FontFace *face, FontGlyph *glyph)
{
    (void) face;
    if (glyph == NULL) return;
    if (glyph->colors != NULL && glyph->budget != NULL)
        budget_free(glyph->budget, glyph->colors);
    if (glyph->pixels != NULL && glyph->budget != NULL)
        budget_free(glyph->budget, glyph->pixels);
    memset(glyph, 0, sizeof(*glyph));
}
