/*
 * Coverage for the video presentation scaler.
 *
 * The defect this exists for: presenting a decoded frame cost 10.1 ms of the
 * main CPU per frame on a PSP-3000 (cycle E3, scale-total 6643464us over 655
 * frames), because every destination pixel paid an index lookup, three byte
 * loads and a 16-bit store. The fast path replaces that with per-geometry
 * tables, one aligned word load, and a 32-bit store per pixel pair -- and on
 * Allegrex it names the bitfield instructions itself.
 *
 * So the tests pin two things. First, byte-exact agreement with the
 * straightforward reference for every path the presenter can take: gathered
 * and 1:1 columns, upscaled and downscaled rows, both pixel formats, aligned
 * and odd destinations. Second, the table refusals, so a stream geometry the
 * display cannot hold cannot be blitted through a stale or half-built map.
 *
 * The timing block at the end is a lower bound on the algorithm's shape, not
 * a device number: the host has a different cache, a different issue width
 * and a hundred times the clock. Only the device cycle measures the budget.
 */

#include "tilefinch/psp_media_scale.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/* The 360p surface policy's stride and row count, which covers the 240p one
   the device cycle actually plays as well. */
#define SOURCE_STRIDE 768
#define SOURCE_ROWS 368
#define DESTINATION_STRIDE 512
#define DESTINATION_ROWS 272

static uint32_t source_rgba[SOURCE_STRIDE * SOURCE_ROWS];
static uint16_t source_rgb565[SOURCE_STRIDE * SOURCE_ROWS];
static uint16_t fast[DESTINATION_STRIDE * DESTINATION_ROWS];
static uint16_t reference[DESTINATION_STRIDE * DESTINATION_ROWS];
static PspMediaScaleMap map;

static void fill_source(void)
{
    uint32_t state = 0x2463534Du;
    for (size_t i = 0; i < SOURCE_STRIDE * SOURCE_ROWS; i++) {
        state = state * 1664525u + 1013904223u;
        source_rgba[i] = state & 0x00FFFFFFu;
        source_rgb565[i] = (uint16_t) (state >> 11);
    }
}

static bool compare_geometry(
    PspMediaScaleFormat format, int source_width, int source_height,
    int output_width, int output_height, int left)
{
    const void *source = format == PSP_MEDIA_SCALE_RGBA8888
        ? (const void *) source_rgba : (const void *) source_rgb565;
    CHECK(psp_media_scale_map_build(
        &map, format, source_width, source_height, SOURCE_STRIDE,
        output_width, output_height));
    CHECK(psp_media_scale_map_matches(
        &map, format, source_width, source_height, SOURCE_STRIDE,
        output_width, output_height));
    memset(fast, 0xA5, sizeof(fast));
    memset(reference, 0x5A, sizeof(reference));
    psp_media_scale_blit(
        &map, source, fast + left, DESTINATION_STRIDE);
    psp_media_scale_blit_reference(
        &map, source, reference + left, DESTINATION_STRIDE);
    for (int y = 0; y < output_height; y++) {
        const uint16_t *produced = fast + left + y * DESTINATION_STRIDE;
        const uint16_t *expected = reference + left + y * DESTINATION_STRIDE;
        if (memcmp(produced, expected,
                   (size_t) output_width * sizeof(*produced)) != 0) {
            for (int x = 0; x < output_width; x++) {
                if (produced[x] != expected[x]) {
                    fprintf(stderr,
                            "FAIL %dx%d -> %dx%d left=%d format=%d: "
                            "row %d column %d produced 0x%04X expected "
                            "0x%04X\n",
                            source_width, source_height, output_width,
                            output_height, left, (int) format, y, x,
                            produced[x], expected[x]);
                    return false;
                }
            }
        }
    }
    /* Nothing outside the display rectangle may be touched: the presenter
       clears the letterbox itself and composites chrome over the rest. */
    for (int y = 0; y < output_height; y++) {
        const uint16_t *produced = fast + y * DESTINATION_STRIDE;
        for (int x = 0; x < left; x++) CHECK(produced[x] == 0xA5A5u);
        for (int x = left + output_width; x < DESTINATION_STRIDE; x++)
            CHECK(produced[x] == 0xA5A5u);
    }
    for (int y = output_height; y < DESTINATION_ROWS; y++) {
        const uint16_t *produced = fast + y * DESTINATION_STRIDE;
        for (int x = 0; x < DESTINATION_STRIDE; x++)
            CHECK(produced[x] == 0xA5A5u);
    }
    return true;
}

static bool test_conversion_self_check(void)
{
    CHECK(psp_media_scale_self_check());
    return true;
}

static bool test_matches_reference(void)
{
    /* The stream the device cycle plays: 426x240 coded 432x240, presented to
       the full panel width. Both axes upscale, so the map gathers columns and
       repeats rows. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 426, 240, 480, 270, 0));
    /* The ceiling this pass is scoped to: 480x272 arrives 1:1. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 480, 272, 480, 272, 0));
    /* 360p downscaled into the panel: columns and rows both decimate. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 640, 360, 480, 270, 0));
    /* A pillarboxed 4:3 stream lands at an odd left edge, which refuses the
       paired 32-bit store and must still produce identical pixels. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 320, 240, 363, 272, 58));
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 320, 240, 362, 272, 59));
    /* Odd output widths exercise the trailing single-pixel tail. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 426, 240, 479, 269, 0));
    /* The host fixture backend publishes RGB565 instead. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGB565, 426, 240, 480, 270, 0));
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGB565, 480, 272, 480, 272, 0));
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGB565, 320, 240, 363, 272, 58));
    /* Degenerate but legal: one pixel wide, and a source narrower than its
       stride by the full macroblock padding. */
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 1, 1, 480, 272, 0));
    CHECK(compare_geometry(PSP_MEDIA_SCALE_RGBA8888, 17, 3, 480, 272, 0));
    return true;
}

static bool test_identity_columns_flagged(void)
{
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 480, 272, SOURCE_STRIDE, 480, 272));
    CHECK(map.identity_columns);
    CHECK(!map.row_repeats[0]);
    for (int y = 1; y < 272; y++) CHECK(!map.row_repeats[y]);
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    CHECK(!map.identity_columns);
    int repeats = 0;
    for (int y = 0; y < 270; y++) if (map.row_repeats[y]) repeats++;
    /* 240 source rows across 270 destination rows: thirty rows are copies. */
    CHECK(repeats == 30);
    return true;
}

static bool test_refuses_impossible_geometry(void)
{
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    /* Wider than the panel, taller than the panel, a stride that does not
       cover the source, and non-positive extents are all refused -- and the
       refusal leaves the previous map in place but unmatched, so a caller
       that ignores the return value cannot blit through half-built tables. */
    CHECK(!psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 481, 270));
    CHECK(!psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 273));
    CHECK(!psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, 425, 480, 270));
    CHECK(!psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 0, 240, SOURCE_STRIDE, 480, 270));
    CHECK(!psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 0));
    CHECK(!psp_media_scale_map_build(
        &map, (PspMediaScaleFormat) 7, 426, 240, SOURCE_STRIDE, 480, 270));
    CHECK(psp_media_scale_map_matches(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    CHECK(!psp_media_scale_map_matches(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 271));
    CHECK(!psp_media_scale_map_matches(
        &map, PSP_MEDIA_SCALE_RGB565, 426, 240, SOURCE_STRIDE, 480, 270));
    return true;
}

static bool test_samples_stay_inside_the_source(void)
{
    /* Every table entry has to address a pixel the decoder actually wrote:
       the last destination column of a 1:1 map is the last source column, and
       nothing ever indexes past it. */
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    for (int x = 0; x < 480; x++) CHECK(map.column_offset[x] <= 425u * 4u);
    CHECK(map.column_offset[479] == 425u * 4u);
    for (int y = 0; y < 270; y++) CHECK(map.row_index[y] <= 239u);
    CHECK(map.row_index[269] == 239u);
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGB565, 426, 240, SOURCE_STRIDE, 480, 270));
    for (int x = 0; x < 480; x++) CHECK(map.column_offset[x] <= 425u * 2u);
    return true;
}

static bool test_null_arguments_are_inert(void)
{
    memset(fast, 0xA5, sizeof(fast));
    CHECK(!psp_media_scale_map_build(
        NULL, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    CHECK(!psp_media_scale_map_matches(
        NULL, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    CHECK(psp_media_scale_map_build(
        &map, PSP_MEDIA_SCALE_RGBA8888, 426, 240, SOURCE_STRIDE, 480, 270));
    psp_media_scale_blit(&map, NULL, fast, DESTINATION_STRIDE);
    psp_media_scale_blit(&map, source_rgba, NULL, DESTINATION_STRIDE);
    psp_media_scale_blit(NULL, source_rgba, fast, DESTINATION_STRIDE);
    /* A destination stride narrower than the output would run off the row. */
    psp_media_scale_blit(&map, source_rgba, fast, 479);
    for (size_t i = 0; i < sizeof(fast) / sizeof(*fast); i++)
        CHECK(fast[i] == 0xA5A5u);
    return true;
}

static double seconds_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double) now.tv_sec + (double) now.tv_nsec / 1e9;
}

/*
 * The loop this change replaced, reproduced for the timing comparison only:
 * a uint16 column index per destination pixel and three byte loads per
 * source pixel. It is not the correctness oracle -- blit_reference is -- but
 * it is what the 10.1 ms device measurement was taken on.
 */
static uint16_t legacy_column_map[PSP_MEDIA_SCALE_MAX_OUTPUT_WIDTH];
static uint16_t legacy_row_map[PSP_MEDIA_SCALE_MAX_OUTPUT_HEIGHT];

static void legacy_blit(const unsigned char *source, uint16_t *destination,
                        int source_height, int source_stride_pixels,
                        int output_width, int output_height,
                        int destination_stride_pixels)
{
    (void) source_height;
    for (int y = 0; y < output_height; y++) {
        int source_y = legacy_row_map[y];
        uint16_t *out = destination
            + (size_t) y * (size_t) destination_stride_pixels;
        const unsigned char *row = source
            + (size_t) source_y * (size_t) source_stride_pixels * 4u;
        for (int x = 0; x < output_width; x++) {
            const unsigned char *pixel =
                row + (size_t) legacy_column_map[x] * 4u;
            out[x] = (uint16_t) ((((uint16_t) pixel[0] >> 3) << 11)
                | (((uint16_t) pixel[1] >> 2) << 5)
                | ((uint16_t) pixel[2] >> 3));
        }
    }
}

static void report_timing(const char *label, PspMediaScaleFormat format,
                          int source_width, int source_height,
                          int output_width, int output_height)
{
    const void *source = format == PSP_MEDIA_SCALE_RGBA8888
        ? (const void *) source_rgba : (const void *) source_rgb565;
    if (!psp_media_scale_map_build(
            &map, format, source_width, source_height, SOURCE_STRIDE,
            output_width, output_height)) return;
    const int iterations = 200;
    /* Warm the tables and the destination into cache first: the number that
       matters is the steady-state loop, not the first miss storm. */
    psp_media_scale_blit(&map, source, fast, DESTINATION_STRIDE);
    psp_media_scale_blit_reference(&map, source, reference,
                                   DESTINATION_STRIDE);
    double started = seconds_now();
    for (int i = 0; i < iterations; i++)
        psp_media_scale_blit(&map, source, fast, DESTINATION_STRIDE);
    double fast_us = (seconds_now() - started) * 1e6 / iterations;
    double legacy_us = 0.0;
    if (format == PSP_MEDIA_SCALE_RGBA8888) {
        for (int x = 0; x < output_width; x++)
            legacy_column_map[x] =
                (uint16_t) (x * source_width / output_width);
        for (int y = 0; y < output_height; y++)
            legacy_row_map[y] =
                (uint16_t) (y * source_height / output_height);
        legacy_blit((const unsigned char *) source, reference, source_height,
                    SOURCE_STRIDE, output_width, output_height,
                    DESTINATION_STRIDE);
        started = seconds_now();
        for (int i = 0; i < iterations; i++)
            legacy_blit((const unsigned char *) source, reference,
                        source_height, SOURCE_STRIDE, output_width,
                        output_height, DESTINATION_STRIDE);
        legacy_us = (seconds_now() - started) * 1e6 / iterations;
    }
    printf("psp-media-scale-bench: case=%s pixels=%d fast=%.1fus "
           "legacy=%.1fus ratio=%.2f\n",
           label, output_width * output_height, fast_us, legacy_us,
           fast_us <= 0.0 ? 0.0 : legacy_us / fast_us);
}

int main(void)
{
    fill_source();
    struct { const char *name; bool (*run)(void); } cases[] = {
        {"conversion-self-check", test_conversion_self_check},
        {"matches-reference", test_matches_reference},
        {"identity-columns-flagged", test_identity_columns_flagged},
        {"refuses-impossible-geometry", test_refuses_impossible_geometry},
        {"samples-stay-inside-the-source", test_samples_stay_inside_the_source},
        {"null-arguments-are-inert", test_null_arguments_are_inert}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!cases[i].run()) {
            fprintf(stderr, "FAIL %s\n", cases[i].name);
            return EXIT_FAILURE;
        }
        printf("ok %s\n", cases[i].name);
    }
    report_timing("426x240-to-480x270-rgba", PSP_MEDIA_SCALE_RGBA8888,
                  426, 240, 480, 270);
    report_timing("480x272-identity-rgba", PSP_MEDIA_SCALE_RGBA8888,
                  480, 272, 480, 272);
    report_timing("640x360-to-480x270-rgba", PSP_MEDIA_SCALE_RGBA8888,
                  640, 360, 480, 270);
    return EXIT_SUCCESS;
}
