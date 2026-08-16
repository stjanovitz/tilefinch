#ifndef TILEFINCH_PIXEL_MATH_H
#define TILEFINCH_PIXEL_MATH_H

#include <stdint.h>

/*
 * A uint16_t pixel is an implementation detail, not a portable RGB565 wire
 * value.  Host fixtures keep the conventional RRRRRGGGGGGBBBBB integer
 * layout used by the fidelity tools.  PSP GU/display 5650 is R5:G6:B5 from
 * the least-significant bit upward, so device builds use BBBBBGGGGGGRRRRR.
 * Packing in the rasterizer avoids a 480x272 channel-swap pass at present.
 */
#if defined(__PSP__)
#define TILEFINCH_RGB565_RED_SHIFT 0u
#define TILEFINCH_RGB565_BLUE_SHIFT 11u
#else
#define TILEFINCH_RGB565_RED_SHIFT 11u
#define TILEFINCH_RGB565_BLUE_SHIFT 0u
#endif

#define TILEFINCH_RGB565_PACK_CODES_CONST(red, green, blue) \
    ((uint16_t) ((((uint16_t) (red) & 31u) \
                   << TILEFINCH_RGB565_RED_SHIFT) \
                 | (((uint16_t) (green) & 63u) << 5u) \
                 | (((uint16_t) (blue) & 31u) \
                    << TILEFINCH_RGB565_BLUE_SHIFT)))

#if defined(__PSP__)
_Static_assert(TILEFINCH_RGB565_PACK_CODES_CONST(31u, 0u, 0u) == 0x001fu,
               "PSP 5650 red must occupy the least-significant field");
_Static_assert(TILEFINCH_RGB565_PACK_CODES_CONST(0u, 0u, 31u) == 0xf800u,
               "PSP 5650 blue must occupy the most-significant field");
#else
_Static_assert(TILEFINCH_RGB565_PACK_CODES_CONST(31u, 0u, 0u) == 0xf800u,
               "host RGB565 red must retain the fidelity-tool layout");
#endif

static inline uint16_t tilefinch_rgb565_pack_codes(
    unsigned red, unsigned green, unsigned blue)
{
    return TILEFINCH_RGB565_PACK_CODES_CONST(red, green, blue);
}

static inline uint16_t tilefinch_rgb565_pack_u8(
    unsigned red, unsigned green, unsigned blue)
{
    return tilefinch_rgb565_pack_codes(red >> 3, green >> 2, blue >> 3);
}

static inline unsigned tilefinch_rgb565_red_code(uint16_t pixel)
{
    return (pixel >> TILEFINCH_RGB565_RED_SHIFT) & 31u;
}

static inline unsigned tilefinch_rgb565_green_code(uint16_t pixel)
{
    return (pixel >> 5) & 63u;
}

static inline unsigned tilefinch_rgb565_blue_code(uint16_t pixel)
{
    return (pixel >> TILEFINCH_RGB565_BLUE_SHIFT) & 31u;
}

/*
 * Exact integer identities used by the RGB565 rasterizer. Allegrex has a
 * long-latency integer divide, so keeping these visibly division-free matters
 * in the per-pixel path. The small multiply/shift forms are exhaustive exact
 * replacements for floor(code*255/max), not the common rounded bit-replication
 * approximation. The reciprocal identity is exact for every input through
 * 65535, covering an 8-bit source-over numerator plus rounding.
 */
static inline unsigned tilefinch_rgb5_to_u8(unsigned code)
{
    return ((code & 31u) * 1053u) >> 7;
}

static inline unsigned tilefinch_rgb6_to_u8(unsigned code)
{
    return ((code & 63u) * 259u + 3u) >> 6;
}

static inline unsigned tilefinch_div255_u16(uint32_t value)
{
    value++;
    return (unsigned) ((value + (value >> 8)) >> 8);
}

#endif
