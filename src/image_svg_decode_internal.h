#ifndef TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H
#define TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H

#include <stddef.h>

#include "tilefinch/budget.h"

unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height);

#endif
