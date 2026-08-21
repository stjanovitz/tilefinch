#ifndef TILEFINCH_SWDEC_BOUNDS_H
#define TILEFINCH_SWDEC_BOUNDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared, allocation-free admission rules for data that crosses into the
 * optional decoder or its Media Engine helpers. Keep these checks at every
 * ownership boundary: a source parser, decoder, or component can fail
 * independently and none may authorize a write into the next owner's fixed
 * buffer. */
static inline bool swdec_dimensions_admitted(
    int width, int height, int max_width, int max_height)
{
    return width > 0 && height > 0 && max_width > 0 && max_height > 0
        && width <= max_width && height <= max_height;
}

static inline bool swdec_rgb565_destination_fits(
    int width, int height, int stride_pixels, size_t capacity_bytes)
{
    if (width <= 0 || height <= 0 || (width & 1) != 0
        || stride_pixels < width)
        return false;
    if ((size_t) stride_pixels > SIZE_MAX / sizeof(uint16_t))
        return false;
    size_t row_bytes = (size_t) stride_pixels * sizeof(uint16_t);
    return (size_t) height <= capacity_bytes / row_bytes;
}

static inline bool swdec_audio_channels_admitted(int channels)
{
    return channels == 1 || channels == 2;
}

#endif
