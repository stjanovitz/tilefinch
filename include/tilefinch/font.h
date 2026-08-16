#ifndef TILEFINCH_FONT_H
#define TILEFINCH_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

typedef struct TilefinchGlyphProvider TilefinchGlyphProvider;

/* Keep retained transform products from turning a single glyph into an
   unbounded stb_truetype raster job on the PSP.  This is deliberately above
   the largest untransformed CSS font accepted by the style layer. */
#define TILEFINCH_FONT_RASTER_PIXEL_LIMIT 256
#define TILEFINCH_BITMAP_FONT_SCALE_LIMIT 32
#define TILEFINCH_WEB_FONT_FAMILY_LIMIT 2
#define TILEFINCH_WEB_FONT_FAMILY_STRIDE 5
#define TILEFINCH_WEB_FONT_BASE 16
#define TILEFINCH_WEB_FONT_GLYPH_PIXEL_LIMIT (64u * 1024u)

typedef enum {
    FONT_FACE_BACKEND_NONE = 0,
    /* Application assets are trusted build inputs and retain the smaller
       stb implementation used by the existing PSP renderer. */
    FONT_FACE_BACKEND_STB_TRUSTED,
    /* Page-provided bytes must use the size-aware FreeType backend. */
    FONT_FACE_BACKEND_FREETYPE_WEB
} FontFaceBackend;

/* Font families are retained in every computed style.  The two web-font
   slots are encoded above the built-ins but still fit comfortably in one
   byte; keep the semantic type explicit instead of relying on the compiler's
   four-byte enum representation. */
typedef uint8_t FontFamily;
enum {
    FONT_SANS,
    FONT_SERIF,
    FONT_MONOSPACE,
    FONT_METRIC_SANS,
    /* Keep a separate family identity while reusing the bounded sans face so
       named Verdana stacks can apply compatible Latin advances without
       retaining another raster font on the PSP. */
    FONT_HUMANIST_SANS
};

_Static_assert(TILEFINCH_WEB_FONT_BASE
                   + TILEFINCH_WEB_FONT_FAMILY_LIMIT
                     * TILEFINCH_WEB_FONT_FAMILY_STRIDE
                   <= UINT8_MAX,
               "web-font family encoding must remain byte-sized");

typedef struct {
    Budget *budget;
    unsigned char *data;
    size_t data_length;
    void *implementation;
    FontFaceBackend backend;
    bool loaded;
} FontFace;

typedef struct {
    FontFace sans;
    FontFace sans_bold;
    FontFace sans_italic;
    FontFace serif;
    FontFace serif_bold;
    FontFace metric_sans;
    FontFace metric_sans_bold;
} FontSet;

/* Page-owned faces stay separate from the application fallback set so a
   transactional navigation can stage and discard them without mutating the
   incumbent page.  The style layer encodes the family slot plus a bounded
   fallback family in FontFamily. */
typedef struct {
    FontFace regular;
    FontFace bold;
} WebFontFamilyFaces;

typedef struct {
    WebFontFamilyFaces families[TILEFINCH_WEB_FONT_FAMILY_LIMIT];
} WebFontSet;

typedef struct {
    unsigned char *pixels;
    uint16_t *colors;
    Budget *budget;
    int width;
    int height;
    int x_offset;
    int y_offset;
    int advance;
    int advance_fixed;
    /* A preferred optional glyph was queued but not storage-resident yet.
       The renderer may show the embedded fallback for this frame, but must
       not retain it in the glyph cache across the provider pump. */
    bool provider_pending;
} FontGlyph;

/* Process-singleton binding for immutable optional packs. Font work and the
   provider pump both run on the browser thread; ownership remains with the
   application component session. */
bool font_optional_glyph_provider_install(TilefinchGlyphProvider *provider);
bool font_optional_glyph_provider_uninstall(
    TilefinchGlyphProvider *provider);
/* Match a multi-codepoint optional glyph. Single-scalar fallback retains the
   normal face-first policy; this path exists so one grapheme is measured and
   rasterized under one opaque cache key. */
bool font_optional_glyph_match_sequence(
    const char *text, size_t length, size_t *used, unsigned *glyph_key);
bool font_optional_glyph_match_sequence_ending_at(
    const char *text, size_t end, size_t *start, unsigned *glyph_key);

bool font_set_load(FontSet *fonts, Budget *budget,
                   const char *sans_path, const char *serif_path,
                   const char *sans_italic_path, const char *sans_bold_path,
                   const char *serif_bold_path,
                   const char *metric_sans_path,
                   const char *metric_sans_bold_path,
                   size_t max_total_bytes);
void font_set_destroy(FontSet *fonts);
const FontFace *font_set_face(const FontSet *fonts, FontFamily family);
const FontFace *font_set_face_style(const FontSet *fonts, FontFamily family,
                                    bool italic);
const FontFace *font_set_face_variant(const FontSet *fonts, FontFamily family,
                                      bool italic, bool bold);
bool font_set_face_is_bold(const FontSet *fonts, const FontFace *face);
bool font_family_web(unsigned slot, FontFamily fallback, FontFamily *family);
bool font_family_is_web(FontFamily family);
unsigned font_family_web_slot(FontFamily family);
FontFamily font_family_web_fallback(FontFamily family);
/* Return the platform fallback family whose metrics apply to the selected
   face.  A successfully loaded web face uses its own metrics instead. */
FontFamily font_context_metric_family(
    const WebFontSet *web_fonts, FontFamily family, const FontFace *face);
const FontFace *font_context_face_variant(
    const FontSet *fonts, const WebFontSet *web_fonts, FontFamily family,
    bool italic, bool bold);
const FontFace *font_context_face(
    const FontSet *fonts, const WebFontSet *web_fonts, FontFamily family);
bool font_context_face_is_bold(
    const FontSet *fonts, const WebFontSet *web_fonts,
    const FontFace *face);
/* Accepts bounded TrueType-flavoured sfnt or WOFF 1.0 data through the
   size-aware page-font backend and owns an internal encoded copy on success.
   WOFF2, TTC, CFF, and every stb path are deliberately rejected here. */
bool font_face_load_encoded(FontFace *face, Budget *budget,
                            const unsigned char *data, size_t length,
                            size_t maximum_backend_bytes);
void font_face_destroy(FontFace *face);
void web_font_set_destroy(WebFontSet *fonts);
int font_text_width(const FontFace *face, const char *text, size_t length,
                    int pixel_height, bool bold);
/* Returns the same advance in signed 26.6 pixels so adjacent text runs can
   retain fractional glyph advances instead of rounding every DOM/word
   boundary independently. */
int font_text_width_fixed(const FontFace *face, const char *text,
                          size_t length, int pixel_height, bool bold);
int font_text_width_at_size_fixed(const FontFace *face, const char *text,
                                  size_t length, int pixel_height_fixed,
                                  bool bold);
/* Measure a platform family with any bounded metric-compatibility profile.
   `metric_bold` chooses the real bold face's advances independently of the
   synthetic emboldening flag used by the raster backend. */
int font_text_width_for_family_at_size_fixed(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int pixel_height_fixed,
    bool synthetic_bold, bool metric_bold);
int font_glyph_advance_for_family_at_size_fixed(
    FontFamily metric_family, unsigned codepoint, int pixel_height_fixed,
    bool synthetic_bold, bool metric_bold, int native_advance_fixed);
/* Returns the unhinted scalable face metric box at the CSS font size.  This
   deliberately excludes the user agent's minimum `normal` line-height. */
int font_metric_height(const FontFace *face, int pixel_height);
int font_metric_height_at_size(const FontFace *face,
                               int pixel_height_fixed);
int font_metric_height_fixed_at_size(const FontFace *face,
                                     int pixel_height_fixed);
int font_line_height(const FontFace *face, int pixel_height);
int font_line_height_at_size(const FontFace *face, int pixel_height_fixed);
int font_line_height_fixed_at_size(const FontFace *face,
                                   int pixel_height_fixed);
int font_ascent(const FontFace *face, int pixel_height);
int font_ascent_at_size(const FontFace *face, int pixel_height_fixed);
/* Returns the baseline relative to the top of a used line box, including
   bounded CSS half-leading for an explicit used line-height. */
int font_line_baseline(const FontFace *face, int pixel_height,
                       int used_line_height);
int font_line_baseline_at_size(const FontFace *face,
                               int pixel_height_fixed,
                               int used_line_height);
int font_kerning(const FontFace *face, unsigned left, unsigned right,
                 int pixel_height);
int font_kerning_fixed(const FontFace *face, unsigned left, unsigned right,
                       int pixel_height);
int font_kerning_at_size_fixed(const FontFace *face, unsigned left,
                               unsigned right, int pixel_height_fixed);
/* Exact cmap coverage. font_glyph_load() succeeds for a codepoint the face
   lacks -- it renders .notdef, which is blank in one shipped subset and a box
   in another -- so a caller that must know whether the face really has the
   character has to ask this instead of reading a load result. */
bool font_face_has_codepoint(const FontFace *face, unsigned codepoint);
bool font_glyph_load(const FontFace *face, unsigned codepoint,
                     int pixel_height, bool bold, FontGlyph *glyph);
bool font_glyph_load_at_size(const FontFace *face, unsigned codepoint,
                             int pixel_height_fixed, bool bold,
                             FontGlyph *glyph);
void font_glyph_destroy(const FontFace *face, FontGlyph *glyph);
size_t font_utf8_next(const char *text, size_t remaining,
                      unsigned *codepoint);
bool font_codepoint_default_ignorable(unsigned codepoint);

#endif
