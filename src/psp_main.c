/* Minimal PSP frontend for the target qualification fixture
   (docs/engineering/DEVICE_QUALIFICATION.md).

   Renders the embedded fixture page through the standard engine pipeline
   (parse -> viewport -> stylesheet -> layout -> tile cache), mirrors the
   host lab's --scroll-all pass so the deterministic counters are
   comparable against tests/counter-baselines.tsv, prints those counters
   to stdout (captured by PPSSPP headless), and then serves d-pad
   scrolling until HOME/START exits.

   Deliberately not here yet: JavaScript, navigation, networking (the
   null transport reports failure), and web fonts. */

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/font.h"
#include "tilefinch/layout.h"
#include "tilefinch/platform.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/render.h"
#include "tilefinch/style.h"
#include "tilefinch/viewport.h"

#include "psp_fixture_html.h"

PSP_MODULE_INFO("Tilefinch Fixture", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define PSP_SCREEN_WIDTH 480
#define PSP_SCREEN_HEIGHT 272
#define PSP_VRAM_STRIDE 512
#define MIB (1024u * 1024u)
#define KIB 1024u

/* Mirrors the host lab defaults so counters stay comparable: the budget
   is an accounting ceiling, not a reservation, and the fixture peaks
   near 3.5 MiB. */
#define PSP_ENGINE_BUDGET_BYTES (48u * MIB)
#define PSP_TILE_CAPACITY 8
#define PSP_MAX_FONT_BYTES (1536u * KIB)
#define PSP_PARSE_DEPTH_LIMIT 512
#define PSP_FALLBACK_VIEWPORT_WIDTH 980

static int psp_exit_callback(int arg1, int arg2, void *common)
{
    (void) arg1; (void) arg2; (void) common;
    sceKernelExitGame();
    return 0;
}

static int psp_callback_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    int callback_id = sceKernelCreateCallback("exit", psp_exit_callback,
                                              NULL);
    if (callback_id >= 0) sceKernelRegisterExitCallback(callback_id);
    sceKernelSleepThreadCB();
    return 0;
}

static void psp_setup_callbacks(void)
{
    int thread_id = sceKernelCreateThread(
        "callbacks", psp_callback_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK, 0xFA0, 0, NULL);
    if (thread_id >= 0) sceKernelStartThread(thread_id, 0, NULL);
}

static uint64_t psp_time_ns(void *context)
{
    (void) context;
    return (uint64_t) sceKernelGetSystemTimeWide() * 1000u;
}

static uint64_t psp_time_us(void *context)
{
    (void) context;
    return (uint64_t) sceKernelGetSystemTimeWide();
}

static uint64_t psp_wall_time_ns(void *context)
{
    (void) context;
    time_t seconds = time(NULL);
    return seconds <= 0 ? 0
        : (uint64_t) seconds * UINT64_C(1000000000);
}

static void psp_log_message(void *context, const char *message)
{
    (void) context;
    printf("engine: %s\n", message == NULL ? "" : message);
}

static uint16_t psp_color_rgb565(uint32_t color)
{
    return tilefinch_rgb565_pack_u8(
        (color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu);
}

static bool psp_frame_is_blank(const uint16_t *frame, size_t pixels,
                               uint16_t background)
{
    for (size_t i = 0; i < pixels; i++) {
        if (frame[i] != background) return false;
    }
    return true;
}

/* Scanout is owned by the shared front end so the fixture exercises exactly
   the path the browser ships (include/tilefinch/psp_display.h). */
static PspDisplay psp_display;

static void psp_present(const uint16_t *frame)
{
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (vram == NULL) return;
    for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
        memcpy(vram + (size_t) y * PSP_VRAM_STRIDE,
               frame + (size_t) y * PSP_SCREEN_WIDTH,
               PSP_SCREEN_WIDTH * sizeof(*frame));
    }
    (void) psp_display_publish(&psp_display);
}

/* Font files ship beside the EBOOT; derive their directory from argv[0]
   (for example ms0:/PSP/GAME/tilefinch/EBOOT.PBP). */
static void psp_font_path(char *output, size_t size, const char *argv0,
                          const char *name)
{
    const char *slash = argv0 == NULL ? NULL : strrchr(argv0, '/');
    if (slash == NULL) {
        snprintf(output, size, "fonts/%s", name);
        return;
    }
    snprintf(output, size, "%.*s/fonts/%s",
             (int) (slash - argv0), argv0, name);
}

static void psp_counter(const char *group, const char *name,
                        unsigned long long value)
{
    printf("psp-counter\tfloat-article\t%s.%s\t%llu\n", group, name, value);
}

int main(int argc, char *argv[])
{
    psp_setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* Claim the display mode before the first scanout rather than inheriting
       whatever the launcher left behind. */
    bool display_ready =
        psp_display_begin(&psp_display, psp_display_system_backend());
    printf("tilefinch-psp: display ready=%d mode-error=0x%08x\n",
           display_ready ? 1 : 0, (unsigned) psp_display.mode_error);

    static const TilefinchPlatformServices services = {
        .wall_time_ns = psp_wall_time_ns,
        .monotonic_time_ns = psp_time_ns,
        .monotonic_time_us = psp_time_us,
        .log_message = psp_log_message,
    };
    tilefinch_platform_set_services(&services);

    printf("tilefinch-psp: boot\n");

    Budget budget;
    budget_init(&budget, PSP_ENGINE_BUDGET_BYTES);
    if (!budget_install_lexbor(&budget)) {
        printf("tilefinch-psp: lexbor allocator install failed\n");
        goto sleep_forever;
    }

    uint16_t *frame = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RENDER,
        PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT * sizeof(*frame));
    if (frame == NULL) {
        printf("tilefinch-psp: framebuffer allocation failed\n");
        goto sleep_forever;
    }

    char sans[256], serif[256], sans_italic[256], sans_bold[256];
    char serif_bold[256], metric_sans[256], metric_sans_bold[256];
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    psp_font_path(sans, sizeof(sans), argv0, "DejaVuSans.ttf");
    psp_font_path(serif, sizeof(serif), argv0, "DejaVuSerif.ttf");
    psp_font_path(sans_italic, sizeof(sans_italic), argv0,
                  "DejaVuSans-Oblique-Latin.ttf");
    psp_font_path(sans_bold, sizeof(sans_bold), argv0,
                  "DejaVuSans-Bold-Latin.ttf");
    psp_font_path(serif_bold, sizeof(serif_bold), argv0,
                  "DejaVuSerif-Bold-Latin.ttf");
    psp_font_path(metric_sans, sizeof(metric_sans), argv0,
                  "TilefinchSans-Regular.ttf");
    psp_font_path(metric_sans_bold, sizeof(metric_sans_bold), argv0,
                  "TilefinchSans-Bold.ttf");

    FontSet fonts;
    bool fonts_loaded = font_set_load(&fonts, &budget, sans, serif,
                                      sans_italic, sans_bold, serif_bold,
                                      metric_sans, metric_sans_bold,
                                      PSP_MAX_FONT_BYTES);
    printf("tilefinch-psp: fonts %s\n",
           fonts_loaded && fonts.sans.loaded ? "loaded" : "fallback");

    PocDocument document;
    if (!document_parse(&document, &budget,
                        (const char *) psp_fixture_html,
                        psp_fixture_html_length, PSP_PARSE_DEPTH_LIMIT)) {
        printf("tilefinch-psp: parse failed\n");
        goto sleep_forever;
    }

    ViewportContext viewport;
    if (!viewport_context_resolve(&viewport, &document, PSP_SCREEN_WIDTH,
                                  PSP_SCREEN_HEIGHT,
                                  PSP_FALLBACK_VIEWPORT_WIDTH)) {
        printf("tilefinch-psp: viewport resolution failed\n");
        goto sleep_forever;
    }

    Stylesheet stylesheet;
    if (!stylesheet_build_context(&stylesheet, &budget, &document,
                                  &viewport)) {
        printf("tilefinch-psp: stylesheet build failed\n");
        goto sleep_forever;
    }

    LayoutDocument layout;
    if (!layout_build_context(&layout, &budget, &document, &stylesheet,
                              fonts_loaded && (fonts.sans.loaded
                                               || fonts.serif.loaded)
                                  ? &fonts : NULL,
                              NULL, &viewport)) {
        printf("tilefinch-psp: layout failed\n");
        goto sleep_forever;
    }

    TileCache cache;
    if (!tile_cache_init(&cache, &budget, &layout, PSP_TILE_CAPACITY)
        || !tile_cache_set_frame(&cache, frame,
                                 PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT)) {
        printf("tilefinch-psp: tile cache init failed\n");
        goto sleep_forever;
    }

    /* Mirror the host lab's --scroll-all pass so cache counters are
       comparable, including the revisit determinism check. */
    size_t frame_pixels = PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT;
    uint16_t background = psp_color_rgb565(layout.page_background);
    int max_scroll = viewport_max_scroll_css(&layout.viewport,
                                             layout.height);
    int step = layout.viewport.css_height / 2;
    if (step < 1) step = 1;
    uint16_t *first_frame = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RENDER, frame_pixels * sizeof(*frame));
    size_t scroll_frames = 0;
    size_t blank_frames = 0;
    bool revisit_match = false;
    if (first_frame != NULL) {
        for (size_t i = 0;; i++) {
            long long candidate = (long long) i * step;
            int scroll = candidate < max_scroll ? (int) candidate
                                                : max_scroll;
            if (!tile_cache_render_frame(&cache, scroll, PSP_SCREEN_WIDTH,
                                         PSP_SCREEN_HEIGHT, NULL)) {
                printf("tilefinch-psp: frame %u failed\n", (unsigned) i);
                break;
            }
            if (i == 0) {
                memcpy(first_frame, frame, frame_pixels * sizeof(*frame));
            }
            if (psp_frame_is_blank(frame, frame_pixels, background)) {
                blank_frames++;
            }
            scroll_frames++;
            int prefetch_y = scroll + layout.viewport.css_height;
            if (prefetch_y < layout.height) {
                tile_cache_prefetch_row(&cache, prefetch_y,
                                        PSP_SCREEN_WIDTH);
            }
            if (scroll == max_scroll) break;
        }
        if (tile_cache_render_frame(&cache, 0, PSP_SCREEN_WIDTH,
                                    PSP_SCREEN_HEIGHT, NULL)) {
            revisit_match = memcmp(first_frame, frame,
                                   frame_pixels * sizeof(*frame)) == 0;
            scroll_frames++;
        }
        budget_free(&budget, first_frame);
    }

    /* Put a frame on the panel before reporting, so the scanout counters
       below describe a present that actually happened.  Reporting them ahead
       of the interactive loop would have made them permanently zero — the
       same "counted the attempt, not the outcome" mistake that hid a blank
       browser for an entire release. */
    psp_present(frame);

    /* Scanout outcome belongs in the counters: a fixture that rendered
       perfectly into a framebuffer nobody displayed is not a passing run. */
    psp_counter("display", "presents", psp_display.presents);
    psp_counter("display", "rejected", psp_display.rejections);
    psp_counter("document", "nodes", document.node_count);
    psp_counter("document", "elements", document.element_count);
    psp_counter("document", "text-nodes", document.text_node_count);
    psp_counter("document", "attributes", document.attribute_count);
    psp_counter("layout", "width", (unsigned long long) layout.width);
    psp_counter("layout", "scroll-width",
                (unsigned long long) layout.scroll_width);
    psp_counter("layout", "height", (unsigned long long) layout.height);
    psp_counter("layout", "commands", layout.count);
    psp_counter("layout", "links", layout.link_count);
    psp_counter("layout", "sticky", layout.sticky_count);
    psp_counter("layout", "fixed", layout.fixed_count);
    psp_counter("stylesheet", "rules", stylesheet.count);
    psp_counter("stylesheet", "important-rules",
                stylesheet.important_rule_count);
    psp_counter("stylesheet", "layers", stylesheet.layer_count);
    psp_counter("stylesheet", "variables", stylesheet.variable_count);
    psp_counter("stylesheet", "scoped-variables",
                stylesheet.custom_rule_count);
    psp_counter("stylesheet", "deferred-bytes", stylesheet.deferred_bytes);
    psp_counter("stylesheet-diagnostics", "declarations",
                stylesheet.diagnostic_declarations);
    psp_counter("stylesheet-diagnostics", "supported",
                stylesheet.diagnostic_supported_declarations);
    psp_counter("stylesheet-diagnostics", "rejected",
                stylesheet.diagnostic_rejected_declarations);
    psp_counter("stylesheet-diagnostics", "deferred",
                stylesheet.diagnostic_deferred_declarations);
    psp_counter("stylesheet-diagnostics", "custom-drops",
                stylesheet.diagnostic_custom_property_drops);
    psp_counter("stylesheet-diagnostics", "selector-drops",
                stylesheet.diagnostic_selector_drops);
    psp_counter("stylesheet-diagnostics", "unknown-media",
                stylesheet.diagnostic_unknown_media_features);
    psp_counter("stylesheet-diagnostics", "supports-false",
                stylesheet.diagnostic_supports_false_queries);
    psp_counter("tiles", "hits", cache.hits);
    psp_counter("tiles", "misses", cache.misses);
    psp_counter("tiles", "evictions", cache.evictions);
    psp_counter("tiles", "rasterized", cache.rasterized);
    psp_counter("tiles", "candidates", cache.command_candidates);
    psp_counter("tiles", "spatial-bands", layout.spatial_band_count);
    psp_counter("tiles", "glyph-misses", cache.glyph_cache_misses);
    psp_counter("tiles", "glyph-evictions", cache.glyph_cache_evictions);
    psp_counter("scroll", "frames", scroll_frames);
    psp_counter("scroll", "blank", blank_frames);
    printf("psp-counter\tfloat-article\tscroll.revisit\t%s\n",
           revisit_match ? "match" : "mismatch");
    printf("tilefinch-psp: counters-complete\n");

    /* Interactive: d-pad / analog scrolling until HOME or START. */
    int scroll = 0;
    bool dirty = true;
    for (;;) {
        if (dirty) {
            if (tile_cache_render_frame(&cache, scroll, PSP_SCREEN_WIDTH,
                                        PSP_SCREEN_HEIGHT, NULL)) {
                psp_present(frame);
            }
            dirty = false;
        }
        sceDisplayWaitVblankStart();
        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) continue;
        int next = scroll;
        if (pad.Buttons & PSP_CTRL_UP) next -= 16;
        if (pad.Buttons & PSP_CTRL_DOWN) next += 16;
        if (pad.Buttons & PSP_CTRL_LTRIGGER) next -= layout.viewport.css_height;
        if (pad.Buttons & PSP_CTRL_RTRIGGER) next += layout.viewport.css_height;
        if (pad.Ly < 64) next -= 12;
        if (pad.Ly > 192) next += 12;
        if (pad.Buttons & PSP_CTRL_START) break;
        if (next < 0) next = 0;
        if (next > max_scroll) next = max_scroll;
        if (next != scroll) {
            scroll = next;
            dirty = true;
        }
    }

    sceKernelExitGame();
    return 0;

sleep_forever:
    printf("tilefinch-psp: halted\n");
    sceKernelSleepThread();
    return 1;
}
