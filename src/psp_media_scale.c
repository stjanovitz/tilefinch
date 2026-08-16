#include "tilefinch/psp_media_scale.h"
#include "tilefinch/pixel_math.h"

#include <string.h>

/*
 * RGBA8888 (little-endian byte order R,G,B,A) to RGB565.
 *
 * One aligned word load replaces the three byte loads the previous presenter
 * issued per pixel, and the alpha byte is never touched.
 */
static inline uint32_t psp_media_scale_565_portable(uint32_t pixel)
{
    return tilefinch_rgb565_pack_u8(
        pixel & 0xffu, (pixel >> 8) & 0xffu, (pixel >> 16) & 0xffu);
}

/*
 * -march=allegrex answers __mips 2 -- the base ISA really is MIPS II -- while
 * still assembling the core's r2 bitfield instructions, so the ISA-revision
 * macro is the wrong gate here and the architecture macro is the right one.
 */
#if defined(_MIPS_ARCH_ALLEGREX)
#define PSP_MEDIA_SCALE_BITFIELD_OPS 1
/*
 * Allegrex assembles the r2 bitfield ops, so the three colour fields are one
 * ext each and the assembly is two ins. psp-gcc will not choose those
 * instructions from the
 * shift/mask expression above -- measured on the shipping toolchain it emits
 * seven ALU operations per pixel where five suffice, and another two to pair
 * the halves of a 32-bit store where one suffices. That is 26 operations per
 * two pixels against 21, on the loop that costs the most main-CPU time in a
 * media session, so the instructions are named here instead.
 *
 * psp_media_scale_self_check() proves this agrees with the portable form on
 * the device it runs on; the host tests prove the portable form is right.
 */
static inline uint32_t psp_media_scale_565(uint32_t pixel)
{
    uint32_t red, green, blue;
    __asm__("ext %0,%1,3,5" : "=r" (red) : "r" (pixel));
    __asm__("ext %0,%1,10,6" : "=r" (green) : "r" (pixel));
    __asm__("ext %0,%1,19,5" : "=r" (blue) : "r" (pixel));
    __asm__("ins %0,%1,5,6" : "+r" (red) : "r" (green));
    __asm__("ins %0,%1,11,5" : "+r" (red) : "r" (blue));
    return red;
}

static inline uint32_t psp_media_scale_pair(uint32_t low, uint32_t high)
{
    __asm__("ins %0,%1,16,16" : "+r" (low) : "r" (high));
    return low;
}
#else
static inline uint32_t psp_media_scale_565(uint32_t pixel)
{
    return psp_media_scale_565_portable(pixel);
}

static inline uint32_t psp_media_scale_pair(uint32_t low, uint32_t high)
{
    return low | (high << 16);
}
#endif

static inline bool psp_media_scale_pair_aligned(
    const uint16_t *destination, int destination_stride_pixels)
{
    /* Two destination pixels per 32-bit store needs word-aligned row starts,
       which needs an even row stride and an even left edge. */
    return ((uintptr_t) (const void *) destination % 4u) == 0
        && (destination_stride_pixels % 2) == 0;
}

uint16_t psp_media_scale_convert_pixel(uint32_t rgba)
{
    return (uint16_t) psp_media_scale_565(rgba);
}

bool psp_media_scale_map_matches(
    const PspMediaScaleMap *map, PspMediaScaleFormat format,
    int source_width, int source_height, int source_stride_pixels,
    int output_width, int output_height)
{
    return map != NULL
        && map->format == format
        && map->source_width == source_width
        && map->source_height == source_height
        && map->source_stride_pixels == source_stride_pixels
        && map->output_width == output_width
        && map->output_height == output_height;
}

bool psp_media_scale_map_build(
    PspMediaScaleMap *map, PspMediaScaleFormat format,
    int source_width, int source_height, int source_stride_pixels,
    int output_width, int output_height)
{
    if (map == NULL) return false;
    if (source_width <= 0 || source_height <= 0
        || output_width <= 0 || output_height <= 0
        || source_stride_pixels < source_width
        || output_width > PSP_MEDIA_SCALE_MAX_OUTPUT_WIDTH
        || output_height > PSP_MEDIA_SCALE_MAX_OUTPUT_HEIGHT
        || (format != PSP_MEDIA_SCALE_RGBA8888
            && format != PSP_MEDIA_SCALE_RGB565)) {
        /* Leave any previously built map intact but unmatched, so a caller
           that ignores the refusal cannot blit through half-built tables. */
        return false;
    }
    unsigned bytes_per_pixel =
        format == PSP_MEDIA_SCALE_RGBA8888 ? 4u : 2u;
    for (int x = 0; x < output_width; x++) {
        int column = (int) (((unsigned) x * (unsigned) source_width)
                            / (unsigned) output_width);
        if (column >= source_width) column = source_width - 1;
        map->column_offset[x] = (uint32_t) column * bytes_per_pixel;
    }
    for (int y = 0; y < output_height; y++) {
        int row = (int) (((unsigned) y * (unsigned) source_height)
                         / (unsigned) output_height);
        if (row >= source_height) row = source_height - 1;
        map->row_index[y] = (uint16_t) row;
        map->row_repeats[y] = y > 0 && map->row_index[y - 1] == row;
    }
    map->format = format;
    map->source_width = source_width;
    map->source_height = source_height;
    map->source_stride_pixels = source_stride_pixels;
    map->output_width = output_width;
    map->output_height = output_height;
    map->identity_columns = output_width == source_width;
    return true;
}

void psp_media_scale_blit_reference(
    const PspMediaScaleMap *map, const void *source,
    uint16_t *destination, int destination_stride_pixels)
{
    if (map == NULL || source == NULL || destination == NULL) return;
    for (int y = 0; y < map->output_height; y++) {
        const unsigned char *row = (const unsigned char *) source
            + (size_t) map->row_index[y]
                * (size_t) map->source_stride_pixels
                * (map->format == PSP_MEDIA_SCALE_RGBA8888 ? 4u : 2u);
        uint16_t *out = destination
            + (size_t) y * (size_t) destination_stride_pixels;
        for (int x = 0; x < map->output_width; x++) {
            const unsigned char *pixel = row + map->column_offset[x];
            if (map->format == PSP_MEDIA_SCALE_RGBA8888) {
                uint32_t word;
                memcpy(&word, pixel, sizeof(word));
                out[x] = (uint16_t) psp_media_scale_565_portable(word);
            } else {
                uint16_t word;
                memcpy(&word, pixel, sizeof(word));
                out[x] = word;
            }
        }
    }
}

static void psp_media_scale_row_rgba(
    uint16_t *out, const uint32_t *row, const uint32_t *column_offset,
    int output_width, bool identity_columns, bool pair_aligned)
{
    if (!pair_aligned) {
        if (identity_columns) {
            for (int x = 0; x < output_width; x++)
                out[x] = (uint16_t) psp_media_scale_565(row[x]);
            return;
        }
        const unsigned char *base = (const unsigned char *) row;
        for (int x = 0; x < output_width; x++) {
            const uint32_t *pixel =
                (const uint32_t *) (const void *) (base + column_offset[x]);
            out[x] = (uint16_t) psp_media_scale_565(*pixel);
        }
        return;
    }
    uint32_t *pair = (uint32_t *) (void *) out;
    int x = 0;
    if (identity_columns) {
        /* A 1:1 column mapping is the common case for a 480-wide stream and
           for every seek preview: walk the source instead of gathering. */
        for (; x + 1 < output_width; x += 2) {
            uint32_t low = psp_media_scale_565(row[x]);
            uint32_t high = psp_media_scale_565(row[x + 1]);
            *pair++ = psp_media_scale_pair(low, high);
        }
    } else {
        const unsigned char *base = (const unsigned char *) row;
        for (; x + 1 < output_width; x += 2) {
            const uint32_t *first =
                (const uint32_t *) (const void *) (base + column_offset[x]);
            const uint32_t *second = (const uint32_t *) (const void *)
                (base + column_offset[x + 1]);
            uint32_t low = psp_media_scale_565(*first);
            uint32_t high = psp_media_scale_565(*second);
            *pair++ = psp_media_scale_pair(low, high);
        }
    }
    if (x < output_width) {
        const unsigned char *base = (const unsigned char *) row;
        const uint32_t *pixel = identity_columns
            ? row + x
            : (const uint32_t *) (const void *) (base + column_offset[x]);
        out[x] = (uint16_t) psp_media_scale_565(*pixel);
    }
}

static void psp_media_scale_row_rgb565(
    uint16_t *out, const uint16_t *row, const uint32_t *column_offset,
    int output_width, bool identity_columns)
{
    if (identity_columns) {
        memcpy(out, row, (size_t) output_width * sizeof(*out));
        return;
    }
    /* Offsets are whole pixels of a 16-bit format and the row base is a
       16-bit array, so the cast never widens the alignment requirement --
       which matters on Allegrex, where unaligned loads are a trap. */
    const unsigned char *base = (const unsigned char *) row;
    for (int x = 0; x < output_width; x++) {
        out[x] = *(const uint16_t *) (const void *)
            (base + column_offset[x]);
    }
}

/*
 * Prove the fast path and the reference agree on the machine actually running
 * them. The presenter's conversion is named in assembly on Allegrex, and a
 * toolchain that mis-assembled it would otherwise show up as discoloured
 * video nobody can reproduce on the host. Sixteen source pixels through both
 * paths, including the channel extremes, costs microseconds once per stream.
 */
bool psp_media_scale_self_check(void)
{
    static const uint32_t probes[16] = {
        UINT32_C(0x00000000), UINT32_C(0xFFFFFFFF), UINT32_C(0x000000FF),
        UINT32_C(0x0000FF00), UINT32_C(0x00FF0000), UINT32_C(0xFF000000),
        UINT32_C(0x00010203), UINT32_C(0x00FEFDFC), UINT32_C(0x0080402A),
        UINT32_C(0x00F81C07), UINT32_C(0x0007E0F8), UINT32_C(0x00123456),
        UINT32_C(0x00789ABC), UINT32_C(0x00DEF012), UINT32_C(0x005A5A5A),
        UINT32_C(0x00A5A5A5)
    };
    uint16_t fast[16];
    uint16_t reference[16];
    for (unsigned i = 0; i < 16u; i++) {
        fast[i] = (uint16_t) psp_media_scale_565(probes[i]);
        reference[i] = (uint16_t) psp_media_scale_565_portable(probes[i]);
    }
    if (memcmp(fast, reference, sizeof(fast)) != 0) return false;
    for (unsigned i = 0; i + 1u < 16u; i += 2u) {
        uint32_t paired = psp_media_scale_pair(fast[i], fast[i + 1u]);
        if (paired != ((uint32_t) fast[i]
                       | ((uint32_t) fast[i + 1u] << 16))) return false;
    }
    return true;
}

void psp_media_scale_blit(
    const PspMediaScaleMap *map, const void *source,
    uint16_t *destination, int destination_stride_pixels)
{
    if (map == NULL || source == NULL || destination == NULL
        || map->output_width <= 0 || map->output_height <= 0
        || destination_stride_pixels < map->output_width) return;
    bool rgba = map->format == PSP_MEDIA_SCALE_RGBA8888;
    size_t source_row_bytes = (size_t) map->source_stride_pixels
        * (rgba ? 4u : 2u);
    size_t output_row_bytes = (size_t) map->output_width * sizeof(*destination);
    bool pair_aligned =
        psp_media_scale_pair_aligned(destination, destination_stride_pixels);
    uint16_t *out = destination;
    for (int y = 0; y < map->output_height; y++) {
        if (map->row_repeats[y]) {
            /*
             * Upscaling vertically makes some destination rows identical to
             * the one above. Copying 960 published bytes costs a fraction of
             * converting 480 source pixels again, and the result is the same
             * image the reference produces.
             */
            memcpy(out, out - destination_stride_pixels, output_row_bytes);
        } else if (rgba) {
            const uint32_t *row = (const uint32_t *) source
                + (size_t) map->row_index[y] * map->source_stride_pixels;
            psp_media_scale_row_rgba(
                out, row, map->column_offset, map->output_width,
                map->identity_columns, pair_aligned);
        } else {
            const unsigned char *row = (const unsigned char *) source
                + (size_t) map->row_index[y] * source_row_bytes;
            psp_media_scale_row_rgb565(
                out, (const uint16_t *) (const void *) row,
                map->column_offset, map->output_width,
                map->identity_columns);
        }
        out += destination_stride_pixels;
    }
}
