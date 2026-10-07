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
unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height);

#endif
