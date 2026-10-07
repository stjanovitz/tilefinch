/* libFuzzer target: page-provided web fonts (TrueType sfnt / WOFF 1.0)
   through font_face_load_encoded and the FreeType-backed metric, kerning
   and glyph-raster paths in src/font.c. The whole input is the font. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/font.h"

#define MIB (1024u * 1024u)

static const char sample[] =
    "Hello, World! AVATAR fi fl \xc3\xa9\xc3\x9f \xd7\xa9\xd7\x9c "
    "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80 0123456789";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4 || size > 512 * 1024) return 0;
    unsigned char *font = malloc(size);
    if (font == NULL) return 0;
    memcpy(font, data, size);
    Budget budget;
    budget_init(&budget, 24u * MIB);
    FontFace face;
    memset(&face, 0, sizeof(face));
    if (font_face_load_encoded(&face, &budget, font, size, 4u * MIB)) {
        static const int sizes[] = {1, 12, 37, 120};
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            int pixel_height = sizes[s];
            (void) font_text_width(&face, sample, sizeof(sample) - 1,
                                   pixel_height, s & 1u);
            (void) font_text_width_at_size_fixed(
                &face, sample, sizeof(sample) - 1, pixel_height * 64 + 17,
                false);
            (void) font_metric_height(&face, pixel_height);
            (void) font_line_height(&face, pixel_height);
            (void) font_ascent(&face, pixel_height);
            (void) font_kerning(&face, 'A', 'V', pixel_height);
            (void) font_kerning_fixed(&face, 'T', 'o', pixel_height);
        }
        size_t offset = 0;
        while (offset < sizeof(sample) - 1) {
            unsigned codepoint = 0;
            size_t used = font_utf8_next(sample + offset,
                                         sizeof(sample) - 1 - offset,
                                         &codepoint);
            if (used == 0) break;
            offset += used;
            (void) font_face_has_codepoint(&face, codepoint);
            FontGlyph glyph;
            memset(&glyph, 0, sizeof(glyph));
            if (font_glyph_load(&face, codepoint, 18, offset & 1u, &glyph)) {
                if (glyph.pixels != NULL && glyph.width > 0
                    && glyph.height > 0) {
                    volatile unsigned char sink = glyph.pixels[
                        (size_t) glyph.width * (size_t) glyph.height - 1u];
                    (void) sink;
                }
                font_glyph_destroy(&face, &glyph);
            }
        }
        font_face_destroy(&face);
    }
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_font: budget leak of %zu bytes\n",
                budget.current);
        abort();
    }
    free(font);
    return 0;
}
