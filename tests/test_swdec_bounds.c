#include "swdec_bounds.h"
#include "swdec_csc_mask.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    puts("test: decoded video geometry remains inside admitted surfaces");
    CHECK(swdec_dimensions_admitted(432, 240, 432, 240));
    CHECK(swdec_dimensions_admitted(320, 180, 432, 240));
    CHECK(!swdec_dimensions_admitted(448, 240, 432, 240));
    CHECK(!swdec_dimensions_admitted(432, 256, 432, 240));
    CHECK(!swdec_dimensions_admitted(0, 240, 432, 240));

    puts("test: RGB565 capacity includes stride and every decoded row");
    CHECK(swdec_rgb565_destination_fits(
        432, 240, 432, 432u * 240u * 2u));
    CHECK(swdec_rgb565_destination_fits(
        426, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        448, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        431, 240, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        432, 256, 432, 432u * 240u * 2u));
    CHECK(!swdec_rgb565_destination_fits(
        432, 240, 432, 432u * 240u * 2u - 1u));

    puts("test: the fixed two-plane AAC ABI admits mono and stereo only");
    CHECK(swdec_audio_channels_admitted(1));
    CHECK(swdec_audio_channels_admitted(2));
    CHECK(!swdec_audio_channels_admitted(0));
    CHECK(!swdec_audio_channels_admitted(3));
    CHECK(!swdec_audio_channels_admitted(8));

    puts("test: clock correction re-anchors instead of accumulating");
    CHECK(swdec_clock_reanchor_slip(1000000u, 925000u) == 75000u);
    CHECK(swdec_clock_reanchor_slip(1000000u, 925000u) == 75000u);
    CHECK(swdec_clock_reanchor_slip(900000u, 925000u) == 0u);

    puts("test: CSC completion covers the bottom of a 272-row frame");
    SwdecCscMask mask = {{0, 0}};
    for (unsigned unit = 0; unit < 34u; unit++)
        swdec_csc_mask_set(&mask, unit);
    CHECK(swdec_csc_mask_test(&mask, 31u));
    CHECK(swdec_csc_mask_test(&mask, 32u)); /* rows 256..263 */
    CHECK(swdec_csc_mask_test(&mask, 33u)); /* rows 264..271 */
    CHECK(!swdec_csc_mask_test(&mask, SWDEC_CSC_MASK_UNITS));
    swdec_csc_mask_clear(&mask);
    CHECK(!swdec_csc_mask_test(&mask, 32u));
    puts("swdec bounds tests passed");
    return 0;
}
