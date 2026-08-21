#include "swdec_bounds.h"

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
    puts("swdec bounds tests passed");
    return 0;
}
