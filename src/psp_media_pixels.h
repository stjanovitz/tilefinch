#ifndef TILEFINCH_PSP_MEDIA_PIXELS_H
#define TILEFINCH_PSP_MEDIA_PIXELS_H

#include <stdint.h>
#include "tilefinch/pixel_math.h"

static inline uint16_t psp_media_rgba565(const unsigned char *pixel)
{
    return tilefinch_rgb565_pack_u8(pixel[0], pixel[1], pixel[2]);
}

#endif
