/* Boot-time qualification modes of the validation EBOOT.
 *
 * Each mode is selected by a boot.cfg validation knob, runs one hardware or
 * engine probe in place of the interactive session, logs its
 * `tilefinch-validation: outcome=` line and leaves the process. The JavaScript
 * micro-benchmark is the exception: it runs before the first HOME frame and
 * boot continues. main() reaches these through the hooks in
 * psp_app_validation.h.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_validation.h"
#include "tilefinch_compiler.h"

#include "tilefinch/js_bench.h"
#include "tilefinch/psp_media_fixture.h"
#include "tilefinch/psp_media_range_probe.h"
#include "tilefinch/psp_raster_fixture.h"
#include "psp_webgl_ge_probe.h"
#include "psp_boot_timing.h"
#include "tilefinch/game_audio.h"

static TILEFINCH_COLD_PATH void psp_js_bench_emit(void *opaque,
                                                  const char *line)
{
    (void) opaque;
    psp_log_printf("%s\n", line);
}

/* The JavaScript engine micro-benchmark (boot.cfg validation_js_bench=N):
   runs before the first HOME frame on the same clock, allocator and engine
   build as page script, then boot continues so the input script can end the
   run in the ordinary way. */
static TILEFINCH_COLD_PATH void psp_run_js_bench(const char *argv0,
                                                 const PspBootConfig *config)
{
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TIMING JAVASCRIPT ENGINE", 720);
    JsBenchOptions options;
    js_bench_default_options(&options);
    options.scale = (unsigned) config->validation_js_bench;
    options.only = config->js_bench_only[0] != '\0'
        ? config->js_bench_only : NULL;
    char trace_path[512];
    const char *dir = config->js_bench_dir[0] != '\0' ? config->js_bench_dir
        : strcmp(config->trace, "none") != 0 ? config->trace : NULL;
    if (dir != NULL) {
        psp_sibling_path(trace_path, sizeof(trace_path), argv0, dir);
        options.trace_dir = trace_path;
    }
    psp_log_checkpoint("js-bench-begin");
    int failed = js_bench_run(&options, psp_js_bench_emit, NULL);
    psp_log_printf("tilefinch-js-bench: outcome=%s\n",
                   failed == 0 ? "complete" : "failed");
    psp_log_checkpoint("js-bench-complete");
}

static TILEFINCH_COLD_PATH int psp_run_webgl_ge_qualification(
    BrowserEngine *engine, Budget *budget,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING WEBGL GE COST", 720);
    PspWebglGeProbeReport report;
    uint16_t *page = psp_display_back_buffer(&psp_display);
    size_t frame_pixels = 0;
    uint16_t *frame = browser_engine_framebuffer(engine, &frame_pixels);
    bool passed = psp_webgl_ge_probe_run(
        page, frame, frame_pixels, &report);
    if (passed) passed = psp_display_publish(&psp_display);
    printf(
        "tilefinch-webgl-ge-probe: memory color=%zu depth=%zu "
        "texture-cache=%zu context-inits=%u syncs=%u\n",
        report.color_bytes, report.depth_bytes, report.texture_cache_bytes,
        report.context_initializations, report.synchronizations);
    for (unsigned at = 0; at < PSP_WEBGL_GE_PROBE_SCENE_COUNT; at++) {
        const PspWebglGeProbeScene *scene = &report.scenes[at];
        if (scene->name == NULL || scene->frames == 0) continue;
        printf(
            "tilefinch-webgl-ge-probe: scene=%s frames=%u draws=%u "
            "vertices=%u upload-bytes=%zu prepare-avg=%lluus "
            "emit-avg=%lluus wait-avg=%lluus total-avg=%lluus "
            "total-max=%lluus list-avg=%zu list-max=%zu "
            "checksum=0x%08x outcome=%s\n",
            scene->name, scene->frames, scene->draw_calls_per_frame,
            scene->vertices_per_frame, scene->upload_bytes_per_frame,
            (unsigned long long) (scene->prepare_us / scene->frames),
            (unsigned long long) (scene->emit_us / scene->frames),
            (unsigned long long) (scene->wait_us / scene->frames),
            (unsigned long long) (scene->total_us / scene->frames),
            (unsigned long long) scene->maximum_frame_us,
            scene->list_bytes / scene->frames,
            scene->maximum_list_bytes, (unsigned) scene->checksum,
            scene->passed ? "pass" : "fail");
    }
    const PspWebglGeConversionProbe *conversion = &report.conversion;
    printf(
        "tilefinch-webgl-ge-probe: conversion=320x180-to-480x272 "
        "frames=%u cpu-convert-avg=%lluus cpu-convert-max=%lluus "
        "cpu-copy-avg=%lluus cpu-copy-max=%lluus "
        "ge-submit-avg=%lluus ge-wait-avg=%lluus "
        "ge-total-avg=%lluus ge-total-max=%lluus "
        "pixels=%zu mismatches=%zu cpu=0x%08x ge=0x%08x "
        "pixel-exact=%s available=%s\n",
        conversion->frames,
        (unsigned long long) (conversion->frames == 0 ? 0
            : conversion->cpu_convert_us / conversion->frames),
        (unsigned long long) conversion->cpu_convert_max_us,
        (unsigned long long) (conversion->frames == 0 ? 0
            : conversion->cpu_copy_us / conversion->frames),
        (unsigned long long) conversion->cpu_copy_max_us,
        (unsigned long long) (conversion->frames == 0 ? 0
            : conversion->ge_submit_us / conversion->frames),
        (unsigned long long) (conversion->frames == 0 ? 0
            : conversion->ge_wait_us / conversion->frames),
        (unsigned long long) (conversion->frames == 0 ? 0
            : conversion->ge_total_us / conversion->frames),
        (unsigned long long) conversion->ge_total_max_us,
        conversion->compared_pixels, conversion->mismatched_pixels,
        (unsigned) conversion->cpu_checksum,
        (unsigned) conversion->ge_checksum,
        conversion->pixel_exact ? "yes" : "no",
        conversion->available ? "yes" : "no");
    for (unsigned variant = 0; variant < conversion->strip_variants;
         variant++) {
        printf(
            "tilefinch-webgl-ge-probe: strip-texels=%u frames=%u "
            "ge-wait-avg=%lluus ge-total-avg=%lluus ge-total-max=%lluus "
            "mismatches=%zu\n",
            conversion->strip_texels[variant], conversion->frames,
            (unsigned long long) (conversion->frames == 0 ? 0
                : conversion->strip_wait_us[variant] / conversion->frames),
            (unsigned long long) (conversion->frames == 0 ? 0
                : conversion->strip_total_us[variant] / conversion->frames),
            (unsigned long long) conversion->strip_total_max_us[variant],
            conversion->strip_mismatches[variant]);
    }
    printf("tilefinch-webgl-ge-probe: dcache-wbinv-all=%lluus\n",
           (unsigned long long) conversion->dcache_all_us);
    printf("tilefinch-webgl-ge-probe: event=%s detail=\"%s\"\n",
           passed ? "pass" : "fail", report.detail);
    psp_report_budget_counters(budget, "webgl-ge-probe-exit");
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           passed ? "clean-exit" : "qualification-failed",
           passed ? "webgl-ge-pass" : "webgl-ge-fail");
    psp_log_checkpoint("webgl-ge-probe-complete");
    psp_log_finish(
        passed ? "webgl-ge-probe-pass" : "webgl-ge-probe-fail");
    psp_exit_console(exit_to);
    return passed ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_ge_present_qualification(
    BrowserEngine *engine, Budget *budget,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    /*
     * The one way the graphics-engine presenter can be exercised without
     * a decoder. It supplies its own pictures, so it reaches everything
     * in that path except the firmware that would normally produce them:
     * context setup, the framebuffer address, texture format and stride,
     * the UV mapping, the wide-frame split, the completion wait, and the
     * cache handshake either side of the draw.
     */
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO PRESENTATION", 860);
    void *probe_scratch = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1,
        PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES);
    /*
     * The probe draws into the 32-bit video surface, because that is the
     * surface a Smooth present draws into and the passthrough it checks is a
     * property of both ends being 8888. Switching here also exercises the
     * mode change itself, which the emulator runs as faithfully as a game
     * does.
     */
    bool surface_entered = psp_display_video_begin(&psp_display);
    char probe_detail[192];
    snprintf(probe_detail, sizeof(probe_detail),
             surface_entered ? "scratch unavailable"
                             : "video surface unavailable");
    PspMediaPresentProbeCase probe_cases[PSP_MEDIA_PRESENT_PROBE_CASES] = {0};
    /* Staged into the display's own EDRAM texture, which is what playback
       samples: a probe that measured a main-memory read would be timing a
       pipeline nothing runs. */
    bool probe_passed = probe_scratch != NULL && surface_entered
        && psp_media_present_ge_probe(
               probe_scratch, PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES,
               psp_display_video_texture(&psp_display),
               PSP_DISPLAY_VIDEO_TEXTURE_BYTES,
               psp_display_video_back_buffer(&psp_display),
               probe_cases, probe_detail, sizeof(probe_detail));
    bool surface_left = psp_display_video_end(&psp_display);
    /*
     * The per-case lines, logged here rather than in the presenter. That
     * translation unit's stdout is the PSP console, which nothing collects,
     * so its own printf never reached the validation log the truth cycle
     * reads -- and the emulator runner asks for exactly these lines.
     */
    for (unsigned at = 0; at < PSP_MEDIA_PRESENT_PROBE_CASES; at++) {
        const PspMediaPresentProbeCase *probe_case = &probe_cases[at];
        if (probe_case->source_width == 0) continue;
        /* sync is the real staged draw; output-sync is the same rectangle
           from a cache-resident texture, so texture-read = sync - output-sync.
           On device that split says whether the remaining GE cost is the
           texture read (chase it) or the 8888 output write (irreducible). */
        uint64_t texture_sync_us =
            probe_case->sync_us > probe_case->output_sync_us
                ? probe_case->sync_us - probe_case->output_sync_us : 0;
        printf("tilefinch-media-present-probe: case=%dx%d stride=%d quads=%u "
               "slot=%u clockwise=%d crop=%d "
               "output=%dx%d texture=%s flat=0x%06x halves=0x%06x/0x%06x "
               "submit=%lluus sync=%lluus output-sync=%lluus "
               "texture-sync=%lluus outcome=%s\n",
               probe_case->source_width, probe_case->source_height,
               probe_case->source_stride, probe_case->quads,
               probe_case->slot, probe_case->clockwise ? 1 : 0,
               probe_case->portrait_crop ? 1 : 0,
               probe_case->output_width, probe_case->output_height,
               probe_case->staged ? "edram" : "main",
               (unsigned) probe_case->flat, (unsigned) probe_case->left,
               (unsigned) probe_case->right,
               (unsigned long long) probe_case->submit_us,
               (unsigned long long) probe_case->sync_us,
               (unsigned long long) probe_case->output_sync_us,
               (unsigned long long) texture_sync_us,
               probe_case->passed ? "pass" : "fail");
    }
    uint32_t probe_drawn = 0;
    uint32_t probe_source = 0;
    psp_media_present_ge_passthrough(&probe_drawn, &probe_source);
    printf("tilefinch-media-present-probe: event=%s drawn=0x%08x "
           "source=0x%08x passthrough=%s surface=%d/%d detail=\"%s\"\n",
           probe_passed ? "pass" : "fail",
           (unsigned) probe_drawn, (unsigned) probe_source,
           probe_drawn != 0 && (probe_drawn & UINT32_C(0x00ffffff))
                   == (probe_source & UINT32_C(0x00ffffff))
               ? "yes" : "no",
           surface_entered ? 1 : 0, surface_left ? 1 : 0,
           probe_detail);
    /*
     * Where each surface byte landed, in both worlds. The engine column must
     * repeat its own probe -- that is what passthrough means -- and the scaler
     * column records the PSP-native 5650 mapping used by Sharp and chrome.
     */
    uint32_t probe_map_engine[PSP_MEDIA_PRESENT_CHANNEL_PROBES] = {0};
    uint16_t probe_map_scaler[PSP_MEDIA_PRESENT_CHANNEL_PROBES] = {0};
    psp_media_present_ge_channel_map(probe_map_engine, probe_map_scaler);
    printf("tilefinch-media-present-probe: channel-map "
           "byte0=0x%08x/0x%04x byte1=0x%08x/0x%04x byte2=0x%08x/0x%04x\n",
           (unsigned) probe_map_engine[0], (unsigned) probe_map_scaler[0],
           (unsigned) probe_map_engine[1], (unsigned) probe_map_scaler[1],
           (unsigned) probe_map_engine[2], (unsigned) probe_map_scaler[2]);
    budget_free(budget, probe_scratch);
    psp_report_budget_counters(budget, "ge-present-probe-exit");
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           probe_passed ? "clean-exit" : "qualification-failed",
           probe_passed ? "ge-present-pass" : "ge-present-fail");
    psp_log_checkpoint("ge-present-probe-complete");
    psp_log_finish(
        probe_passed ? "ge-present-probe-pass"
                     : "ge-present-probe-fail");
    psp_exit_console(exit_to);
    return probe_passed ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_csc_order_qualification(
    BrowserEngine *engine, Budget *budget,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    /*
     * The experiment the present probe left owed to a device.
     *
     * Smooth and Sharp disagree about which surface byte holds which colour,
     * no PSP pixel format can reconcile them, and the only remaining free
     * variable is the byte order sceMpegBaseCscAvc writes -- hypothetically
     * chosen by two mode words nothing readable here documents. This decodes
     * one picture and re-converts it under each candidate, printing what the
     * firmware wrote beside what the panel is proven to want. It is its own
     * boot mode for a reason: no playback session may ever run this, because a
     * candidate that succeeds leaves an unpresentable surface behind.
     */
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO COLOUR ORDER", 860);
    PspMediaCscOrderReport report = {0};
    char probe_error[256] = {0};
    bool probe_passed = psp_media_fixture_csc_order_probe(
        budget, &report, probe_error, sizeof(probe_error));
    /* The same boundary the embedded fixture reports: an emulator has no
       raw-NAL decoder, so it never produces the picture this repeats, and that
       is untested rather than failed. */
    bool emulator_untested = !probe_passed
        && (report.last_native_error
                == MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE
            || strstr(probe_error, "raw-NAL") != NULL
            || strstr(probe_error, "80010002") != NULL);
    printf(
        "tilefinch-media-csc-probe: event=%s error=\"%s\" candidates=%u "
        "usable=%u refused=%u quarantine=%d native=0x%08X\n",
        probe_passed ? "hardware-pass"
            : (emulator_untested ? "emulator-untested" : "hardware-fail"),
        probe_error, report.candidates_tried, report.candidates_usable,
        report.candidates_refused, report.quarantined ? 1 : 0,
        (unsigned) report.last_native_error);
    psp_report_budget_counters(budget, "csc-order-probe-exit");
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           probe_passed || emulator_untested
               ? "clean-exit" : "qualification-failed",
           probe_passed ? "csc-order-pass"
               : (emulator_untested ? "emulator-untested"
                                    : "csc-order-fail"));
    psp_log_checkpoint("csc-order-probe-complete");
    psp_log_finish(
        probe_passed ? "csc-order-probe-pass"
            : (emulator_untested ? "csc-order-probe-untested"
                                 : "csc-order-probe-fail"));
    psp_exit_console(exit_to);
    return probe_passed || emulator_untested ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_media_range_qualification(
    BrowserEngine *engine, const PspBootConfig *config,
    PspClockWorker *clock_worker, bool clock_worker_started)
{
    /*
     * The transport half of a playback session with nothing behind it.
     *
     * A range or fragment-window regression can only be seen today inside the
     * first second of a real playback session, where the resolver, the Media
     * Engine pool, mpeg_vsh and a firmware decoder are all between the reader
     * and the log. This runs the same resolve, the same two bounded windows,
     * the same moov/sidx reads and the same sequential sample reads, far
     * enough to cross fragment boundaries, and stops there. It needs the
     * network and nothing else.
     *
     * Unlike the probes above it does not destroy the engine: it runs after
     * the media session, text input and offline store have been built on the
     * engine's budget, and tearing the budget out from under them would report
     * a clean teardown this mode has not earned. The budget counters below are
     * the honest half -- they say whether the probe returned its own bytes.
     */
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    PspMediaRangeProbeReport report = {0};
    char probe_error[256] = {0};
    bool passed = psp_media_range_probe_run(
        budget, session, config->url, &report,
        probe_error, sizeof(probe_error));
    printf(
        "tilefinch-media-range-probe: event=%s resolved=%d split=%d "
        "video-boundaries=%u audio-boundaries=%u error=\"%s\"\n",
        passed ? "pass" : "fail", report.resolved ? 1 : 0,
        report.split ? 1 : 0, report.video.fragment_boundaries,
        report.audio.fragment_boundaries, probe_error);
    psp_report_budget_counters(budget, "media-range-probe-exit");
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           passed ? "clean-exit" : "qualification-failed",
           passed ? "media-range-pass" : "media-range-fail");
    psp_log_checkpoint("media-range-probe-complete");
    psp_log_finish(
        passed ? "media-range-probe-pass" : "media-range-probe-fail");
    psp_exit_console(config->exit_to);
    return passed ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_raster_qualification(
    BrowserEngine *engine, Budget *budget, const char *argv0,
    PspGlyphComponentSession *glyph_component,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING PAGE RASTER", 860);
    PspRasterFixtureReport report = {0};
    char error[256] = {0};
    bool passed = psp_raster_fixture_run(
        engine, &report, error, sizeof(error));
    unsigned pack_reads = 0;
    bool regional_requested = false;
    bool emoji_requested = false;
    if (passed && glyph_component != NULL
        && glyph_component->provider != NULL) {
        static const unsigned regional_probes[] = {
            0x6f22u, /* CJK */
            0x0416u, /* Cyrillic */
            0x1ed9u  /* Extended Latin */
        };
        uint32_t regional_key = 0, emoji_key = 0;
        unsigned width = 0;
        for (size_t probe = 0;
             probe < sizeof(regional_probes) / sizeof(regional_probes[0]);
             probe++) {
            if (tilefinch_glyph_provider_has_codepoint(
                    glyph_component->provider, regional_probes[probe],
                    &regional_key, &width)
                && tilefinch_glyph_provider_key_pending(
                    glyph_component->provider, regional_key)) {
                regional_requested = true;
                break;
            }
        }
        emoji_requested =
            tilefinch_glyph_provider_has_codepoint(
                glyph_component->provider, 0x1f600u,
                &emoji_key, &width)
            && tilefinch_glyph_provider_key_pending(
                glyph_component->provider, emoji_key);
        /* Each pass can expose another bounded block. Keep the qualification
           deterministic and below the device write/read discipline: this is
           at most eight 16 KiB reads, and the release player does no such
           eager scan. */
        for (unsigned pass = 0; pass < 8u; pass++) {
            if (!psp_glyph_component_session_pump_runtime(
                    glyph_component, engine)) break;
            pack_reads++;
            passed = psp_raster_fixture_rerender(
                engine, &report, error, sizeof(error));
            if (!passed) break;
        }
        const unsigned attached_pack_mask =
            (1u << TILEFINCH_GLYPH_PACK_COUNT) - 1u;
        const unsigned language_pack_mask = attached_pack_mask
            & ~(1u << TILEFINCH_GLYPH_PACK_COLOR_EMOJI);
        bool regional_attached =
            (glyph_component->attached_mask & language_pack_mask) != 0;
        bool emoji_attached =
            (glyph_component->attached_mask
             & (1u << TILEFINCH_GLYPH_PACK_COLOR_EMOJI)) != 0;
        if ((regional_attached && !regional_requested)
            || (emoji_attached
                && (!emoji_requested
                    || report.fallback_authored_color_pixels == 0))) {
            passed = false;
            snprintf(error, sizeof(error),
                     "optional glyph raster not exercised regional=%u "
                     "emoji=%u color=%zu",
                     regional_requested ? 1u : 0u,
                     emoji_requested ? 1u : 0u,
                     report.fallback_authored_color_pixels);
        }
    }
    BrowserFrameView frame = {0};
    bool dumped = passed
        && browser_engine_frame_view(engine, &frame)
        && psp_dump_frame_strided_named(
            argv0, "frame-raster.ppm", frame.pixels,
            frame.pixel_count, (size_t) frame.stride);
    printf(
        "tilefinch-raster-fixture: event=%s hash=%016llx "
        "round=%zu/%zu stroke=%zu/%zu fallback=%zu color=%zu "
        "pack-reads=%u regional=%u emoji=%u italic=%zu/%d dump=%d "
        "error=\"%s\"\n",
        passed ? "psp-pass" : "psp-fail",
        (unsigned long long) report.frame_hash,
        report.rounded_interior_pixels, report.rounded_edge_pixels,
        report.stroke_interior_pixels, report.stroke_edge_pixels,
        report.fallback_ink_pixels,
        report.fallback_authored_color_pixels, pack_reads,
        regional_requested ? 1u : 0u, emoji_requested ? 1u : 0u,
        report.italic_continuous_pairs,
        report.italic_maximum_centroid_step, dumped ? 1 : 0, error);
    psp_report_budget_counters(budget, "raster-fixture-exit");
    psp_glyph_component_session_destroy(glyph_component);
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf(
        "tilefinch-validation: outcome=%s qualification=%s\n",
        passed ? "clean-exit" : "qualification-failed",
        passed ? "psp-raster-pass" : "psp-raster-fail");
    psp_log_checkpoint("raster-fixture-complete");
    psp_log_finish(
        passed ? "raster-fixture-pass" : "raster-fixture-fail");
    psp_exit_console(exit_to);
    return passed ? 0 : 1;
}

/* The qualification modes that need only the engine. The order is boot's:
   the media fixture, the GE presenter probe, the JavaScript benchmark (which
   returns to boot), the WebGL GE probe and the CSC order probe. */
bool psp_validation_engine_qualifications(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code)
{
    if (process->config.validation_media_fixture_auto != 0) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO HARDWARE", 860);
        PspMediaFixtureReport fixture_report = {0};
        char fixture_error[256] = {0};
        bool fixture_passed = psp_media_fixture_run(
            browser->budget, &fixture_report,
            fixture_error, sizeof(fixture_error));
        bool emulator_untested = !fixture_passed
            && (fixture_report.last_native_error
                    == MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE
                || strstr(fixture_error, "raw-NAL") != NULL
                || strstr(fixture_error, "0x80010002") != NULL
                || strstr(fixture_error, "80010002") != NULL);
        printf(
            "tilefinch-media-fixture: event=%s error=\"%s\" "
            "clips=%u decoded=%u observed=%u native=0x%08X\n",
            fixture_passed ? "hardware-pass"
                : (emulator_untested ? "emulator-untested"
                                     : "hardware-fail"),
            fixture_error,
            fixture_report.clips_completed,
            fixture_report.frames_decoded,
            fixture_report.frames_observed,
            (unsigned) fixture_report.last_native_error);
        psp_report_budget_counters(browser->budget, "media-fixture-exit");
        browser_engine_destroy(browser->engine);
        browser->engine = NULL;
        if (process->clock_live) {
            (void) psp_clock_worker_shutdown(&process->clock_worker);
            process->clock_live = false;
        }
        printf(
            "tilefinch-validation: outcome=%s qualification=%s\n",
            fixture_passed || emulator_untested
                ? "clean-exit" : "qualification-failed",
            fixture_passed ? "hardware-pass"
                : (emulator_untested ? "emulator-untested"
                                     : "hardware-fail"));
        psp_log_checkpoint("media-fixture-complete");
        psp_log_finish(
            fixture_passed ? "media-fixture-pass"
                : (emulator_untested ? "media-fixture-untested"
                                     : "media-fixture-fail"));
        psp_exit_console(process->config.exit_to);
        *exit_code = fixture_passed || emulator_untested ? 0 : 1;
        return true;
    }
    if (process->config.validation_ge_present_probe != 0) {
        *exit_code = psp_run_ge_present_qualification(
            browser->engine, browser->budget, &process->clock_worker, process->clock_live,
            process->config.exit_to);
        return true;
    }
    if (process->config.validation_js_bench != 0)
        psp_run_js_bench(argv0, &process->config);
    if (process->config.validation_webgl_ge_probe != 0) {
        *exit_code = psp_run_webgl_ge_qualification(
            browser->engine, browser->budget, &process->clock_worker,
            process->clock_live, process->config.exit_to);
        return true;
    }
    if (process->config.validation_csc_order_probe != 0) {
        *exit_code = psp_run_csc_order_qualification(
            browser->engine, browser->budget, &process->clock_worker, process->clock_live,
            process->config.exit_to);
        return true;
    }
    return false;
}

/* Logs which glyph packs boot attached, then runs the page-raster fixture,
   which needs them. */
bool psp_validation_raster_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code)
{
    printf(
        "tilefinch-glyph-component: language=%u color=%u "
        "installed=0x%02x attached=0x%02x\n",
        (unsigned) browser_profile_glyph_language(browser->profile),
        browser_profile_color_emoji(browser->profile) ? 1u : 0u,
        (unsigned) browser->glyph_component_session->installed_mask,
        (unsigned) browser->glyph_component_session->attached_mask);
    if (process->config.validation_raster_fixture_auto == 0) return false;
    *exit_code = psp_run_raster_qualification(
        browser->engine, browser->budget, argv0,
        browser->glyph_component_session,
        &process->clock_worker, process->clock_live,
        process->config.exit_to);
    return true;
}

/* The one validation mode that has to be dispatched apart from the others:
   it is the only one that needs the network, and main calls this at the
   first point at which the network is up. */
bool psp_validation_network_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    int *exit_code)
{
    if (process->config.validation_media_range_probe == 0) return false;
    *exit_code = psp_run_media_range_qualification(
        browser->engine, &process->config, &process->clock_worker,
        process->clock_live);
    return true;
}

/* Boot reports and boot-time validation settings. */
TILEFINCH_COLD_PATH void psp_log_engine_creation(
    const BrowserEngine *engine)
{
    BrowserEngineCreationMetrics metrics = {0};
    if (!browser_engine_creation_metrics(engine, &metrics)) return;
    psp_log_printf(
        "tilefinch-engine-create: control=%lluus session=%lluus "
        "blocker=%lluus navigation=%lluus fonts=%lluus "
        "framebuffer=%lluus\n",
        (unsigned long long) metrics.control_ready_us,
        (unsigned long long) metrics.session_ready_us,
        (unsigned long long) metrics.blocker_ready_us,
        (unsigned long long) metrics.navigation_ready_us,
        (unsigned long long) metrics.fonts_ready_us,
        (unsigned long long) metrics.framebuffer_ready_us);
}

TILEFINCH_OUT_OF_LINE void psp_boot_input_probe(uint64_t sample_us)
{
    static uint64_t started_us;
    if (sample_us == 0) {
        started_us = (uint64_t) sceKernelGetSystemTimeWide();
    } else if (started_us != 0) {
        printf("tilefinch-boot-input: first-sample=%lluus entry-wait=%lluus\n",
               (unsigned long long) sample_us,
               (unsigned long long) (sample_us - started_us));
        started_us = 0;
    }
}


TILEFINCH_COLD_PATH void psp_boot_report_handoff(
    int argc, char *argv[], uint64_t entered_us, uint64_t surface_us)
{
    PspBootHandoffTiming timing;
    if (argc != 2 || argv == NULL || surface_us < entered_us
        || !psp_boot_timing_decode(argv[1], entered_us, &timing)) {
        printf("tilefinch-boot-handoff: valid=0 clock=rtc-us\n");
        return;
    }
    const char *source = argv[0] != NULL && strncmp(argv[0], "ms0:", 4) == 0
        ? "memory-stick" : argv[0] != NULL && strncmp(argv[0], "host0:", 6) == 0
        ? "usb" : "other";
    printf("tilefinch-boot-handoff: valid=1 clock=rtc-us source=%s "
           "launcher-us=%lluus loadexec-us=%lluus "
           "entry-to-scanout-us=%lluus handoff-to-scanout-us=%lluus\n",
           source,
           (unsigned long long) (timing.handoff_us - timing.launcher_us),
           (unsigned long long) (entered_us - timing.handoff_us),
           (unsigned long long) (surface_us - entered_us),
           (unsigned long long) (surface_us - timing.handoff_us));
}

/* One boot per process: the clock main starts on entry. */
static struct {
    uint64_t entered_us;
    uint64_t previous_us;
} psp_boot_timing;

TILEFINCH_COLD_PATH void psp_boot_timing_begin(uint64_t entered_us)
{
    psp_boot_timing.entered_us = entered_us;
    psp_boot_timing.previous_us = entered_us;
}

TILEFINCH_COLD_PATH void psp_boot_timing_mark(const char *stage)
{
    if (stage == NULL) return;
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    printf("tilefinch-boot-timing: stage=%s elapsed=%lluus delta=%lluus\n",
           stage, (unsigned long long) (now_us - psp_boot_timing.entered_us),
           (unsigned long long) (now_us - psp_boot_timing.previous_us));
    psp_boot_timing.previous_us = now_us;
}
TILEFINCH_COLD_PATH void psp_report_heap_capacity_lower_bound(void)
{
    size_t capacity_lower_bound =
        media_psp_backend_heap_capacity_lower_bound();
    printf("tilefinch-psp-script: heap-capacity-lower-bound=%uMB\n",
           (unsigned) (capacity_lower_bound / MIB));
}

#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)
TILEFINCH_COLD_PATH void psp_preload_validation_ca_bundle(
    const TilefinchInstallPaths *paths)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    bool ready = paths != NULL
        && tilefinch_install_program_path(
            paths, "roots.pem", path, sizeof(path))
        && fetch_set_ca_bundle_path(path)
        && fetch_prepare_ca_bundle();
    printf("tilefinch-network: validation-ca-preload=%s\n",
           ready ? "ready" : "unavailable-fail-closed");
}
#endif
TILEFINCH_COLD_PATH void psp_report_tls_assets(
    size_t bundle_bytes,
    const uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES],
    time_t tls_time, const PspTimeFields *rtc, PspTimeStatus rtc_status)
{
    printf("tilefinch-network: trust-bytes=%zu trust-sha256="
           "%02x%02x%02x%02x%02x%02x%02x%02x "
           "verification=required utc-epoch=%ld "
           "rtc=%04u-%02u-%02uT%02u:%02u:%02uZ rtc-status=%s "
           "curl=%s tls=%s http2=%s http2-enabled=%u\n",
           bundle_bytes,
           digest[0], digest[1], digest[2], digest[3],
           digest[4], digest[5], digest[6], digest[7],
           (long) tls_time, rtc->year, rtc->month, rtc->day,
           rtc->hour, rtc->minute, rtc->second,
           psp_time_status_name(rtc_status),
           fetch_transport_version(),
           fetch_transport_tls_version(),
           fetch_transport_http2_version(),
           fetch_transport_http2_available() ? 1u : 0u);
}

/* Opt-in final readback after the measured input sequence, on the
   presentation owner. Unlike the small temporal ring it keeps every RGB565
   pixel, without retaining another frame allocation. Out of line: main's
   size is an instruction-cache ratchet. */
__attribute__((noinline, cold))
void psp_dump_final_frame(const char *argv0)
{
    bool dumped = psp_dump_frame_strided_named(
        argv0, "frame-device-final.ppm",
        psp_display_front_buffer(&psp_display),
        PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
    printf("tilefinch-psp-script: final-frame-dumped=%d\n", dumped ? 1 : 0);
}


/* boot.cfg's validation-only engine environment (newlib setenv works
   in-process): profiler, replay pump and measurement switches. */
void psp_validation_apply_env_knobs(const PspBootConfig *config)
{
    char scratch[32];
    tilefinch_game_audio_validation_mute_output(config->validation_game_audio_mute == 1);
    printf("tilefinch-game-audio: validation-output-muted=%ld\n",
           config->validation_game_audio_mute);
    /* Same EBOOT, same page and replay, with or without JS call wrappers and
       interrupt-time sampling. The setting comes only from boot.cfg. */
    int profile_set_status = setenv(
        "TILEFINCH_VALIDATION_JS_PROFILE",
        config->validation_js_profile == 0 ? "0" : "1", 1);
    const char *profile_setting = getenv("TILEFINCH_VALIDATION_JS_PROFILE");
    printf("tilefinch-js-profiler: requested=%ld enabled=%d set-status=%d\n",
           config->validation_js_profile,
           profile_setting == NULL || strcmp(profile_setting, "0") != 0,
           profile_set_status);
    if (config->validation_js_outlier_us > 0) {
        snprintf(scratch, sizeof(scratch), "%ld",
                 config->validation_js_outlier_us);
        int outlier_set_status = setenv(
            "TILEFINCH_JS_PROFILE_OUTLIER_US", scratch, 1);
        printf("tilefinch-js-outlier-config: threshold-us=%ld set-status=%d\n",
               config->validation_js_outlier_us, outlier_set_status);
    }
    if (config->validation_script_split != 2) {
        int split_set_status = setenv(
            "TILEFINCH_SCRIPT_SPLIT",
            config->validation_script_split == 0 ? "0" : "1", 1);
        printf("tilefinch-script-split: mode=%ld set-status=%d\n",
               config->validation_script_split, split_set_status);
    }
    /* Distinguish a sampler-off run from an interpreter with no hooks. */
    printf("tilefinch-execution-census-build: compiled=%d requested=%ld\n",
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
           1,
#else
           0,
#endif
           config->validation_execution_census);
    if (config->validation_execution_census != 0) {
        int census_status = setenv("TILEFINCH_EXECUTION_CENSUS",
            config->validation_execution_census == 2 ? "2" : "1", 1);
        printf("tilefinch-execution-census-config: mode=%ld set-status=%d\n",
               config->validation_execution_census, census_status);
    }
    if (config->validation_gc_pacing == 0) {
        int pacing_set_status = setenv("TILEFINCH_JS_GC_PACING", "0", 1);
        printf("tilefinch-gc-pacing: mode=0 set-status=%d\n",
               pacing_set_status);
    }
    if (config->trace_ignore_request_body == 1) {
        int body_set_status = setenv(
            "TILEFINCH_REPLAY_IGNORE_REQUEST_BODY", "1", 1);
        printf("tilefinch-trace-replay: ignore-request-body=1 set-status=%d\n",
               body_set_status);
    }
    if (config->trace_volatile_uuids == 1) {
        int uuid_set_status = setenv(
            "TILEFINCH_REPLAY_VOLATILE_UUIDS", "1", 1);
        printf("tilefinch-trace-replay: volatile-uuids=1 set-status=%d\n",
               uuid_set_status);
    }
    if (config->trace_replay_pump_us > 0) {
        char pump_us[16];
        snprintf(pump_us, sizeof(pump_us), "%ld",
                 config->trace_replay_pump_us);
        int pump_set_status = setenv(
            "TILEFINCH_REPLAY_PUMP_US", pump_us, 1);
        printf("tilefinch-trace-replay: pump-us=%s set-status=%d\n",
               pump_us, pump_set_status);
    }
}
