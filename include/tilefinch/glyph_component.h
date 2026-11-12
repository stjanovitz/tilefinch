#ifndef TILEFINCH_GLYPH_COMPONENT_H
#define TILEFINCH_GLYPH_COMPONENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define TILEFINCH_GLYPH_COMPONENT_ABI 1u
#define TILEFINCH_GLYPH_COMPONENT_ID_LIMIT 24u
#define TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT 512u
#define TILEFINCH_GLYPH_COMPONENT_PACK_LIMIT 4u
#define TILEFINCH_GLYPH_COMPONENT_LAZY_LANGUAGE_LIMIT 2u
_Static_assert(TILEFINCH_GLYPH_COMPONENT_PACK_LIMIT <= 4u,
               "glyph provider keys reserve two bits for the pack index");
#define TILEFINCH_GLYPH_COMPONENT_SEQUENCE_LIMIT 4096u
#define TILEFINCH_GLYPH_COMPONENT_SEQUENCE_CODEPOINT_LIMIT 8u
#define TILEFINCH_GLYPH_COMPONENT_GLYPH_LIMIT 131072u
#define TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT 4352u
#define TILEFINCH_GLYPH_COMPONENT_NOTICE_LIMIT (64u * 1024u)

typedef enum {
    TILEFINCH_GLYPH_COMPONENT_MONO = 1,
    TILEFINCH_GLYPH_COMPONENT_COLOR = 2
} TilefinchGlyphComponentKind;

/* A process owns at most one provider. The browser engine is already a
   process singleton because Lexbor's allocator is process-global, so font
   fallback can bind this provider without creating a second lifetime model. */
typedef struct TilefinchGlyphProvider TilefinchGlyphProvider;

typedef struct {
    const uint8_t *pixels;
    unsigned width;
    unsigned height;
    TilefinchGlyphComponentKind kind;
    /* A monochrome glyph's own columns are first_column .. first_column +
       width - 1 of its cell. baseline_rows is how many cell rows sit above the
       baseline, or 0 when the pack declares none and glyphs use the fallback
       7/8 em baseline. */
    unsigned first_column;
    unsigned baseline_rows;
} TilefinchGlyphSource;

TilefinchGlyphProvider *tilefinch_glyph_provider_create(Budget *budget);
void tilefinch_glyph_provider_destroy(TilefinchGlyphProvider *provider);

/* A scalar script's letters need a shared baseline and their own widths, which
   square CJK cells do not. A pack declares both with a nonzero header byte 15
   (cell rows above the baseline) plus one byte per glyph in the last glyph_count
   bytes of its notice: high nibble columns - 1, low nibble first column. They
   sit there so a reader that predates them still accepts the same file and
   draws its cells at their full width. The table is read at attach because
   measuring text may not touch the file. */

/* Opens and validates only the bounded index. Glyph payloads remain on disk
   and are read solely by tilefinch_glyph_provider_pump(). Earlier packs have
   priority, allowing a regional override to precede neutral Extended Han. */
bool tilefinch_glyph_provider_attach(
    TilefinchGlyphProvider *provider, const char *path,
    const char *expected_component_id);

/* Match one codepoint or the longest installed emoji/Indic/Kana sequence. `glyph_key`
   is an opaque provider key, not a Unicode scalar. Ordinary callers should
   retain the original UTF-8 when this returns false. */
bool tilefinch_glyph_provider_match(
    TilefinchGlyphProvider *provider, const char *text, size_t length,
    size_t *used, uint32_t *glyph_key, unsigned *source_width);
bool tilefinch_glyph_provider_has_codepoint(
    TilefinchGlyphProvider *provider, unsigned codepoint,
    uint32_t *glyph_key, unsigned *source_width);
/* Whether a glyph comes from a pack that declares a baseline and column boxes. */
bool tilefinch_glyph_provider_key_is_metric(
    const TilefinchGlyphProvider *provider, uint32_t glyph_key);
bool tilefinch_glyph_provider_metrics(
    const TilefinchGlyphProvider *provider, uint32_t glyph_key,
    unsigned *source_width, unsigned *source_height,
    TilefinchGlyphComponentKind *kind);

/* A cache miss queues the containing block and returns false. No file I/O is
   permitted here: measurement and rasterization call this on hot paths. */
bool tilefinch_glyph_provider_source(
    TilefinchGlyphProvider *provider, uint32_t glyph_key,
    TilefinchGlyphSource *source);
bool tilefinch_glyph_provider_key_pending(
    const TilefinchGlyphProvider *provider, uint32_t glyph_key);

/* Reads at most maximum_bytes from one queued block. `changed` means at least
   one requested glyph became available and cached page tiles should repaint. */
bool tilefinch_glyph_provider_pump(
    TilefinchGlyphProvider *provider, size_t maximum_bytes, bool *changed,
    size_t *bytes_read);
size_t tilefinch_glyph_provider_pack_count(
    const TilefinchGlyphProvider *provider);

#endif
