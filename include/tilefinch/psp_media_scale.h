#ifndef TILEFINCH_PSP_MEDIA_SCALE_H
#define TILEFINCH_PSP_MEDIA_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Presentation scaler for decoded video.
 *
 * The Media Engine hands back a macroblock-padded 32-bit ABGR surface at a
 * fixed stride; the panel wants RGB565 inside a display rectangle. Doing that
 * with a per-pixel index divide and three byte loads cost 10.1 ms per frame on
 * a PSP-3000 (cycle E3: scale-total 6643464us over 655 frames), which is a
 * third of a 30 fps budget spent before the compositor draws anything.
 *
 * This module answers the same question with tables built once per geometry:
 * a source byte offset per destination column, a source row per destination
 * row, and a note of which destination rows merely repeat the row above. The
 * hot loop then reads one aligned word per pixel, converts with shift/mask
 * only, and publishes two destination pixels per 32-bit store.
 *
 * The filter is nearest-neighbour, as it was before. Video frames are live
 * content and are never goldened, so the fidelity floors do not constrain the
 * choice; the budget does.
 */

#define PSP_MEDIA_SCALE_MAX_OUTPUT_WIDTH 480
#define PSP_MEDIA_SCALE_MAX_OUTPUT_HEIGHT 272

typedef enum {
    /* Little-endian byte order R,G,B,A -- what sceMpegBaseCscAvc writes. */
    PSP_MEDIA_SCALE_RGBA8888 = 0,
    PSP_MEDIA_SCALE_RGB565 = 1
} PspMediaScaleFormat;

typedef struct {
    int source_width;
    int source_height;
    int source_stride_pixels;
    int output_width;
    int output_height;
    PspMediaScaleFormat format;
    /* Set when every destination column samples its own source column, which
       lets the blitter walk the source sequentially instead of gathering. */
    bool identity_columns;
    /* Byte offset into a source row for each destination column. */
    uint32_t column_offset[PSP_MEDIA_SCALE_MAX_OUTPUT_WIDTH];
    /* Source row for each destination row. */
    uint16_t row_index[PSP_MEDIA_SCALE_MAX_OUTPUT_HEIGHT];
    /* True when row_index[y] == row_index[y - 1]: the blitter copies the
       destination row it just produced rather than converting it again. */
    bool row_repeats[PSP_MEDIA_SCALE_MAX_OUTPUT_HEIGHT];
} PspMediaScaleMap;

/*
 * Build the tables. Refuses geometry the display cannot hold or that would
 * sample outside the source, so a caller cannot turn a stream geometry change
 * into an out-of-bounds read.
 */
bool psp_media_scale_map_build(
    PspMediaScaleMap *map, PspMediaScaleFormat format,
    int source_width, int source_height, int source_stride_pixels,
    int output_width, int output_height);

/* True when the map already describes exactly this geometry. */
bool psp_media_scale_map_matches(
    const PspMediaScaleMap *map, PspMediaScaleFormat format,
    int source_width, int source_height, int source_stride_pixels,
    int output_width, int output_height);

/*
 * Convert and scale the source into destination[0 .. output_height) rows of
 * destination_stride_pixels. The destination is the display rectangle's top
 * left pixel, not the buffer origin.
 */
void psp_media_scale_blit(
    const PspMediaScaleMap *map, const void *source,
    uint16_t *destination, int destination_stride_pixels);

/*
 * Reference implementation kept for the host tests: the straightforward
 * per-pixel form the fast path has to agree with byte for byte.
 */
void psp_media_scale_blit_reference(
    const PspMediaScaleMap *map, const void *source,
    uint16_t *destination, int destination_stride_pixels);

/*
 * One source pixel through the same conversion the blitter uses. Exported so
 * a second presenter can be held to this one's answer: the panel's channel
 * order is whatever this function says it is, and a presenter that disagrees
 * would change a video's colours when the viewer changes the scaling option.
 */
uint16_t psp_media_scale_convert_pixel(uint32_t rgba);

/*
 * True when the platform's conversion agrees with the portable one. The
 * Allegrex build names its bitfield instructions in assembly, so this is the
 * device-side proof that the toolchain assembled what was meant. Cheap enough
 * to run once per opened stream.
 */
bool psp_media_scale_self_check(void);

#endif
