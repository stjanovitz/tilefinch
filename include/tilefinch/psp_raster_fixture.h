#ifndef TILEFINCH_PSP_RASTER_FIXTURE_H
#define TILEFINCH_PSP_RASTER_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"

typedef struct {
    uint64_t frame_hash;
    size_t rounded_interior_pixels;
    size_t rounded_edge_pixels;
    size_t stroke_interior_pixels;
    size_t stroke_edge_pixels;
    size_t fallback_ink_pixels;
    size_t fallback_authored_color_pixels;
    size_t italic_continuous_pairs;
    int italic_maximum_centroid_step;
} PspRasterFixtureReport;

bool psp_raster_fixture_run(
    BrowserEngine *engine, PspRasterFixtureReport *report,
    char *error, size_t error_size);
/* Independent, network/decoder-free validation of the native queue input. */
bool psp_queue_shortcut_qualify(void);
bool psp_queue_menu_profile(Budget *budget);
bool psp_raster_fixture_rerender(
    BrowserEngine *engine, PspRasterFixtureReport *report,
    char *error, size_t error_size);

#endif
