#include "tilefinch/psp_raster_fixture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char raster_fixture_html[] =
    "<!doctype html><meta name=viewport content='width=480,initial-scale=1'>"
    "<title>Raster qualification</title><style>"
    "html,body{margin:0;width:480px;height:272px;overflow:hidden;"
    "background:#101820;color:#fff}"
    "#italic{position:absolute;left:0;top:0;font:italic 32px/40px serif}"
    "#round{position:absolute;left:64px;top:6px;width:56px;height:40px;"
    "background:#18d080;border-radius:14px}"
    "#alpha{position:absolute;left:132px;top:6px;width:56px;height:40px;"
    "background:rgba(255,255,255,.5)}"
    /* Unfilled on purpose: the only ink inside this box comes from the
       stroke, so any pixel that is neither the border colour nor the page
       background is fractional stroke coverage and nothing else. */
    "#stroke{position:absolute;left:200px;top:6px;width:56px;height:40px;"
    "box-sizing:border-box;border:3px solid #f0a020;border-radius:14px}"
    "#fallback{position:absolute;left:8px;top:58px;font:20px/28px sans-serif}"
    "#baseline{position:absolute;left:8px;top:100px;font:24px/32px sans-serif}"
    "#baseline small{font-size:13px}"
    "</style><span id=italic>|</span><div id=round></div>"
    "<div id=alpha></div><div id=stroke></div>"
    "<div id=fallback>한 漢 あ 😀</div>"
    "<div id=baseline>Ag <small>small</small> Égj</div>";

static void raster_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static unsigned rgb565_weight(uint16_t pixel)
{
    return ((pixel >> 11) & 31u) * 2u
        + ((pixel >> 5) & 63u) + (pixel & 31u) * 2u;
}

static bool raster_fixture_analyze(
    BrowserEngine *engine, PspRasterFixtureReport *report,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (engine == NULL || report == NULL) {
        raster_error(error, error_size, "raster fixture request invalid");
        return false;
    }
    memset(report, 0, sizeof(*report));
    BrowserFrameView frame = {0};
    if (!browser_engine_frame_view(engine, &frame)
        || frame.pixels == NULL || frame.width != 480 || frame.height != 272
        || frame.stride < frame.width) {
        raster_error(
            error, error_size, "raster fixture geometry invalid %dx%d/%d",
            frame.width, frame.height, frame.stride);
        return false;
    }

    uint64_t hash = UINT64_C(1469598103934665603);
    for (int y = 0; y < frame.height; y++) {
        const uint16_t *row = frame.pixels + (size_t) y * frame.stride;
        for (int x = 0; x < frame.width; x++) {
            hash ^= row[x];
            hash *= UINT64_C(1099511628211);
        }
    }
    report->frame_hash = hash;
    uint16_t background = frame.pixels[(size_t) 250 * frame.stride + 470u];
    uint16_t rounded = frame.pixels[(size_t) 26 * frame.stride + 92u];
    uint16_t alpha = frame.pixels[(size_t) 26 * frame.stride + 160u];
    uint16_t stroke = frame.pixels[(size_t) 7 * frame.stride + 228u];
    if (rounded == background || alpha == background || alpha == rounded
        || stroke == background || stroke == rounded) {
        raster_error(error, error_size, "sentinel surface colors collapsed");
        return false;
    }
    for (int y = 6; y < 46; y++) {
        const uint16_t *row = frame.pixels + (size_t) y * frame.stride;
        for (int x = 64; x < 120; x++) {
            if (row[x] == rounded) report->rounded_interior_pixels++;
            else if (row[x] != background) report->rounded_edge_pixels++;
        }
        /* The stroke box carries no fill, so a pixel here is either the
           whole border colour, the untouched page, or a corner the stroke
           covers fractionally. The third bucket is the one that was empty
           while rounded strokes used a binary inside/outside predicate. */
        for (int x = 200; x < 256; x++) {
            if (row[x] == stroke) report->stroke_interior_pixels++;
            else if (row[x] != background) report->stroke_edge_pixels++;
        }
    }

    int previous_centroid = 0;
    bool have_previous = false;
    for (int y = 0; y < 42; y++) {
        uint64_t weighted_x = 0;
        uint64_t coverage = 0;
        const uint16_t *row = frame.pixels + (size_t) y * frame.stride;
        unsigned background_weight = rgb565_weight(background);
        for (int x = 0; x < 40; x++) {
            unsigned weight = rgb565_weight(row[x]);
            weight = weight > background_weight
                ? weight - background_weight : 0u;
            coverage += weight;
            weighted_x += (uint64_t) weight * (unsigned) x * 256u;
        }
        if (coverage == 0) {
            have_previous = false;
            continue;
        }
        int centroid = (int) (weighted_x / coverage);
        if (have_previous) {
            int step = centroid - previous_centroid;
            if (step < 0) step = -step;
            if (step > report->italic_maximum_centroid_step)
                report->italic_maximum_centroid_step = step;
            report->italic_continuous_pairs++;
        }
        previous_centroid = centroid;
        have_previous = true;
    }

    for (int y = 55; y < 94; y++) {
        const uint16_t *row = frame.pixels + (size_t) y * frame.stride;
        for (int x = 0; x < 180; x++) {
            uint16_t pixel = row[x];
            if (pixel != background) report->fallback_ink_pixels++;
            unsigned red = (pixel >> 11) & 31u;
            unsigned green = (pixel >> 5) & 63u;
            unsigned blue = pixel & 31u;
            /* The fixture's authored page ink is white over a dark blue
               background. A strongly yellow pixel cannot come from either
               endpoint or their alpha blend; it proves that an optional
               colour glyph reached the RGB565 raster unchanged. */
            if (red >= 20u && green >= 30u && blue <= 12u)
                report->fallback_authored_color_pixels++;
        }
    }
    if (report->rounded_interior_pixels < 1200u
        || report->rounded_edge_pixels < 12u
        || report->stroke_interior_pixels < 300u
        || report->stroke_edge_pixels < 24u
        || report->fallback_ink_pixels < 120u
        || report->italic_continuous_pairs < 8u
        || report->italic_maximum_centroid_step >= 192) {
        raster_error(
            error, error_size,
            "raster invariant failed round=%zu/%zu stroke=%zu/%zu "
            "fallback=%zu italic=%zu/%d",
            report->rounded_interior_pixels, report->rounded_edge_pixels,
            report->stroke_interior_pixels, report->stroke_edge_pixels,
            report->fallback_ink_pixels, report->italic_continuous_pairs,
            report->italic_maximum_centroid_step);
        return false;
    }
    return true;
}

bool psp_raster_fixture_run(
    BrowserEngine *engine, PspRasterFixtureReport *report,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (engine == NULL || report == NULL
        || !browser_engine_commit_html(
            engine, "https://tilefinch.local/raster-fixture",
            raster_fixture_html, sizeof(raster_fixture_html) - 1u, false)
        || !browser_engine_refresh_shell(engine)
        || !browser_engine_render_frame(engine, NULL)) {
        raster_error(
            error, error_size, "raster fixture render failed: %s",
            engine == NULL ? "invalid engine" : browser_engine_last_error(engine));
        return false;
    }
    return raster_fixture_analyze(engine, report, error, error_size);
}

bool psp_raster_fixture_rerender(
    BrowserEngine *engine, PspRasterFixtureReport *report,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (engine == NULL || report == NULL
        || !browser_engine_render_frame(engine, NULL)) {
        raster_error(
            error, error_size, "raster fixture rerender failed: %s",
            engine == NULL ? "invalid engine" : browser_engine_last_error(engine));
        return false;
    }
    return raster_fixture_analyze(engine, report, error, error_size);
}
