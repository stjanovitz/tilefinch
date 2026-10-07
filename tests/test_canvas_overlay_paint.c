/* Menus and dialogs over a live canvas, through the BrowserEngine facade.

   A full-viewport animated 2D canvas carries a centred dialog (rounded,
   translucent, a large glow box-shadow, overflow:auto) whose buttons have
   outline and border-colour focus styles and whose message line changes
   text. Each update is rendered the way the PSP frontend renders it (one
   bounded input-priority job) and then compared pixel for pixel against the
   same frame composed from a freshly rebuilt overlay. The counters check
   that only damage is repainted and that no background tile job starts.

   `--timing [translate|flex]` prints host costs instead of asserting. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/platform.h"
#include "tilefinch/render.h"
#include "../src/tilefinch_test_faults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define KIB 1024u
#define MIB (1024u * 1024u)
#define FRAME_W 480
#define FRAME_H 272

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool timing_mode;

static BrowserEngine *make_engine(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    if (!browser_config_apply_psp_memory_profile(
            &config, BROWSER_PSP_MEMORY_REALISTIC)) return NULL;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char fonts[7][512];
    static const char *names[7] = {
        "DejaVuSans-Latin.ttf", "DejaVuSerif-Latin.ttf",
        "DejaVuSans-Oblique-Latin.ttf", "DejaVuSans-Bold-Latin.ttf",
        "DejaVuSerif-Bold-Latin.ttf", "TilefinchSans-Regular.ttf",
        "TilefinchSans-Bold.ttf"
    };
    for (size_t at = 0; at < 7u; at++)
        snprintf(fonts[at], sizeof(fonts[at]), "%s/fonts/%s",
                 TILEFINCH_TEST_SOURCE_DIR, names[at]);
    if (!browser_config_set_font_paths(
            &config, fonts[0], fonts[1], fonts[2], fonts[3], fonts[4],
            fonts[5], fonts[6], 1536u * KIB)) return NULL;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine: %s\n", error);
    return engine;
}

/* translate: left/top 50% with translate(-50%,-50%). flex: a
   full-viewport flex layer centres the panel without a transform. fixed:
   a position:fixed translated panel (the retained fixed layer). article:
   the fixed panel over ordinary text, no canvas. */
typedef enum {
    VARIANT_TRANSLATE, VARIANT_FLEX, VARIANT_FIXED, VARIANT_ARTICLE,
    VARIANT_COUNT
} PageVariant;

static const char *const variant_names[VARIANT_COUNT] = {
    "translate", "flex", "fixed", "article"
};

static int page_html(char *out, size_t capacity, PageVariant variant)
{
    bool flex = variant == VARIANT_FLEX;
    bool article = variant == VARIANT_ARTICLE;
    const char *position = flex ? ""
        : variant == VARIANT_TRANSLATE
            ? "position:absolute;left:50%;top:50%;"
              "transform:translate(-50%,-50%);"
            : "position:fixed;left:50%;top:50%;"
              "transform:translate(-50%,-50%);";
    static const char text[] =
        "<p>Ordinary article text under a dialog that stays open while the "
        "reader moves between its buttons with the D-pad.</p>";
    return snprintf(out, capacity,
        "<!doctype html><html><head>"
        "<meta name=viewport content=\"width=480\"><style>"
        "html,body{margin:0;%s;background:#000;color:#cde}"
        "canvas{position:absolute;left:0;top:0;width:480px;height:272px}"
        "%s"
        "#panel{%s width:300px;max-height:250px;overflow-y:auto;"
        "padding:8px 12px;border:1px solid #5de0c2;border-radius:10px;"
        "background:rgba(4,18,23,.93);"
        "box-shadow:0 0 18px rgba(58,215,182,.28);text-align:center}"
        "h1{margin:0 0 4px;color:#fff;font-size:18px}"
        "#message{margin:0 0 6px;min-height:18px;color:#b9d7dc;"
        "font-size:13px}"
        "button{display:block;width:220px;margin:3px auto;padding:2px;"
        "border:1px solid #477d7d;border-radius:4px;background:#102b31;"
        "color:#d8fbf4;font:600 12px sans-serif}"
        "button:focus{outline:2px solid #ffffff;outline-offset:1px}"
        "button.ring:focus{outline:none;border-color:#ffd76f}"
        "</style></head><body>%s%s%s%s%s"
        "<div id=panel><h1>Main menu</h1><p id=message>Choose a mode</p>"
        "<button id=b0>Quick match</button><button id=b1>Campaign</button>"
        "<button id=b2>Practice</button><button id=b3 class=ring>"
        "Settings</button><button id=b4>Replays</button></div>%s%s"
        "</body></html>",
        article ? "" : "height:100%;overflow:hidden",
        flex ? "#layer{position:absolute;left:0;top:0;width:100%;"
               "height:100%;display:flex;align-items:center;"
               "justify-content:center;pointer-events:none}"
               "#panel{pointer-events:auto}" : "",
        position,
        article ? text : "<canvas id=c width=320 height=181></canvas>",
        article ? text : "", article ? text : "", article ? text : "",
        flex ? "<div id=layer>" : "",
        flex ? "</div>" : "",
        article ? ""
            : "<script>const c=document.getElementById('c');"
              "const g=c.getContext('2d');let t=0;"
              "function f(){t++;g.fillStyle='rgb('+(t*7%256)+',40,'"
              "+(90+t%60)+')';g.fillRect(0,0,320,181);"
              "g.fillStyle='#ffd040';g.fillRect((t*5)%300,40+(t%90),24,24);"
              "requestAnimationFrame(f)}requestAnimationFrame(f);</script>");
}

static TileCache *render_cache(BrowserEngine *engine)
{
    /* Test-only: drop the retained overlay to compose a reference. */
    return (TileCache *) browser_engine_render_metrics_view(engine);
}

static bool advance_and_render(BrowserEngine *engine)
{
    bool changed = false;
    if (!browser_engine_advance_runtime(engine, 16, 8, &changed))
        return false;
    BrowserRenderJobStatus status = BROWSER_RENDER_JOB_PENDING;
    for (int slice = 0; slice < 4096
         && status == BROWSER_RENDER_JOB_PENDING; slice++)
        status = browser_engine_render_frame_bounded(engine, 8000u, 4u);
    return status == BROWSER_RENDER_JOB_COMPLETE;
}

typedef struct {
    const char *name;
    uint64_t update_us;
    size_t builds_before;
    size_t patch_pixels_before;
    size_t jobs_before;
    size_t fixed_builds_before;
    size_t fixed_builds;
    uint64_t us;
    size_t patch_pixels;
    size_t builds;
    bool tile_job;
} UpdateCost;

/* The update has been applied to the page; render it as the PSP input path
   does, then compare against a rebuilt overlay with the same canvas. */
static bool render_and_compare(BrowserEngine *engine, UpdateCost *cost)
{
    static uint16_t incremental[FRAME_W * FRAME_H];
    TileCache *cache = render_cache(engine);
    size_t builds = cost->builds_before;
    size_t patch_pixels = cost->patch_pixels_before;
    size_t jobs = cost->jobs_before;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    BrowserRenderJobStatus status =
        browser_engine_render_frame_bounded(engine, 8000u, 4u);
    cost->us = tilefinch_platform_monotonic_time_us() - started;
    cost->builds = cache->canvas_overlay_builds - builds;
    cost->patch_pixels = cache->canvas_overlay_patch_pixels - patch_pixels;
    cost->fixed_builds =
        cache->fixed_cache_builds - cost->fixed_builds_before;
    cost->tile_job = cache->frame_jobs_scheduled != jobs
        || status != BROWSER_RENDER_JOB_COMPLETE;
    if (status != BROWSER_RENDER_JOB_COMPLETE) {
        /* Finish it so the comparison below still has a whole frame. */
        for (int slice = 0; slice < 4096
             && status == BROWSER_RENDER_JOB_PENDING; slice++)
            status = browser_engine_render_frame_bounded(engine, 8000u, 4u);
        CHECK(status == BROWSER_RENDER_JOB_COMPLETE);
    }
    CHECK(cache->frame != NULL
          && cache->frame_pixels >= (size_t) FRAME_W * FRAME_H);
    memcpy(incremental, cache->frame, sizeof(incremental));
    (void) tile_cache_reclaim_optional(cache);
    cache->canvas_paint_pending = true;
    CHECK(browser_engine_render_frame(engine, NULL));
    size_t mismatches = 0, first = 0;
    for (size_t at = 0; at < (size_t) FRAME_W * FRAME_H; at++) {
        if (incremental[at] != cache->frame[at]) {
            if (mismatches++ == 0) first = at;
        }
    }
    if (mismatches != 0)
        fprintf(stderr, "%s: %zu pixels differ, first at %zu,%zu "
                "(%04x vs %04x)\n", cost->name, mismatches,
                first % FRAME_W, first / FRAME_W, incremental[first],
                cache->frame[first]);
    CHECK(mismatches == 0);
    return true;
}

/* Rebuild the retained overlay and fixed layer twice, with the translucent
   blend memo and pixel by pixel: every retained pixel, its alpha and the
   composed frame must be identical. */
static bool retained_layers_match_reference(BrowserEngine *engine,
                                            const char *name)
{
    enum { OVERLAY_LIMIT = 131072 };
    static uint16_t frame[FRAME_W * FRAME_H];
    static uint16_t overlay[OVERLAY_LIMIT];
    static uint8_t overlay_alpha[OVERLAY_LIMIT];
    TileCache *cache = render_cache(engine);
    (void) tile_cache_reclaim_optional(cache);
    tile_cache_invalidate_rect(cache, 0, 0, FRAME_W, 4 * FRAME_H);
    cache->canvas_paint_pending = true;
    CHECK(browser_engine_render_frame(engine, NULL));
    size_t pixels = cache->canvas_overlay_pixel_count;
    CHECK(pixels <= OVERLAY_LIMIT);
    if (pixels != 0) {
        memcpy(overlay, cache->canvas_overlay_pixels,
               pixels * sizeof(*overlay));
        memcpy(overlay_alpha, cache->canvas_overlay_alpha, pixels);
    }
    memcpy(frame, cache->frame, sizeof(frame));
    tilefinch_test_faults()->raster_span_reference = true;
    (void) tile_cache_reclaim_optional(cache);
    tile_cache_invalidate_rect(cache, 0, 0, FRAME_W, 4 * FRAME_H);
    cache->canvas_paint_pending = true;
    bool rendered = browser_engine_render_frame(engine, NULL);
    tilefinch_test_faults()->raster_span_reference = false;
    CHECK(rendered && cache->canvas_overlay_pixel_count == pixels);
    size_t overlay_mismatches = 0, frame_mismatches = 0;
    for (size_t at = 0; at < pixels; at++) {
        if (overlay[at] != cache->canvas_overlay_pixels[at]
            || overlay_alpha[at] != cache->canvas_overlay_alpha[at])
            overlay_mismatches++;
    }
    for (size_t at = 0; at < (size_t) FRAME_W * FRAME_H; at++)
        if (frame[at] != cache->frame[at]) frame_mismatches++;
    if (overlay_mismatches != 0 || frame_mismatches != 0)
        fprintf(stderr, "%s: blend memo differs from the reference in %zu "
                "overlay and %zu frame pixels\n", name, overlay_mismatches,
                frame_mismatches);
    CHECK(overlay_mismatches == 0 && frame_mismatches == 0);
    return true;
}

static bool run_page_script(BrowserEngine *engine, const char *script)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    ScriptResult result;
    CHECK(script_runtime_evaluate_diagnostic(
        navigation->page.runtime, script, "<step>", &result));
    /* Commit the mutation without another canvas frame. */
    bool changed = false;
    return browser_engine_advance_runtime(engine, 0, 8, &changed);
}

static bool set_message(BrowserEngine *engine, const char *text)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    char source[256];
    snprintf(source, sizeof(source),
             "document.getElementById('message').textContent='%s';"
             "globalThis.pocSummary='ok'", text);
    ScriptResult result;
    CHECK(script_runtime_evaluate_diagnostic(
        navigation->page.runtime, source, "<message>", &result));
    /* Commit the mutation without another canvas frame. */
    bool changed = false;
    return browser_engine_advance_runtime(engine, 0, 8, &changed);
}

static void report(BrowserEngine *engine, const UpdateCost *cost,
                   const char *variant)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    ScriptResult result;
    const char *focused = script_runtime_evaluate_diagnostic(
            navigation->page.runtime,
            "globalThis.pocSummary=document.activeElement?"
            "document.activeElement.id+'/'+document.getElementById('panel').getBoundingClientRect().height:'-'", "<focused>", &result)
        ? result.summary : "?";
    printf("canvas-overlay-paint variant=%s update=%s update-us=%llu "
           "render-us=%llu "
           "patch-pixels=%zu builds=%zu fixed-builds=%zu tile-job=%d "
           "focus=%s relayouts=%zu "
           "outline-skips=%zu paint-skips=%zu\n", variant, cost->name,
           (unsigned long long) cost->update_us,
           (unsigned long long) cost->us, cost->patch_pixels, cost->builds,
           cost->fixed_builds, cost->tile_job ? 1 : 0, focused,
           navigation->incremental_relayouts,
           navigation->performance.focus_outline_relayout_skips,
           navigation->performance.focus_paint_relayout_skips);
}

static bool run_variant(PageVariant kind)
{
    const char *variant = variant_names[kind];
    bool canvas = kind != VARIANT_ARTICLE;
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    static char html[8192];
    int length = page_html(html, sizeof(html), kind);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    CHECK(browser_engine_commit_html(
        engine, "https://menu.test/index.html", html, (size_t) length,
        true));
    while (!browser_engine_baseline_fonts_ready(engine))
        CHECK(browser_engine_pump_baseline_fonts(engine));
    for (int frame = 0; frame < 6; frame++)
        CHECK(advance_and_render(engine));
    TileCache *cache = render_cache(engine);
    CHECK(!canvas
          || (cache->canvas_fast_frames >= 4u && cache->canvas_overlay_ready));
    CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN));
    CHECK(advance_and_render(engine) && advance_and_render(engine));

    static const struct {
        const char *name;
        int kind; /* 0 focus down, 1 focus up, 2 text, 3 script */
        const char *text;
    } steps[] = {
        /* Another menu screen: a panel of a different height. */
        {"screen-short", 3, "for(const b of document.querySelectorAll"
         "('button'))if(b.id>'b1')b.style.display='none';"
         "document.getElementById('message').textContent='Pick one';"
         "globalThis.pocSummary='ok'"},
        {"screen-full", 3, "for(const b of document.querySelectorAll"
         "('button'))b.style.display='';globalThis.pocSummary='ok'"},
        {"focus-outline-1", 0, NULL},
        {"focus-outline-2", 0, NULL},
        {"focus-border", 0, NULL},
        {"focus-border-leave", 0, NULL},
        {"focus-outline-back", 1, NULL},
        {"text-1", 2, "Clear three arenas before your last redeploy."},
        {"text-2", 2, "Score x1.80 | Deploy at half armor."},
        {"text-3", 2, "Ready"},
    };
    size_t tile_jobs = 0;
    for (size_t at = 0; at < sizeof(steps) / sizeof(steps[0]); at++) {
        /* Two ordinary canvas frames: the overlay is retained again. */
        CHECK(advance_and_render(engine) && advance_and_render(engine));
        CHECK(!canvas || cache->canvas_overlay_ready);
        UpdateCost cost = {
            .fixed_builds_before = cache->fixed_cache_builds,
            .name = steps[at].name,
            .builds_before = cache->canvas_overlay_builds,
            .patch_pixels_before = cache->canvas_overlay_patch_pixels,
            .jobs_before = cache->frame_jobs_scheduled
        };
        uint64_t update_started = tilefinch_platform_monotonic_time_us();
        if (steps[at].kind == 3) {
            CHECK(run_page_script(engine, steps[at].text));
        } else if (steps[at].kind == 2) {
            CHECK(set_message(engine, steps[at].text));
        } else {
            CHECK(browser_engine_focus_direction(
                engine, steps[at].kind == 0 ? CONTROLLER_FOCUS_DOWN
                                            : CONTROLLER_FOCUS_UP));
        }
        cost.update_us =
            tilefinch_platform_monotonic_time_us() - update_started;
        CHECK(render_and_compare(engine, &cost));
        if (!timing_mode)
            CHECK(retained_layers_match_reference(engine, steps[at].name));
        if (timing_mode) report(engine, &cost, variant);
        tile_jobs += cost.tile_job ? 1u : 0u;
        /* A patch repaints its damage, never a whole viewport panel. */
        /* A focus or text change over the canvas recomposes with the
           canvas fast path in the same bounded job: no tile job, no
           overlay rebuild, and only its damage repainted. */
        if (!timing_mode && canvas && steps[at].kind == 3)
            CHECK(!cost.tile_job);
        else if (!timing_mode && canvas)
            CHECK(!cost.tile_job && cost.builds == 0
                  && cost.patch_pixels < (size_t) FRAME_W * FRAME_H / 8u);
        /* A focus change inside a fixed dialog patches the fixed layer. */
        if (!timing_mode && steps[at].kind <= 1)
            CHECK(cost.fixed_builds == 0);
    }
    if (timing_mode) {
        /* A full overlay build (a screen change over the canvas). */
        uint64_t build_total = 0;
        for (int at = 0; at < 20; at++) {
            (void) tile_cache_reclaim_optional(cache);
            cache->canvas_paint_pending = true;
            uint64_t build_started = tilefinch_platform_monotonic_time_us();
            CHECK(browser_engine_render_frame(engine, NULL));
            build_total += tilefinch_platform_monotonic_time_us()
                - build_started;
        }
        printf("canvas-overlay-paint variant=%s full-build-us=%llu "
               "regions=%zu pixels=%zu\n", variant,
               (unsigned long long) (build_total / 20u),
               cache->canvas_overlay_region_count,
               cache->canvas_overlay_pixel_count);
        uint64_t started = tilefinch_platform_monotonic_time_us();
        for (int frame = 0; frame < 20; frame++)
            CHECK(advance_and_render(engine));
        printf("canvas-overlay-paint variant=%s steady-frame-us=%llu "
               "tile-jobs=%zu\n", variant,
               (unsigned long long)
                   ((tilefinch_platform_monotonic_time_us() - started) / 20u),
               tile_jobs);
    }
    browser_engine_destroy(engine);
    return true;
}

/* An in-place repaint of a page already on screen keeps its presented
   scroll position: the PSP frontend shows placeholder tiles only when the
   view moved, so a relayout never fills in tile by tile. */
static bool plain_page_in_place_repaint(void)
{
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    static const char html[] =
        "<!doctype html><body style=\"margin:0\"><h1 id=t>Title</h1>"
        "<p>Some text that stays.</p></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://plain.test/", html, sizeof(html) - 1u, true));
    while (!browser_engine_baseline_fonts_ready(engine))
        CHECK(browser_engine_pump_baseline_fonts(engine));
    CHECK(advance_and_render(engine));
    TileCache *cache = render_cache(engine);
    CHECK(cache->presented_scroll_valid && cache->presented_scroll_y == 0);
    CHECK(run_page_script(
        engine, "document.getElementById('t').textContent='Changed';"
                "globalThis.pocSummary='ok'"));
    CHECK(browser_engine_render_frame_bounded(engine, 1u, 1u)
              != BROWSER_RENDER_JOB_FAILED
          && cache->presented_scroll_valid
          && cache->presented_scroll_y == 0);
    browser_engine_destroy(engine);
    return true;
}

/* A full-screen game hides its menu and HUD (two relayouts in one turn,
   the second with the retained overlay already dropped): the opaque
   full-viewport canvas composes the next frame by itself in one bounded
   job, exactly as the tile path would, instead of the page under it being
   rasterized in background tile slices. A canvas that leaves part of the
   viewport uncovered still takes the tile path. */
static bool full_viewport_canvas_relayout(bool covering)
{
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    static char html[4096];
    int length = snprintf(html, sizeof(html),
        "<!doctype html><html><head>"
        "<meta name=viewport content=\"width=480\"><style>"
        "html,body{margin:0;height:100%%;overflow:hidden;background:#123}"
        "canvas{position:absolute;left:0;top:0;width:%dpx;height:272px}"
        "#hud{position:absolute;left:8px;top:4px;color:#ff0;"
        "font:12px sans-serif}"
        "#panel{position:absolute;left:50%%;top:50%%;"
        "transform:translate(-50%%,-50%%);width:300px;padding:8px;"
        "border:1px solid #5de0c2;border-radius:10px;"
        "background:rgba(4,18,23,.93);"
        "box-shadow:0 0 18px rgba(58,215,182,.28);color:#fff}"
        "body.run #panel{display:none}"
        "</style></head><body><canvas id=c width=320 height=181></canvas>"
        "<div id=hud>00000</div><div id=panel><h1>Quick match</h1>"
        "<button id=b>Deploy</button></div>"
        "<script>const c=document.getElementById('c');"
        "const g=c.getContext('2d');function f(){g.fillStyle='#246';"
        "g.fillRect(0,0,320,181);g.fillStyle='#fd4';"
        "g.fillRect(40,30,60,50);requestAnimationFrame(f)}"
        "requestAnimationFrame(f);</script></body></html>",
        covering ? 480 : 400);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    CHECK(browser_engine_commit_html(
        engine, "https://game.test/index.html", html, (size_t) length,
        true));
    while (!browser_engine_baseline_fonts_ready(engine))
        CHECK(browser_engine_pump_baseline_fonts(engine));
    for (int frame = 0; frame < 6; frame++)
        CHECK(advance_and_render(engine));
    TileCache *cache = render_cache(engine);
    CHECK(!covering
          || (cache->canvas_fast_frames >= 4u && cache->canvas_overlay_ready));
    CHECK(run_page_script(engine, "document.body.className='run';"
                                  "globalThis.pocSummary='ok'"));
    /* Gameplay chrome is retained elsewhere: the HUD goes too. Nothing
       is left over the canvas, so the tile path is an exact reference
       (a translucent overlay is composed differently on the two paths). */
    CHECK(run_page_script(engine, "document.getElementById('hud')"
                                  ".style.display='none';"
                                  "globalThis.pocSummary='ok'"));
    size_t jobs = cache->frame_jobs_scheduled;
    size_t canvas_frames = cache->canvas_fast_frames;
    BrowserRenderJobStatus status =
        browser_engine_render_frame_bounded(engine, 8000u, 4u);
    if (!covering) {
        CHECK(cache->frame_jobs_scheduled != jobs);
        browser_engine_destroy(engine);
        return true;
    }
    CHECK(status == BROWSER_RENDER_JOB_COMPLETE);
    CHECK(cache->frame_jobs_scheduled == jobs);
    CHECK(cache->canvas_fast_frames == canvas_frames + 1u);
    static uint16_t composed[FRAME_W * FRAME_H];
    CHECK(cache->frame != NULL
          && cache->frame_pixels >= (size_t) FRAME_W * FRAME_H);
    memcpy(composed, cache->frame, sizeof(composed));
    /* Reference: the same page through the tile path. */
    tilefinch_test_faults()->canvas_frame_reference = true;
    cache->last_frame_scroll_valid = false;
    tile_cache_invalidate_rect(cache, 0, 0, FRAME_W, FRAME_H);
    status = BROWSER_RENDER_JOB_PENDING;
    for (int slice = 0; slice < 4096
         && status == BROWSER_RENDER_JOB_PENDING; slice++)
        status = browser_engine_render_frame_bounded(engine, 8000u, 4u);
    tilefinch_test_faults()->canvas_frame_reference = false;
    CHECK(status == BROWSER_RENDER_JOB_COMPLETE);
    CHECK(cache->frame_jobs_scheduled != jobs);
    size_t mismatches = 0, first = 0;
    for (size_t at = 0; at < (size_t) FRAME_W * FRAME_H; at++) {
        if (composed[at] != cache->frame[at] && mismatches++ == 0)
            first = at;
    }
    if (mismatches != 0)
        fprintf(stderr, "full-viewport canvas: %zu pixels differ, first at "
                "%zu,%zu (%04x vs %04x)\n", mismatches, first % FRAME_W,
                first / FRAME_W, composed[first], cache->frame[first]);
    CHECK(mismatches == 0);
    /* Ordinary canvas frames continue on the canvas path. */
    canvas_frames = cache->canvas_fast_frames;
    CHECK(advance_and_render(engine) && advance_and_render(engine));
    CHECK(cache->canvas_fast_frames >= canvas_frames + 2u);
    browser_engine_destroy(engine);
    return true;
}

/* Fill spans in page tiles: a page of rounded, translucent and opaque
   fills (and the paint fixture) rasterized with spans and pixel by pixel
   must give identical frames, scrolled through the whole page. */
static bool tile_fills_match_reference(const char *html, size_t length)
{
    static uint16_t frame[FRAME_W * FRAME_H];
    BrowserEngine *engine = make_engine();
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(
        engine, "https://fills.test/index.html", html, length, true));
    while (!browser_engine_baseline_fonts_ready(engine))
        CHECK(browser_engine_pump_baseline_fonts(engine));
    TileCache *cache = render_cache(engine);
    NavigationSession *navigation = browser_engine_navigation(engine);
    int height = navigation->page.layout.height;
    for (int scroll = 0; scroll == 0 || scroll < height - FRAME_H;
         scroll += FRAME_H) {
        if (scroll != 0) CHECK(browser_engine_scroll_by(engine, FRAME_H));
        tile_cache_invalidate_rect(cache, 0, 0, FRAME_W, height + FRAME_H);
        CHECK(browser_engine_render_frame(engine, NULL));
        memcpy(frame, cache->frame, sizeof(frame));
        tilefinch_test_faults()->raster_span_reference = true;
        tile_cache_invalidate_rect(cache, 0, 0, FRAME_W, height + FRAME_H);
        (void) tile_cache_reclaim_optional(cache);
        bool rendered = browser_engine_render_frame(engine, NULL);
        tilefinch_test_faults()->raster_span_reference = false;
        CHECK(rendered);
        size_t mismatches = 0;
        for (size_t at = 0; at < (size_t) FRAME_W * FRAME_H; at++)
            if (frame[at] != cache->frame[at]) mismatches++;
        if (mismatches != 0)
            fprintf(stderr, "fill spans differ from the reference in %zu "
                    "pixels at scroll %d\n", mismatches, scroll);
        CHECK(mismatches == 0);
    }
    browser_engine_destroy(engine);
    return true;
}

static bool page_fills_match_reference(void)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport "
        "content=\"width=480\"><style>"
        "body{margin:0;background:linear-gradient(#183c49,#020506)}"
        "div{margin:6px;padding:8px;color:#fff;font:600 13px sans-serif}"
        ".a{background:rgba(4,18,23,.93);border-radius:10px}"
        ".b{background:#102b31;border-radius:4px 9px 0 17px;"
        "border:1px solid #477d7d}"
        ".c{background:rgba(255,215,111,.4);border-radius:40px;height:60px}"
        ".d{background:rgba(200,200,200,.5)}"
        ".e{background:#777;border-radius:3px;"
        "box-shadow:0 0 18px rgba(58,215,182,.28)}"
        ".f{background:rgba(0,0,0,.2);border-radius:50%;width:90px;"
        "height:90px;mix-blend-mode:multiply}"
        "</style></head><body>"
        "<div class=a>Translucent rounded panel<div class=b>Opaque button"
        "</div><div class=c>Pill</div></div><div class=d>Square "
        "translucent</div><div class=e>Grey neutral</div><div class=f>"
        "</div><div class=a style=\"border-radius:0 30px\">Mixed corners"
        "<div class=e>Inner</div></div>"
        /* Glyph rows: translucent, bold, synthetic-bold, faded and
           letter-spaced ink over flat, translucent and gradient ground. */
        "<p style=\"opacity:.55;color:#ffd76f;font:600 15px sans-serif\">"
        "Faded synthetic bold</p><div class=a style=\"color:rgba(255,255,"
        "255,.6);letter-spacing:1px\">Translucent ink, spaced</div>"
        "<p style=\"font:700 11px serif;color:#9fe\">Bold serif face</p>"
        "</body></html>";
    if (!tile_fills_match_reference(page, sizeof(page) - 1u)) return false;
    char path[512];
    snprintf(path, sizeof(path), "%s/fixtures/paint-features.html",
             TILEFINCH_TEST_SOURCE_DIR);
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    static char fixture[65536];
    size_t length = fread(fixture, 1, sizeof(fixture), file);
    fclose(file);
    CHECK(length > 0 && length < sizeof(fixture));
    return tile_fills_match_reference(fixture, length);
}

/* Every destination and source alpha, over destinations at the channel
   extremes and in between, blended into a layer with its own alpha. */
static bool alpha_blend_exhaustive(void)
{
    static const uint16_t destinations[] = {
        0x0000, 0xffff, 0x0841, 0xf7de, 0x18e3, 0x2945, 0x4a69, 0x8410,
        0xa534, 0xc618, 0x07e0, 0xf800, 0x001f, 0x1234, 0xabcd, 0x5aeb
    };
    static const uint32_t sources[] = {
        0x000000, 0xffffff, 0x3ad7b6, 0x041217, 0x5de0c2, 0xffd76f
    };
    for (size_t d = 0; d < sizeof(destinations) / sizeof(*destinations); d++)
        for (size_t s = 0; s < sizeof(sources) / sizeof(*sources); s++)
            CHECK(render_test_alpha_blend_mismatches(
                      destinations[d], sources[s]) == 0);
    return true;
}

int main(int argc, char **argv)
{
    timing_mode = argc >= 2 && strcmp(argv[1], "--timing") == 0;
    for (int kind = 0; kind < VARIANT_COUNT; kind++) {
        if (timing_mode && argc == 3
            && strcmp(argv[2], variant_names[kind]) != 0) continue;
        if (!run_variant((PageVariant) kind)) return 1;
    }
    if (!timing_mode && !alpha_blend_exhaustive()) return 1;
    if (!timing_mode && !page_fills_match_reference()) return 1;
    if (!timing_mode && !plain_page_in_place_repaint()) return 1;
    if (!timing_mode && (!full_viewport_canvas_relayout(true)
                         || !full_viewport_canvas_relayout(false))) return 1;
    puts("canvas-overlay-paint-tests: ok");
    return 0;
}
