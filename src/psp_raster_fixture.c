#include "tilefinch/psp_raster_fixture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tilefinch/psp_ui.h"
#include "tilefinch/psp_input_route.h"
#include "tilefinch/psp_log.h"
#include <pspkernel.h>

static bool menu_profile_blit(void *opaque, const uint16_t *source,
    int width, int height, int stride, bool changed, uint32_t *destination,
    int destination_stride, int left, int top)
{
    (void) source; (void) width; (void) height; (void) stride;
    (void) destination; (void) destination_stride; (void) left; (void) top;
    if (changed) (*(unsigned *) opaque)++;
    return true; /* Isolate CPU menu preparation; GE submission is excluded. */
}

bool psp_queue_menu_profile(Budget *budget)
{
    if (budget == NULL) return false;
    enum { PIXELS = 480 * 272, MENU_PIXELS = 352 * 208, PHASES = 8, REPEATS = 3 };
    uint16_t *scratch = budget_malloc(budget, PIXELS * sizeof(uint16_t));
    uint16_t *menu = budget_malloc(budget, MENU_PIXELS * sizeof(uint16_t));
    uint32_t *picture = budget_malloc(budget, PIXELS * sizeof(uint32_t));
    if (!scratch || !menu || !picture) {
        budget_free(budget, scratch); budget_free(budget, menu); budget_free(budget, picture);
        return false;
    }
    memset(picture, 0, PIXELS * sizeof(uint32_t));
    PspUiMediaState media; PspUiMediaPresentation p = {0};
    psp_ui_media_init(&media); media.presentation = &p;
    psp_ui_media_set(&media, true, true, false, 15000000, 600000000, "Menu profile");
    p.queue_active = p.queue_next = p.track_menu_open = true;
    p.queue_count = 6; p.track_menu_tab = 3; p.track_menu_selection = 2;
    p.audio_track_count = p.subtitle_track_count = 6; p.chapter_count = MEDIA_CHAPTER_LIMIT;
    for (unsigned at = 0; at < 6; at++) {
        snprintf(p.queue_titles[at], 128, "Fixture video %u with a representative medium-length title", at);
        snprintf(p.audio_tracks[at].label, 48, "Audio track %u original language", at);
        snprintf(p.subtitle_tracks[at].label, 48, "Subtitle track %u original language", at);
    }
    for (unsigned at = 0; at < MEDIA_CHAPTER_LIMIT; at++) {
        p.chapters[at].seconds = at * 10;
        snprintf(p.chapters[at].title, sizeof(p.chapters[at].title),
            "Chapter %u with a representative medium-length title", at);
    }
    PspUiMediaTrackMenuCache cache = {0}; unsigned redraws = 0;
    psp_ui_media_composite_8888_cached(&media, NULL, picture, 480, 272, 480,
        scratch, menu, MENU_PIXELS, 1, &cache, menu_profile_blit, &redraws);
    static const char *const names[] = {"idle-cache", "focus", "auto-next", "tabs",
        "open-cold", "close", "cpu-copy", "raster"};
    struct { uint64_t wall, cpu, query; unsigned calls, redraws; } rows[PHASES * REPEATS];
    bool valid = true;
    for (unsigned phase = 0; phase < PHASES; phase++) {
        for (unsigned repeat = 0; repeat < REPEATS; repeat++) {
            p.track_menu_tab = 3; p.track_menu_open = true; p.track_menu_selection = 2;
            unsigned before_redraws = redraws;
            unsigned calls = phase == 0 || phase == 6 ? 128 : 64;
            SceKernelThreadRunStatus before = { .size = sizeof(before) }, after = { .size = sizeof(after) };
            uint64_t begin = sceKernelGetSystemTimeWide();
            int a = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &before);
            uint64_t before_end = sceKernelGetSystemTimeWide();
            for (unsigned at = 0; at < calls; at++) {
                PspUiInput input = {0};
                if (phase == 1) input.pressed = PSP_UI_BUTTON_DOWN;
                if (phase == 2) input.pressed = PSP_UI_BUTTON_RELOAD;
                if (phase == 3) input.pressed = PSP_UI_BUTTON_PAGE_DOWN;
                if (phase == 4) { p.track_menu_open = false; cache.valid = false; input.pressed = PSP_UI_BUTTON_TOOLBAR; }
                if (phase == 5) { p.track_menu_open = true; input.pressed = PSP_UI_BUTTON_CANCEL; }
                if (input.pressed) (void) psp_ui_media_update(&media, &input);
                if (phase == 7) psp_ui_media_raster_track_menu(&media, menu, 352, 208, 352);
                else psp_ui_media_composite_8888_cached(&media, NULL, picture, 480, 272, 480,
                    scratch, menu, MENU_PIXELS, 1, &cache, phase == 6 ? NULL : menu_profile_blit, &redraws);
            }
            uint64_t after_begin = sceKernelGetSystemTimeWide();
            int z = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &after);
            uint64_t end = sceKernelGetSystemTimeWide();
            unsigned row = phase * REPEATS + repeat;
            uint64_t cpu_before = ((uint64_t) before.runClocks.hi << 32) | before.runClocks.low;
            uint64_t cpu_after = ((uint64_t) after.runClocks.hi << 32) | after.runClocks.low;
            if (a < 0 || z < 0 || cpu_after < cpu_before) valid = false;
            rows[row].wall = end - begin; rows[row].cpu = cpu_after - cpu_before;
            rows[row].query = before_end - begin + end - after_begin;
            rows[row].calls = calls; rows[row].redraws = redraws - before_redraws;
            psp_log_heartbeat();
        }
    }
    for (unsigned row = 0; row < PHASES * REPEATS; row++)
        psp_log_printf("tilefinch-menu-profile: phase=%s repeat=%u calls=%u wall-us=%llu cpu-us=%llu cpu-valid=%u query-us=%llu redraws=%u\n",
            names[row / REPEATS], row % REPEATS, rows[row].calls,
            (unsigned long long) rows[row].wall, (unsigned long long) rows[row].cpu, rows[row].cpu != 0,
            (unsigned long long) rows[row].query, rows[row].redraws);
    budget_free(budget, picture); budget_free(budget, menu); budget_free(budget, scratch);
    return valid;
}

/* No decoder or network: exercise the shipping input receiver and busy
   routing on Allegrex, including the failed-player recovery boundary. */
bool psp_queue_shortcut_qualify(void)
{
    PspUiMediaState media; PspUiMediaPresentation p = {0};
    psp_ui_media_init(&media); media.presentation = &p;
    psp_ui_media_set(&media, true, true, false, 1000000, 60000000, "Queue qualification");
    p.queue_active = p.queue_next = p.track_menu_open = true;
    p.queue_count = 6; p.track_menu_tab = 3; p.track_menu_selection = 2;
    if (psp_ui_media_track_menu_height(&media) != 208) return false;
    PspUiInput square = {.pressed = PSP_UI_BUTTON_RELOAD};
    for (unsigned state = 0; state < 4; state++) {
        media.resolving = state == 1; media.buffering = state == 2;
        media.failed = state == 3; media.lower_quality_recovery_available = true;
        PspInputRouteContext busy = {.owner = PSP_INPUT_OWNER_MEDIA,
            .pressed = square.pressed, .media_menu_open = true, .acknowledge_busy = true};
        if (psp_input_route(&busy) != PSP_INPUT_ROUTE_MEDIA) return false;
        PspUiMediaIntent intent = psp_ui_media_update(&media, &square);
        if (intent.action != PSP_UI_MEDIA_ACTION_AUTOMATIC_NEXT
            || intent.track_index != (state % 2u == 0u)
            || !p.track_menu_open || p.track_menu_selection != 2
            || media.current_time_us != 1000000 || media.seek_preview_active) return false;
    }
    p.track_menu_tab = 0;
    if (psp_ui_media_update(&media, &square).action != PSP_UI_MEDIA_ACTION_NONE) return false;
    p.track_menu_open = false;
    return psp_ui_media_update(&media, &square).action == PSP_UI_MEDIA_ACTION_LOWER_QUALITY;
}

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
    "<div id=fallback>한 漢 あ Ж ộ 😀</div>"
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
