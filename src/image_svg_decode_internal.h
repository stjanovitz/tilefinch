#ifndef TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H
#define TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"

/* Rasterizes SVG markup. On entry *width and *height are the raster size
   wanted (zero for the document's own size, rounded up); the picture is
   scaled uniformly to fit it. On success they hold the raster's size.
   Returns NULL on failure, including while another rasterization is in
   progress (image_svg_decode_busy). */
bool image_svg_decode_busy(void);
typedef bool (*ImageSvgOversizeTarget)(void *opaque, int source_width,
                                      int source_height, int *width,
                                      int *height);
/* Only consult the target when the intrinsic raster exceeds the quota.
   Successful ordinary decodes are unchanged; intrinsic dimensions remain
   separate from the returned pixels. Refused allocations are never retried.
   On a NULL return, *limited (when not NULL) tells why: true when the
   decode was refused by this engine's limits rather than by the markup,
   that is for memory (a Budget refusal while parsing or rasterizing), the
   decoded-bytes quota, a declined or missing oversize target, an
   intrinsic size past 32767 px or re-entry; false for markup no decoder
   takes, which fails the same way every time. */
unsigned char *image_svg_decode_bounded(
    const void *data, size_t length, Budget *budget,
    size_t maximum_decoded_bytes, int *width, int *height,
    ImageSvgOversizeTarget target, void *opaque,
    int *source_width, int *source_height, bool *limited);
unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height);

#endif
