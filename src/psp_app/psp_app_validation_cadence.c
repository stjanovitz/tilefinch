/* Canvas, WebGL and page-runtime measurement for the validation EBOOT.
 *
 * The causal canvas cadence record (one sample per presented canvas frame,
 * with its runtime, render, present and peer-thread CPU phases), the
 * per-frame runtime record behind each tilefinch-loop-work line, the
 * `webgl-measure-*` / `video-measure-*` input-script marks, and the
 * runtime-cadence summary. Production steps reach it through the probe
 * hooks in psp_app_validation.h.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_validation.h"
#include "tilefinch_compiler.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/work_ledger.h"
#include "tilefinch/psp_raster_fixture.h"
#include "psp_thread_contract.h"

#include <stdatomic.h>

/* Set by the canvas-followup-* and canvas-trace-* input-script marks. */
static bool psp_validation_canvas_followup_enabled = true;
static bool psp_validation_canvas_live_trace;

static const PspDisplayBackend *psp_validation_display_inner;
static PspDisplayBackendTiming psp_validation_display_timing;
/* Observations only: publication order and framebuffer ownership stay in
   psp_display.c. Vcounts distinguish a missed scanout from time spent waiting. */
static uint64_t psp_validation_display_ready_us;
static uint64_t psp_validation_display_latched_us;
static uint32_t psp_validation_display_ready_vcount;
static uint32_t psp_validation_display_latched_vcount;

static void *psp_validation_display_compose_base(void)
{
    return psp_validation_display_inner->compose_base();
}

static void psp_validation_display_flush(void *address, size_t bytes)
{
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    psp_validation_display_inner->flush_range(address, bytes);
    psp_validation_display_timing.flush_us +=
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    psp_validation_display_timing.flush_calls++;
}

static int psp_validation_display_set_mode(
    int mode, int width, int height)
{
    return psp_validation_display_inner->set_mode(mode, width, height);
}

static int psp_validation_display_set_framebuffer(
    void *address, int stride, int format, int sync)
{
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    psp_validation_display_ready_us = started_us;
    psp_validation_display_ready_vcount = (uint32_t) sceDisplayGetVcount();
    int result = psp_validation_display_inner->set_frame_buffer(
        address, stride, format, sync);
    psp_validation_display_timing.set_framebuffer_us +=
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    psp_validation_display_timing.set_framebuffer_calls++;
    return result;
}

static void psp_validation_display_wait_vblank(void)
{
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    /* Owner-thread publications only: the ledger ignores the callback
       presenter. */
    unsigned ledger = work_ledger_enter(WORK_LEDGER_SLEEP);
    psp_validation_display_inner->wait_vblank();
    psp_validation_display_latched_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    psp_validation_display_latched_vcount = (uint32_t) sceDisplayGetVcount();
    work_ledger_leave(ledger);
    psp_validation_display_timing.vblank_us +=
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    psp_validation_display_timing.vblank_calls++;
}

static void psp_validation_display_wait_vblank_serviced(
    void *buffer, int stride, int format,
    PspDisplayWaitService service, void *context)
{
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    /* This interval includes useful decoder work, not only sleep. Do not
       charge the complete serviced wait to WORK_LEDGER_SLEEP. */
    if (psp_validation_display_inner->wait_vblank_serviced != NULL)
        psp_validation_display_inner->wait_vblank_serviced(
            buffer, stride, format, service, context);
    else
        psp_validation_display_inner->wait_vblank();
    psp_validation_display_latched_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    psp_validation_display_latched_vcount = (uint32_t) sceDisplayGetVcount();
    psp_validation_display_timing.vblank_us +=
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    psp_validation_display_timing.vblank_calls++;
}

static const PspDisplayBackend psp_validation_display_backend = {
    .compose_base = psp_validation_display_compose_base,
    .flush_range = psp_validation_display_flush,
    .set_mode = psp_validation_display_set_mode,
    .set_frame_buffer = psp_validation_display_set_framebuffer,
    .wait_vblank = psp_validation_display_wait_vblank,
    .wait_vblank_serviced = psp_validation_display_wait_vblank_serviced
};

const PspDisplayBackend *psp_validation_display_wrap(
    const PspDisplayBackend *inner)
{
    psp_validation_display_inner = inner;
    memset(&psp_validation_display_timing, 0,
           sizeof(psp_validation_display_timing));
    return inner == NULL ? NULL : &psp_validation_display_backend;
}

bool psp_display_validation_timing_snapshot(PspDisplayBackendTiming *timing)
{
    if (timing == NULL || psp_validation_display_inner == NULL) return false;
    *timing = psp_validation_display_timing;
    return true;
}
static uint64_t psp_runtime_advance_total_us;
static uint64_t psp_runtime_advance_maximum_us;
static size_t psp_runtime_advance_calls;
static size_t psp_runtime_layout_changes;
static size_t psp_runtime_render_deferrals;
static size_t psp_runtime_handoff_deferrals;
static size_t psp_runtime_reclaim_deferrals;

PspFrameRuntime psp_frame_runtime;

#define PSP_CANVAS_CADENCE_SAMPLE_LIMIT 1024u
#define PSP_CANVAS_CADENCE_SLOW_LIMIT 16u
#define PSP_CANVAS_GAME_SAMPLE_LIMIT 512u
#define PSP_CANVAS_GAME_WORDS 96u
#define PSP_CANVAS_LOOP_GAP_LIMIT 64u
_Static_assert(PSP_CANVAS_GAME_WORDS <= SCRIPT_VALIDATION_FRAME_WORD_LIMIT,
               "canvas packet exceeds runtime observation bound");
typedef struct {
    uint64_t start_us, end_us;
    uint32_t wait_us, runtime_us, callbacks;
    int64_t due_us;
    bool timer_known, timer_frame, fast_followup;
} PspCanvasLoopGap;

/* Intrusive attribution only. Discover outside the timing window and sample
   bounded peer CPU clocks at runtime boundaries, never inside JavaScript. */
static SceUID psp_canvas_peer_ids[PSP_CANVAS_PEER_LIMIT];
static size_t psp_canvas_peer_count;
static bool psp_canvas_peer_enabled;

static void psp_canvas_peer_begin(bool enabled)
{
    psp_canvas_peer_enabled = false;
    psp_canvas_peer_count = 0;
    if (!enabled) return;
    SceUID ids[64];
    int count = 0;
    int result = sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count);
    if (result < 0 || count < 0) {
        printf("tilefinch-canvas-peer-list: available=0 result=0x%08lx\n",
               (unsigned long) result);
        return;
    }
    int visited = count > 64 ? 64 : count;
    size_t omitted = 0;
    SceUID owner = sceKernelGetThreadId();
    for (int at = 0; at < visited; at++) {
        SceKernelThreadInfo info;
        if (psp_thread_snapshot(ids[at], &info) < 0) continue;
        info.name[sizeof(info.name) - 1u] = '\0';
        bool selected = ids[at] != owner
            && (strstr(info.name, "tilefinch") != NULL
                || strstr(info.name, "Tilefinch") != NULL
                || strstr(info.name, "Callback") != NULL
                || strcmp(info.name, "callbacks") == 0
                || strcmp(info.name, "home-exit-watchdog") == 0
                || strstr(info.name, "usbhost") != NULL
                || strstr(info.name, "PSPLINK") != NULL);
        printf("tilefinch-canvas-thread: id=0x%08lx name=\"%s\" priority=%d selected=%u\n",
               (unsigned long) ids[at], info.name, info.currentPriority,
               selected ? 1u : 0u);
        if (!selected) continue;
        if (psp_canvas_peer_count == PSP_CANVAS_PEER_LIMIT) { omitted++; continue; }
        size_t slot = psp_canvas_peer_count++;
        psp_canvas_peer_ids[slot] = ids[at];
        printf("tilefinch-canvas-peer: slot=%lu id=0x%08lx name=\"%s\"\n",
               (unsigned long) slot, (unsigned long) ids[at], info.name);
    }
    psp_canvas_peer_enabled = psp_canvas_peer_count != 0;
    printf("tilefinch-canvas-peer-list: available=1 visited=%d total=%d peers=%lu omitted=%lu\n",
           visited, count, (unsigned long) psp_canvas_peer_count, (unsigned long) omitted);
}

static __attribute__((noinline)) PspCanvasPeerSample psp_canvas_peer_sample(void)
{
    PspCanvasPeerSample sample = {0};
    if (!psp_canvas_peer_enabled) return sample;
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    sample.query_begin_us = started;
    for (size_t at = 0; at < psp_canvas_peer_count; at++) {
        SceKernelThreadRunStatus status = {.size = sizeof(status)};
        if (sceKernelReferThreadRunStatus(psp_canvas_peer_ids[at], &status) < 0) continue;
        sample.run_us[at] = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
        sample.valid_mask |= UINT32_C(1) << at;
    }
    sample.query_end_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t span = sample.query_end_us - started;
    sample.span_us = span > UINT32_MAX ? UINT32_MAX : (uint32_t) span;
    return sample;
}

static PspCanvasCpuSample psp_canvas_cpu_sample(void)
{
    SceKernelThreadRunStatus status = {.size = sizeof(status)};
    PspCanvasCpuSample sample = {0};
    sample.query_begin_us = (uint64_t) sceKernelGetSystemTimeWide();
    int result = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status);
    sample.query_end_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (result < 0)
        return sample;
    /* runClocks is the firmware's microsecond run counter, not CPU cycles.
       A failed query must remain unavailable, never appear as zero CPU. */
    sample.run_us = ((uint64_t) status.runClocks.hi << 32)
        | status.runClocks.low;
    sample.interrupt_preempts = status.intrPreemptCount;
    sample.thread_preempts = status.threadPreemptCount;
    sample.releases = status.releaseCount;
    sample.valid = true;
    return sample;
}

/* Cumulative translator clocks already exist. Copy/difference them only at
   runtime batch boundaries; do not introduce clocks per vertex/instance. The
   instance matrix/color clocks are children of vertex, not sibling costs. */
#define PSP_CANVAS_WEBGL_FIELDS(X) \
    X(total_us, total) X(seed_us, seed) X(command_us, command) \
    X(command_decode_us, decode) X(command_cache_us, cache) \
    X(command_vertex_us, vertex) X(command_instance_matrix_us, matrix) \
    X(command_instance_color_us, color) X(command_state_us, state) \
    X(command_emit_us, emit) X(command_antialias_us, aa) \
    X(command_writeback_us, writeback) X(command_finalize_us, finalize) \
    X(sync_us, sync) X(readback_us, readback) X(frames, flushes) \
    X(vertices, vertices) X(fast_vertices, fast) \
    X(geometry_cache_hit_vertices, cached) X(geometry_cache_misses, misses) \
    X(draw_calls, draws) X(matrix_loads, loads) \
    X(matrix_loads_avoided, loads_saved) X(state_changes, changes) \
    X(state_changes_avoided, changes_saved) X(display_list_bytes, list_bytes) \
    X(antialias_draws, aa_draws) X(antialias_edge_indices, aa_indices)
enum {
#define PSP_CANVAS_WEBGL_ENUM(field, name) PSP_CANVAS_WEBGL_##name,
    PSP_CANVAS_WEBGL_FIELDS(PSP_CANVAS_WEBGL_ENUM)
#undef PSP_CANVAS_WEBGL_ENUM
    PSP_CANVAS_WEBGL_FIELD_COUNT
};
static const char *const psp_canvas_webgl_names[] = {
#define PSP_CANVAS_WEBGL_NAME(field, name) #name,
    PSP_CANVAS_WEBGL_FIELDS(PSP_CANVAS_WEBGL_NAME)
#undef PSP_CANVAS_WEBGL_NAME
};
typedef struct {
    uint32_t value[PSP_CANVAS_WEBGL_FIELD_COUNT];
    uint32_t samples, invalid;
} PspCanvasWebglSample;

typedef struct {
    ScriptRuntimeCpuMetrics value;
    uint32_t samples, invalid;
} PspCanvasRuntimeCpuSample;

typedef struct {
    uint32_t ordinal;
    uint16_t input_step;
    uint16_t input_buttons;
    uint32_t runtime_turns;
    uint32_t render_slices;
    uint32_t pipeline_us;
    uint32_t runtime_us;
    uint32_t runtime_cpu_us, runtime_cpu_samples;
    uint32_t runtime_cpu_window_min_us, runtime_cpu_window_max_us;
    uint32_t runtime_cpu_observer_us;
    uint32_t peer_cpu_us[PSP_CANVAS_PEER_LIMIT], peer_samples[PSP_CANVAS_PEER_LIMIT];
    uint32_t peer_span_us, peer_window_extra_us;
    uint32_t runtime_interrupt_preempts, runtime_thread_preempts;
    uint32_t runtime_releases;
    uint32_t script_runtime_us;
    uint32_t navigation_runtime_us;
    uint32_t damage_apply_us;
    uint32_t callback_us;
    uint32_t webgl_us;
    PspCanvasWebglSample webgl;
    PspCanvasRuntimeCpuSample runtime_cpu;
    uint32_t microtask_us;
    uint32_t render_us;
    uint32_t render_canvas_raster_us, render_canvas_overlay_us;
    uint32_t render_cpu_us, render_cpu_samples;
    uint64_t budget_begin_us, budget_end_us;
    uint32_t budget_owner_cpu_us, budget_wall_min_us, budget_wall_max_us;
    uint32_t budget_observer_us, budget_peer_span_us, budget_peer_extra_us;
    uint32_t budget_peer_cpu_us[PSP_CANVAS_PEER_LIMIT], budget_peer_mask;
    bool budget_valid;
    RenderCanvasConversionMetrics conversion;
    uint32_t conversion_samples;
    ScriptGameAudioNativeMetrics audio_native;
    uint32_t present_us;
    uint32_t present_base_us;
    uint32_t present_base_cpu_us, present_base_bytes;
    bool present_base_cpu_valid;
    uint32_t present_composite_us;
    uint32_t present_publish_us;
    uint32_t present_flush_us;
    uint32_t present_set_framebuffer_us;
    uint32_t present_vblank_us;
    uint32_t ui_focus_scroll_us;
    uint32_t ui_chrome_us;
    uint32_t ui_cursor_us;
    uint32_t ui_screen_us;
    uint32_t ui_toast_us;
    uint32_t ui_loading_us;
    uint32_t js_allocations;
    uint32_t js_frees;
    uint32_t js_reallocations;
    uint32_t js_allocated_bytes;
    uint32_t js_freed_bytes;
    uint32_t js_live_before;
    uint32_t js_live_after;
    uint64_t started_us, ready_us, latched_us, previous_latched_us;
    uint64_t game_origin_us;
    uint32_t ready_vcount, latched_vcount, previous_vcount;
    uint32_t packet_read_us;
    bool causal_window;
    bool game_packet_valid;
    uint64_t loop_start_us, loop_end_us;
    uint32_t loop_phase_us[PSP_CANVAS_LOOP_PHASES];
} PspCanvasPhaseSample;

typedef struct {
    uint32_t intervals_us[PSP_CANVAS_CADENCE_SAMPLE_LIMIT];
    uint32_t pipeline_us[PSP_CANVAS_CADENCE_SAMPLE_LIMIT];
    PspCanvasPhaseSample phases[PSP_CANVAS_CADENCE_SAMPLE_LIMIT];
    uint32_t game_words[PSP_CANVAS_GAME_SAMPLE_LIMIT][PSP_CANVAS_GAME_WORDS];
    uint32_t sorted_us[PSP_CANVAS_CADENCE_SAMPLE_LIMIT];
    PspCanvasPhaseSample slow_phases[PSP_CANVAS_CADENCE_SLOW_LIMIT];
    size_t interval_count;
    size_t pipeline_count;
    size_t slow_phase_count;
    size_t pipeline_total;
    size_t pipelines_over_34ms;
    size_t presentations;
    size_t interval_samples_dropped;
    size_t pipeline_samples_dropped;
    size_t intervals_at_60hz;
    size_t intervals_at_30hz;
    uint64_t previous_present_us;
    uint64_t previous_latched_us;
    uint32_t previous_vcount;
    bool causal_window;
    /* Set by webgl-measure-end: later presentations are not measured. */
    bool window_closed;
    size_t causal_loops_without_sample;
    PspCanvasLoopGap causal_loop_gaps[PSP_CANVAS_LOOP_GAP_LIMIT];
    uint64_t pending_started_us;
    uint64_t pending_runtime_us;
    uint64_t pending_runtime_cpu_us;
    uint64_t pending_runtime_cpu_window_min_us, pending_runtime_cpu_window_max_us;
    uint64_t pending_runtime_cpu_observer_us;
    uint64_t pending_peer_cpu_us[PSP_CANVAS_PEER_LIMIT];
    uint32_t pending_peer_samples[PSP_CANVAS_PEER_LIMIT];
    uint64_t pending_peer_span_us;
    uint64_t pending_peer_window_extra_us;
    uint32_t pending_runtime_cpu_samples;
    uint32_t pending_runtime_interrupt_preempts;
    uint32_t pending_runtime_thread_preempts, pending_runtime_releases;
    uint64_t pending_script_runtime_us;
    uint64_t pending_navigation_runtime_us;
    uint64_t pending_damage_apply_us;
    uint32_t pending_runtime_turns;
    uint32_t pending_render_slices;
    uint64_t pending_callback_us;
    uint64_t pending_webgl_us;
    PspCanvasWebglSample pending_webgl;
    PspCanvasRuntimeCpuSample pending_cpu;
    uint64_t pending_microtask_us;
    uint64_t pending_render_us;
    uint64_t pending_render_canvas_raster_us, pending_render_canvas_overlay_us;
    uint64_t pending_render_cpu_us;
    uint32_t pending_render_cpu_samples;
    RenderCanvasConversionMetrics pending_conversion;
    uint32_t pending_conversion_samples;
    uint64_t pending_js_allocations;
    uint64_t pending_js_frees;
    uint64_t pending_js_reallocations;
    uint64_t pending_js_allocated_bytes;
    uint64_t pending_js_freed_bytes;
    size_t pending_js_live_before;
    size_t pending_js_live_after;
    uint16_t pending_input_step;
    uint16_t pending_input_buttons;
    uint64_t maximum_interval_us;
    uint64_t maximum_pipeline_us;
    PspCanvasPhaseSample maximum_phase;
    bool maximum_phase_valid;
} PspCanvasCadence;

static PspCanvasCadence psp_canvas_cadence;
static bool psp_validation_native_detail;
static const NavigationSession *psp_webgl_measurement_navigation;
/* Marks can arrive on the callback supervisor or during nested native UI.
   They queue only an integer; live realm reads belong to loop entry below. */
static atomic_uint psp_audio_slots_receipt_phase;

static TILEFINCH_OUT_OF_LINE void psp_audio_slots_receipt(
    const NavigationSession *navigation, bool requested_native)
{
    unsigned phase = atomic_exchange_explicit(
        &psp_audio_slots_receipt_phase, 0u, memory_order_acquire);
    if (phase == 0u) return;
    const char *name = phase == 1u ? "pre" : phase == 2u ? "post" : "end";
    const ScriptRuntime *runtime = navigation == NULL ? NULL : navigation->page.runtime;
    ScriptGameAudioStatus status = {0};
    bool available = script_runtime_game_audio_status(runtime, &status);
    bool selected_matches = available && status.selected_native == requested_native;
    bool ready = selected_matches && status.lazy_installed && !status.failed
        && status.native_present == status.selected_native;
    printf("tilefinch-audio-slots-status: phase=%s deferred=1 owner-checkpoint=1 "
           "runtime=%p navigation-generation=%llu available=%u requested=%u selected=%u installed=%u "
           "native=%u failed=%u selected-matches=%u ready=%u\n",
           name, (const void *) runtime,
           (unsigned long long) (navigation == NULL ? 0u : navigation->generation),
           available ? 1u : 0u,
           requested_native ? 1u : 0u, status.selected_native ? 1u : 0u,
           status.lazy_installed ? 1u : 0u, status.native_present ? 1u : 0u,
           status.failed ? 1u : 0u, selected_matches ? 1u : 0u, ready ? 1u : 0u);
}

typedef struct {
    PspCanvasCpuSample owner;
    PspCanvasPeerSample peers;
    uint32_t sequence;
} PspCanvasBudgetBoundary;
static PspCanvasBudgetBoundary psp_canvas_budget_begin, psp_canvas_budget_end;

static TILEFINCH_OUT_OF_LINE void psp_canvas_budget_pre_publish(uint32_t sequence)
{
    if (!psp_canvas_cadence.causal_window || !psp_canvas_peer_enabled
        || psp_canvas_cadence.pending_started_us == 0u) return;
    /* Peer counters contain the owner interval, just like runtime sampling.
       There is no clock query inside a ray, vertex, or audio-sample loop. */
    psp_canvas_budget_end.owner = psp_canvas_cpu_sample();
    psp_canvas_budget_end.peers = psp_canvas_peer_sample();
    psp_canvas_budget_end.sequence = sequence;
}

static TILEFINCH_OUT_OF_LINE void psp_canvas_budget_after_present(void)
{
    if (!psp_canvas_cadence.causal_window || !psp_canvas_peer_enabled) return;
    /* Begin after the preceding presentation's bookkeeping. The report
       keeps absolute query boundaries so the small omitted latch-to-start
       and pre-publish-to-ready tails remain visible, not silently subtracted. */
    psp_canvas_budget_begin.peers = psp_canvas_peer_sample();
    psp_canvas_budget_begin.owner = psp_canvas_cpu_sample();
    memset(&psp_canvas_budget_end, 0, sizeof(psp_canvas_budget_end));
}
static uint32_t psp_canvas_cadence_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t) value;
}

static TILEFINCH_OUT_OF_LINE void psp_canvas_budget_copy(
    PspCanvasPhaseSample *sample, const PspPresentPhaseTiming *timing)
{
    const PspCanvasCpuSample *before = &psp_canvas_budget_begin.owner;
    const PspCanvasCpuSample *after = &psp_canvas_budget_end.owner;
    if (sample == NULL || timing == NULL || !before->valid || !after->valid
        || timing->sequence != psp_canvas_budget_end.sequence
        || after->run_us < before->run_us
        || after->query_begin_us < before->query_end_us) return;
    sample->budget_valid = true;
    sample->budget_begin_us = before->query_end_us;
    sample->budget_end_us = after->query_begin_us;
    sample->budget_owner_cpu_us = psp_canvas_cadence_u32(after->run_us - before->run_us);
    sample->budget_wall_min_us = psp_canvas_cadence_u32(
        after->query_begin_us - before->query_end_us);
    sample->budget_wall_max_us = psp_canvas_cadence_u32(
        after->query_end_us - before->query_begin_us);
    sample->budget_observer_us = psp_canvas_cadence_u32(
        before->query_end_us - before->query_begin_us
        + after->query_end_us - after->query_begin_us);
    const PspCanvasPeerSample *peer_before = &psp_canvas_budget_begin.peers;
    const PspCanvasPeerSample *peer_after = &psp_canvas_budget_end.peers;
    sample->budget_peer_span_us = psp_canvas_cadence_u32(
        (uint64_t) peer_before->span_us + peer_after->span_us);
    if (peer_before->query_begin_us <= before->query_begin_us
        && peer_after->query_end_us >= after->query_end_us)
        sample->budget_peer_extra_us = psp_canvas_cadence_u32(
            before->query_begin_us - peer_before->query_begin_us
            + peer_after->query_end_us - after->query_end_us);
    for (size_t at = 0; at < psp_canvas_peer_count; at++) {
        uint32_t bit = UINT32_C(1) << at;
        if ((peer_before->valid_mask & peer_after->valid_mask & bit) == 0u
            || peer_after->run_us[at] < peer_before->run_us[at]) continue;
        sample->budget_peer_mask |= bit;
        sample->budget_peer_cpu_us[at] = psp_canvas_cadence_u32(
            peer_after->run_us[at] - peer_before->run_us[at]);
    }
}

static void psp_canvas_cadence_started(
    uint64_t started_us, uint64_t runtime_us, uint64_t callback_us,
    uint64_t webgl_us, uint64_t microtask_us, uint64_t script_runtime_us,
    uint64_t navigation_runtime_us, uint64_t damage_apply_us,
    uint64_t js_allocations, uint64_t js_frees,
    uint64_t js_reallocations, uint64_t js_allocated_bytes,
    uint64_t js_freed_bytes, size_t js_live_before, size_t js_live_after)
{
    if (psp_canvas_cadence.pending_started_us == 0) {
        psp_canvas_cadence.pending_started_us = started_us;
        psp_canvas_cadence.pending_runtime_turns = 1;
        psp_canvas_cadence.pending_runtime_us = runtime_us;
        psp_canvas_cadence.pending_script_runtime_us = script_runtime_us;
        psp_canvas_cadence.pending_navigation_runtime_us =
            navigation_runtime_us;
        psp_canvas_cadence.pending_damage_apply_us = damage_apply_us;
        psp_canvas_cadence.pending_callback_us = callback_us;
        psp_canvas_cadence.pending_webgl_us = webgl_us;
        psp_canvas_cadence.pending_microtask_us = microtask_us;
        psp_canvas_cadence.pending_render_us = 0;
        psp_canvas_cadence.pending_render_canvas_raster_us = 0;
        psp_canvas_cadence.pending_render_canvas_overlay_us = 0;
        psp_canvas_cadence.pending_render_cpu_us = 0;
        psp_canvas_cadence.pending_render_cpu_samples = 0;
        memset(&psp_canvas_cadence.pending_conversion, 0,
               sizeof(psp_canvas_cadence.pending_conversion));
        psp_canvas_cadence.pending_conversion_samples = 0;
        psp_canvas_cadence.pending_js_allocations = js_allocations;
        psp_canvas_cadence.pending_js_frees = js_frees;
        psp_canvas_cadence.pending_js_reallocations = js_reallocations;
        psp_canvas_cadence.pending_js_allocated_bytes = js_allocated_bytes;
        psp_canvas_cadence.pending_js_freed_bytes = js_freed_bytes;
        psp_canvas_cadence.pending_js_live_before = js_live_before;
        psp_canvas_cadence.pending_js_live_after = js_live_after;
        psp_canvas_cadence.pending_input_step =
            psp_input_script_diagnostic_step();
        psp_canvas_cadence.pending_input_buttons = (uint16_t)
            psp_input_script_diagnostic_buttons();
    } else {
        /* Every turn contributes deltas, not cumulative totals. Previously
           multi-turn frames counted turns but lost all later phase work. */
        psp_canvas_cadence.pending_runtime_turns++;
        psp_canvas_cadence.pending_runtime_us += runtime_us;
        psp_canvas_cadence.pending_script_runtime_us += script_runtime_us;
        psp_canvas_cadence.pending_navigation_runtime_us += navigation_runtime_us;
        psp_canvas_cadence.pending_damage_apply_us += damage_apply_us;
        psp_canvas_cadence.pending_callback_us += callback_us;
        psp_canvas_cadence.pending_webgl_us += webgl_us;
        psp_canvas_cadence.pending_microtask_us += microtask_us;
        psp_canvas_cadence.pending_js_allocations += js_allocations;
        psp_canvas_cadence.pending_js_frees += js_frees;
        psp_canvas_cadence.pending_js_reallocations += js_reallocations;
        psp_canvas_cadence.pending_js_allocated_bytes += js_allocated_bytes;
        psp_canvas_cadence.pending_js_freed_bytes += js_freed_bytes;
        psp_canvas_cadence.pending_js_live_after = js_live_after;
    }
}

static void psp_canvas_cadence_webgl(
    const ScriptWebglNativeMetrics *before,
    const ScriptWebglNativeMetrics *after, bool available)
{
    if (!psp_canvas_cadence.causal_window) return;
    PspCanvasWebglSample *sample = &psp_canvas_cadence.pending_webgl;
    if (!available) { sample->invalid++; return; }
#define PSP_CANVAS_WEBGL_CHECK(field, name) \
    if (after->field < before->field \
        || (uint64_t) (after->field - before->field) \
            > UINT32_MAX - sample->value[PSP_CANVAS_WEBGL_##name]) { \
        sample->invalid++; return; \
    }
    PSP_CANVAS_WEBGL_FIELDS(PSP_CANVAS_WEBGL_CHECK)
#undef PSP_CANVAS_WEBGL_CHECK
#define PSP_CANVAS_WEBGL_ADD(field, name) \
    sample->value[PSP_CANVAS_WEBGL_##name] += \
        (uint32_t) (after->field - before->field);
    PSP_CANVAS_WEBGL_FIELDS(PSP_CANVAS_WEBGL_ADD)
#undef PSP_CANVAS_WEBGL_ADD
    sample->samples++;
}

static void psp_canvas_cadence_runtime_cpu(const ScriptRuntimeCpuMetrics *before,
    const ScriptRuntimeCpuMetrics *after, bool available)
{
    if (!psp_canvas_cadence.causal_window) return;
    PspCanvasRuntimeCpuSample *s = &psp_canvas_cadence.pending_cpu;
    if (!available) return; /* Disabled measurement is not a bad clock. */
    if (after->errors != before->errors) {s->invalid++; return;}
    for (unsigned at = 0; at < SCRIPT_RUNTIME_CPU_PHASES; at++) {
        if (after->inclusive_cpu_us[at] < before->inclusive_cpu_us[at]
            || after->exclusive_cpu_us[at] < before->exclusive_cpu_us[at]
            || after->calls[at] < before->calls[at]) {s->invalid++; return;}
    }
    for (unsigned at = 0; at < SCRIPT_RUNTIME_CPU_PHASES; at++) {
        s->value.inclusive_cpu_us[at] += after->inclusive_cpu_us[at] - before->inclusive_cpu_us[at];
        s->value.exclusive_cpu_us[at] += after->exclusive_cpu_us[at] - before->exclusive_cpu_us[at];
        s->value.calls[at] += after->calls[at] - before->calls[at];
    }
    if (after->observer_us < before->observer_us) {s->invalid++; return;}
    s->value.observer_us += after->observer_us - before->observer_us;
    s->value.maximum_query_us = after->maximum_query_us;
    s->samples++;
}

static void psp_canvas_cadence_rendered(
    uint64_t render_us, uint64_t canvas_raster_us, uint64_t canvas_overlay_us,
    const PspCanvasCpuSample *before, const PspCanvasCpuSample *after,
    const RenderCanvasConversionMetrics *conversion)
{
    if (psp_canvas_cadence.pending_started_us != 0) {
        psp_canvas_cadence.pending_render_us += render_us;
        psp_canvas_cadence.pending_render_canvas_raster_us += canvas_raster_us;
        psp_canvas_cadence.pending_render_canvas_overlay_us += canvas_overlay_us;
        if (conversion != NULL) {
            /* Last path metadata; samples identifies a multi-slice frame
               where this is not the whole frame's row count. */
            psp_canvas_cadence.pending_conversion = *conversion;
            psp_canvas_cadence.pending_conversion_samples++;
        }
        if (before->valid && after->valid && after->run_us >= before->run_us) {
            psp_canvas_cadence.pending_render_cpu_us += after->run_us - before->run_us;
            psp_canvas_cadence.pending_render_cpu_samples++;
        }
        psp_canvas_cadence.pending_render_slices++;
    }
}

static void psp_canvas_cadence_cpu(
    const PspCanvasCpuSample *before, const PspCanvasCpuSample *after,
    const PspCanvasPeerSample *peer_before, const PspCanvasPeerSample *peer_after)
{
    psp_canvas_cadence.pending_peer_span_us +=
        (uint64_t) peer_before->span_us + peer_after->span_us;
    if (before->valid && after->valid && peer_before->valid_mask != 0u
        && peer_after->valid_mask != 0u) {
        /* Peer counters bracket the owner queries too. Report that extra
           window as uncertainty, never subtract observer time wholesale. */
        psp_canvas_cadence.pending_peer_window_extra_us +=
            before->query_begin_us - peer_before->query_begin_us
            + peer_after->query_end_us - after->query_end_us;
    }
    for (size_t at = 0; at < psp_canvas_peer_count; at++) {
        if ((peer_before->valid_mask & peer_after->valid_mask & (UINT32_C(1) << at)) == 0
            || peer_after->run_us[at] < peer_before->run_us[at]) continue;
        psp_canvas_cadence.pending_peer_cpu_us[at] += peer_after->run_us[at] - peer_before->run_us[at];
        psp_canvas_cadence.pending_peer_samples[at]++;
    }
    if (!before->valid || !after->valid || after->run_us < before->run_us)
        return;
    /* Firmware reads runClocks somewhere inside each status query. These
       matching wall bounds contain its actual interval; the midpoint is
       only an estimate, with explicit bounds in the report. */
    psp_canvas_cadence.pending_runtime_cpu_window_min_us +=
        after->query_begin_us - before->query_end_us;
    psp_canvas_cadence.pending_runtime_cpu_window_max_us +=
        after->query_end_us - before->query_begin_us;
    psp_canvas_cadence.pending_runtime_cpu_observer_us +=
        before->query_end_us - before->query_begin_us
        + after->query_end_us - after->query_begin_us;
    psp_canvas_cadence.pending_runtime_cpu_us += after->run_us - before->run_us;
    psp_canvas_cadence.pending_runtime_cpu_samples++;
    psp_canvas_cadence.pending_runtime_interrupt_preempts +=
        after->interrupt_preempts - before->interrupt_preempts;
    psp_canvas_cadence.pending_runtime_thread_preempts +=
        after->thread_preempts - before->thread_preempts;
    psp_canvas_cadence.pending_runtime_releases += after->releases - before->releases;
}

static void psp_canvas_cadence_presented(
    uint64_t presented_us, uint64_t present_us,
    const PspPresentPhaseTiming *present_timing)
{
    PspCanvasCadence *metrics = &psp_canvas_cadence;
    /* After webgl-measure-end, neither the end mark's own frame (which also
       carries that mark's diagnostics) nor trailing script frames count. */
    bool measured = !metrics->window_closed;
    if (measured) metrics->presentations++;
    if (measured && metrics->previous_present_us != 0
        && presented_us >= metrics->previous_present_us) {
        uint64_t interval = presented_us - metrics->previous_present_us;
        if (interval > metrics->maximum_interval_us)
            metrics->maximum_interval_us = interval;
        if (interval <= UINT64_C(16667)) metrics->intervals_at_60hz++;
        if (interval <= UINT64_C(33333)) metrics->intervals_at_30hz++;
        if (metrics->interval_count < PSP_CANVAS_CADENCE_SAMPLE_LIMIT) {
            metrics->intervals_us[metrics->interval_count++] =
                interval > UINT32_MAX ? UINT32_MAX : (uint32_t) interval;
        } else {
            metrics->interval_samples_dropped++;
        }
    }
    metrics->previous_present_us = presented_us;
    if (measured && metrics->pending_started_us != 0
        && presented_us >= metrics->pending_started_us) {
        ScriptGameAudioNativeMetrics audio_native = {0};
        script_runtime_game_audio_native_take(&audio_native);
        uint64_t pipeline = presented_us - metrics->pending_started_us;
        PspCanvasPhaseSample sample = {
            .ordinal = psp_canvas_cadence_u32(metrics->pipeline_total + 1u),
            .input_step = metrics->pending_input_step,
            .input_buttons = metrics->pending_input_buttons,
            .runtime_turns = metrics->pending_runtime_turns,
            .render_slices = metrics->pending_render_slices,
            .pipeline_us = psp_canvas_cadence_u32(pipeline),
            .runtime_us = psp_canvas_cadence_u32(
                metrics->pending_runtime_us),
            .runtime_cpu_us = psp_canvas_cadence_u32(
                metrics->pending_runtime_cpu_us),
            .runtime_cpu_samples = metrics->pending_runtime_cpu_samples,
            .runtime_cpu_window_min_us = psp_canvas_cadence_u32(
                metrics->pending_runtime_cpu_window_min_us),
            .runtime_cpu_window_max_us = psp_canvas_cadence_u32(
                metrics->pending_runtime_cpu_window_max_us),
            .runtime_cpu_observer_us = psp_canvas_cadence_u32(
                metrics->pending_runtime_cpu_observer_us),
            .peer_span_us = psp_canvas_cadence_u32(metrics->pending_peer_span_us),
            .peer_window_extra_us = psp_canvas_cadence_u32(metrics->pending_peer_window_extra_us),
            .runtime_interrupt_preempts = metrics->pending_runtime_interrupt_preempts,
            .runtime_thread_preempts = metrics->pending_runtime_thread_preempts,
            .runtime_releases = metrics->pending_runtime_releases,
            .script_runtime_us = psp_canvas_cadence_u32(
                metrics->pending_script_runtime_us),
            .navigation_runtime_us = psp_canvas_cadence_u32(
                metrics->pending_navigation_runtime_us),
            .damage_apply_us = psp_canvas_cadence_u32(
                metrics->pending_damage_apply_us),
            .callback_us = psp_canvas_cadence_u32(
                metrics->pending_callback_us),
            .webgl_us = psp_canvas_cadence_u32(
                metrics->pending_webgl_us),
            .webgl = metrics->pending_webgl,
            .runtime_cpu = metrics->pending_cpu,
            .microtask_us = psp_canvas_cadence_u32(
                metrics->pending_microtask_us),
            .render_us = psp_canvas_cadence_u32(
                metrics->pending_render_us),
            .render_canvas_raster_us = psp_canvas_cadence_u32(
                metrics->pending_render_canvas_raster_us),
            .render_canvas_overlay_us = psp_canvas_cadence_u32(
                metrics->pending_render_canvas_overlay_us),
            .render_cpu_us = psp_canvas_cadence_u32(metrics->pending_render_cpu_us),
            .render_cpu_samples = metrics->pending_render_cpu_samples,
            .conversion = metrics->pending_conversion,
            .conversion_samples = metrics->pending_conversion_samples,
            .audio_native = audio_native,
            .present_us = psp_canvas_cadence_u32(present_us),
            .present_base_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->base_us),
            .present_base_cpu_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->base_cpu_us),
            .present_base_bytes = present_timing == NULL ? 0u
                : present_timing->base_bytes,
            .present_base_cpu_valid = present_timing != NULL
                && present_timing->base_cpu_valid,
            .present_composite_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->composite_us),
            .present_publish_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->publish_us),
            .present_flush_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(
                    present_timing->publish_flush_us),
            .present_set_framebuffer_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(
                    present_timing->publish_set_framebuffer_us),
            .present_vblank_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(
                    present_timing->publish_vblank_us),
            .ui_focus_scroll_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(
                    present_timing->ui_focus_scroll_us),
            .ui_chrome_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->ui_chrome_us),
            .ui_cursor_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->ui_cursor_us),
            .ui_screen_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->ui_screen_us),
            .ui_toast_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->ui_toast_us),
            .ui_loading_us = present_timing == NULL ? 0u
                : psp_canvas_cadence_u32(present_timing->ui_loading_us),
            .js_allocations = psp_canvas_cadence_u32(
                metrics->pending_js_allocations),
            .js_frees = psp_canvas_cadence_u32(
                metrics->pending_js_frees),
            .js_reallocations = psp_canvas_cadence_u32(
                metrics->pending_js_reallocations),
            .js_allocated_bytes = psp_canvas_cadence_u32(
                metrics->pending_js_allocated_bytes),
            .js_freed_bytes = psp_canvas_cadence_u32(
                metrics->pending_js_freed_bytes),
            .js_live_before = psp_canvas_cadence_u32(
                metrics->pending_js_live_before),
            .js_live_after = psp_canvas_cadence_u32(
                metrics->pending_js_live_after),
            .started_us = metrics->pending_started_us,
            .ready_us = psp_validation_display_ready_us,
            .latched_us = psp_validation_display_latched_us,
            .previous_latched_us = metrics->previous_latched_us,
            .ready_vcount = psp_validation_display_ready_vcount,
            .latched_vcount = psp_validation_display_latched_vcount,
            .previous_vcount = metrics->previous_vcount,
            .causal_window = metrics->causal_window
        };
        psp_canvas_budget_copy(&sample, present_timing);
        for (size_t at = 0; at < psp_canvas_peer_count; at++) {
            sample.peer_cpu_us[at] = psp_canvas_cadence_u32(metrics->pending_peer_cpu_us[at]);
            sample.peer_samples[at] = metrics->pending_peer_samples[at];
        }
        metrics->pipeline_total++;
        if (pipeline > UINT64_C(34000)) {
            metrics->pipelines_over_34ms++;
            size_t insertion = metrics->slow_phase_count;
            if (insertion == PSP_CANVAS_CADENCE_SLOW_LIMIT) {
                if (sample.pipeline_us
                        <= metrics->slow_phases[insertion - 1u].pipeline_us) {
                    insertion = SIZE_MAX;
                } else {
                    insertion--;
                }
            } else {
                metrics->slow_phase_count++;
            }
            if (insertion != SIZE_MAX) {
                while (insertion != 0
                       && metrics->slow_phases[insertion - 1u].pipeline_us
                              < sample.pipeline_us) {
                    metrics->slow_phases[insertion] =
                        metrics->slow_phases[insertion - 1u];
                    insertion--;
                }
                metrics->slow_phases[insertion] = sample;
            }
        }
        if (!metrics->maximum_phase_valid
            || pipeline > metrics->maximum_pipeline_us) {
            metrics->maximum_pipeline_us = pipeline;
            metrics->maximum_phase = sample;
            metrics->maximum_phase_valid = true;
        }
        if (metrics->pipeline_count < PSP_CANVAS_CADENCE_SAMPLE_LIMIT) {
            size_t at = metrics->pipeline_count++;
            if (sample.causal_window && at < PSP_CANVAS_GAME_SAMPLE_LIMIT
                && psp_webgl_measurement_navigation != NULL) {
                uint64_t read_started = (uint64_t) sceKernelGetSystemTimeWide();
                sample.game_packet_valid = script_runtime_copy_validation_frame_words(
                    psp_webgl_measurement_navigation->page.runtime,
                    metrics->game_words[at], PSP_CANVAS_GAME_WORDS,
                    &sample.game_origin_us);
                sample.packet_read_us = psp_canvas_cadence_u32(
                    (uint64_t) sceKernelGetSystemTimeWide() - read_started);
            }
            metrics->pipeline_us[at] = psp_canvas_cadence_u32(pipeline);
            metrics->phases[at] = sample;
        } else {
            metrics->pipeline_samples_dropped++;
        }
    }
    metrics->previous_latched_us = psp_validation_display_latched_us;
    metrics->previous_vcount = psp_validation_display_latched_vcount;
    metrics->pending_started_us = 0;
    metrics->pending_runtime_us = 0;
    metrics->pending_runtime_cpu_us = 0;
    metrics->pending_runtime_cpu_window_min_us = 0;
    metrics->pending_runtime_cpu_window_max_us = 0;
    metrics->pending_runtime_cpu_observer_us = 0;
    metrics->pending_runtime_cpu_samples = 0;
    memset(metrics->pending_peer_cpu_us, 0, sizeof(metrics->pending_peer_cpu_us));
    memset(metrics->pending_peer_samples, 0, sizeof(metrics->pending_peer_samples));
    metrics->pending_peer_span_us = 0;
    metrics->pending_peer_window_extra_us = 0;
    metrics->pending_runtime_interrupt_preempts = 0;
    metrics->pending_runtime_thread_preempts = 0;
    metrics->pending_runtime_releases = 0;
    metrics->pending_script_runtime_us = 0;
    metrics->pending_navigation_runtime_us = 0;
    metrics->pending_damage_apply_us = 0;
    metrics->pending_runtime_turns = 0;
    metrics->pending_render_slices = 0;
    metrics->pending_callback_us = 0;
    metrics->pending_webgl_us = 0;
    memset(&metrics->pending_webgl, 0, sizeof(metrics->pending_webgl));
    memset(&metrics->pending_cpu, 0, sizeof(metrics->pending_cpu));
    metrics->pending_microtask_us = 0;
    metrics->pending_render_us = 0;
    metrics->pending_render_canvas_raster_us = 0;
    metrics->pending_render_canvas_overlay_us = 0;
    metrics->pending_render_cpu_us = 0;
    metrics->pending_render_cpu_samples = 0;
    metrics->pending_js_allocations = 0;
    metrics->pending_js_frees = 0;
    metrics->pending_js_reallocations = 0;
    metrics->pending_js_allocated_bytes = 0;
    metrics->pending_js_freed_bytes = 0;
    metrics->pending_js_live_before = 0;
    metrics->pending_js_live_after = 0;
    metrics->pending_input_step = 0;
    metrics->pending_input_buttons = 0;
    psp_canvas_budget_after_present();
}

static TILEFINCH_OUT_OF_LINE void psp_canvas_cadence_observe_present(
    unsigned render_visual_state, unsigned presents_before,
    uint64_t present_started_us, uint64_t present_finished_us)
{
    if (render_visual_state != 2u
        || psp_display.presents == presents_before) return;
    PspPresentPhaseTiming present_timing;
    const PspPresentPhaseTiming *timing =
        psp_present_validation_last_timing(&present_timing)
            ? &present_timing : NULL;
    psp_canvas_cadence_presented(present_finished_us,
        present_finished_us >= present_started_us
            ? present_finished_us - present_started_us : 0,
        timing);
}

static uint32_t psp_canvas_cadence_percentile(
    const uint32_t *samples, size_t count, unsigned percentile)
{
    if (samples == NULL || count == 0 || percentile > 100u) return 0;
    memcpy(psp_canvas_cadence.sorted_us, samples,
           count * sizeof(psp_canvas_cadence.sorted_us[0]));
    for (size_t at = 1; at < count; at++) {
        uint32_t value = psp_canvas_cadence.sorted_us[at];
        size_t before = at;
        while (before != 0
               && psp_canvas_cadence.sorted_us[before - 1u] > value) {
            psp_canvas_cadence.sorted_us[before] =
                psp_canvas_cadence.sorted_us[before - 1u];
            before--;
        }
        psp_canvas_cadence.sorted_us[before] = value;
    }
    size_t rank = (count * percentile + 99u) / 100u;
    if (rank != 0) rank--;
    if (rank >= count) rank = count - 1u;
    return psp_canvas_cadence.sorted_us[rank];
}

void psp_report_canvas_cadence(void)
{
    const PspCanvasCadence *metrics = &psp_canvas_cadence;
    size_t gaps = metrics->causal_loops_without_sample;
    if (gaps > PSP_CANVAS_LOOP_GAP_LIMIT) gaps = PSP_CANVAS_LOOP_GAP_LIMIT;
    for (size_t at = 0; at < gaps; at++) {
        const PspCanvasLoopGap *g = &metrics->causal_loop_gaps[at];
        printf("tilefinch-canvas-loop-gap: begin-us=%llu end-us=%llu "
            "wait=%lu runtime=%lu callbacks=%lu timer-known=%d "
            "timer-frame=%d due-us=%lld fast-followup=%d\n",
            (unsigned long long)g->start_us, (unsigned long long)g->end_us,
            (unsigned long)g->wait_us, (unsigned long)g->runtime_us,
            (unsigned long)g->callbacks, g->timer_known, g->timer_frame,
            (long long)g->due_us, g->fast_followup);
    }
    /* Dump after measurement, never printf from the critical frame path.
       runtime includes callback/microtasks; WebGL nests in microtasks. */
    for (size_t at = 0; at < metrics->pipeline_count; at++) {
        const PspCanvasPhaseSample *s = &metrics->phases[at];
        if (!s->causal_window) continue;
        if (s->loop_start_us != 0) {
            const uint32_t *p = s->loop_phase_us;
            printf("tilefinch-loop-timing: begin-us=%llu end-us=%llu "
                "wait=%lu input=%lu action=%lu probe=%lu navigation=%lu "
                "runtime=%lu idle=%lu raster=%lu present=%lu images=%lu tail=%lu\n",
                (unsigned long long)s->loop_start_us,
                (unsigned long long)s->loop_end_us,
                (unsigned long)p[0],(unsigned long)p[1],(unsigned long)p[2],
                (unsigned long)p[3],(unsigned long)p[4],(unsigned long)p[5],
                (unsigned long)p[6],(unsigned long)p[7],(unsigned long)p[8],
                (unsigned long)p[9],(unsigned long)p[10]);
        }
        printf("tilefinch-canvas-frame: n=%lu step=%u buttons=%u "
            "start=%lluus ready=%lluus latch=%lluus previous=%lluus "
            "vready=%lu vlatch=%lu vprevious=%lu turns=%lu slices=%lu "
            "runtime=%lu callback=%lu microtask=%lu webgl=%lu render=%lu "
            "base=%lu composite=%lu flush=%lu set=%lu wait=%lu "
            "packet-read=%lu alloc=%lu bytes=%lu\n",
            (unsigned long)s->ordinal, s->input_step, s->input_buttons,
            (unsigned long long)s->started_us, (unsigned long long)s->ready_us,
            (unsigned long long)s->latched_us,
            (unsigned long long)s->previous_latched_us,
            (unsigned long)s->ready_vcount, (unsigned long)s->latched_vcount,
            (unsigned long)s->previous_vcount, (unsigned long)s->runtime_turns,
            (unsigned long)s->render_slices, (unsigned long)s->runtime_us,
            (unsigned long)s->callback_us, (unsigned long)s->microtask_us,
            (unsigned long)s->webgl_us, (unsigned long)s->render_us,
            (unsigned long)s->present_base_us, (unsigned long)s->present_composite_us,
            (unsigned long)s->present_flush_us,
            (unsigned long)s->present_set_framebuffer_us,
            (unsigned long)s->present_vblank_us, (unsigned long)s->packet_read_us,
            (unsigned long)s->js_allocations, (unsigned long)s->js_allocated_bytes);
        printf("tilefinch-canvas-cpu: n=%lu samples=%lu runtime=%lu "
            "cpu=%lu wall-min=%lu wall-max=%lu observer=%lu "
            "interrupt-preempts=%lu thread-preempts=%lu releases=%lu\n",
            (unsigned long)s->ordinal, (unsigned long)s->runtime_cpu_samples,
            (unsigned long)s->runtime_us, (unsigned long)s->runtime_cpu_us,
            (unsigned long)s->runtime_cpu_window_min_us,
            (unsigned long)s->runtime_cpu_window_max_us,
            (unsigned long)s->runtime_cpu_observer_us,
            (unsigned long)s->runtime_interrupt_preempts,
            (unsigned long)s->runtime_thread_preempts,
            (unsigned long)s->runtime_releases);
        printf("tilefinch-canvas-webgl: n=%lu samples=%lu invalid=%lu",
            (unsigned long)s->ordinal, (unsigned long)s->webgl.samples,
            (unsigned long)s->webgl.invalid);
        for (size_t at = 0; at < PSP_CANVAS_WEBGL_FIELD_COUNT; at++)
            printf(" %s=%lu", psp_canvas_webgl_names[at],
                (unsigned long)s->webgl.value[at]);
        printf("\n");
        static const char *const cpu_names[] = {
            "advance", "preparation", "callback", "jobs", "webgl", "refresh"
        };
        const PspCanvasRuntimeCpuSample *rc = &s->runtime_cpu;
        printf("tilefinch-canvas-runtime-cpu: n=%lu samples=%lu invalid=%lu observer=%llu query-max=%llu",
            (unsigned long)s->ordinal, (unsigned long)rc->samples, (unsigned long)rc->invalid,
            (unsigned long long)rc->value.observer_us, (unsigned long long)rc->value.maximum_query_us);
        for (unsigned at = 0; at < SCRIPT_RUNTIME_CPU_PHASES; at++)
            printf(" %s=%llu/%llu/%llu", cpu_names[at],
                (unsigned long long)rc->value.inclusive_cpu_us[at],
                (unsigned long long)rc->value.exclusive_cpu_us[at],
                (unsigned long long)rc->value.calls[at]);
        printf("\n");
        /* Stopping live sampling must not hide already buffered observations.
           The sampler's enable bit/count may be cleared before this dump. */
        for (size_t at = 0; at < PSP_CANVAS_PEER_LIMIT; at++)
            if (s->peer_samples[at] != 0u)
                printf("tilefinch-canvas-peer-cpu: n=%lu slot=%lu samples=%lu cpu=%lu span=%lu extra=%lu\n",
                       (unsigned long) s->ordinal, (unsigned long) at,
                       (unsigned long) s->peer_samples[at], (unsigned long) s->peer_cpu_us[at],
                       (unsigned long) s->peer_span_us, (unsigned long) s->peer_window_extra_us);
        printf("tilefinch-canvas-raster: n=%lu samples=%lu cpu=%lu "
            "convert=%lu overlay=%lu\n",
            (unsigned long)s->ordinal, (unsigned long)s->render_cpu_samples,
            (unsigned long)s->render_cpu_us,
            (unsigned long)s->render_canvas_raster_us,
            (unsigned long)s->render_canvas_overlay_us);
        printf("tilefinch-canvas-copy: n=%lu valid=%u cpu=%lu bytes=%lu\n",
            (unsigned long)s->ordinal, s->present_base_cpu_valid ? 1u : 0u,
            (unsigned long)s->present_base_cpu_us,
            (unsigned long)s->present_base_bytes);
        if (s->budget_valid) {
            printf("tilefinch-canvas-budget: n=%lu begin=%llu end=%llu "
                "cpu=%lu wall-min=%lu wall-max=%lu observer=%lu peer-span=%lu peer-extra=%lu mask=%lu\n",
                (unsigned long)s->ordinal,
                (unsigned long long)s->budget_begin_us, (unsigned long long)s->budget_end_us,
                (unsigned long)s->budget_owner_cpu_us,
                (unsigned long)s->budget_wall_min_us, (unsigned long)s->budget_wall_max_us,
                (unsigned long)s->budget_observer_us, (unsigned long)s->budget_peer_span_us,
                (unsigned long)s->budget_peer_extra_us, (unsigned long)s->budget_peer_mask);
            for (size_t at = 0; at < PSP_CANVAS_PEER_LIMIT; at++)
                if ((s->budget_peer_mask & (UINT32_C(1) << at)) != 0u)
                    printf("tilefinch-canvas-budget-peer: n=%lu slot=%lu cpu=%lu\n",
                        (unsigned long)s->ordinal, (unsigned long)at,
                        (unsigned long)s->budget_peer_cpu_us[at]);
        }
        const RenderCanvasConversionMetrics *c = &s->conversion;
        printf("tilefinch-canvas-conversion: n=%lu samples=%lu kernel=%lu "
            "general=%lu repeat=%lu skipped=%lu vfpu=%lu pixels=%lu native=%u setup=%u "
            "address=%lu stride=%lu width=%d height=%d out-width=%d out-height=%d\n",
            (unsigned long)s->ordinal, (unsigned long)s->conversion_samples,
            (unsigned long)c->kernel_rows, (unsigned long)c->general_rows,
            (unsigned long)c->repeat_rows, (unsigned long)c->skipped_rows,
            (unsigned long)c->vfpu_rows,
            (unsigned long)c->source_pixels, c->native_source ? 1u : 0u,
            c->setup_once ? 1u : 0u,
            (unsigned long)c->source_address, (unsigned long)c->source_stride,
            c->source_width, c->source_height, c->output_width, c->output_height);
        for (unsigned command = 0; command < SCRIPT_GAME_AUDIO_NATIVE_COMMANDS;
             command++) {
            if (s->audio_native.command_calls[command] == 0) continue;
            printf("tilefinch-canvas-audio: n=%lu command=%u calls=%lu us=%llu nested=%lu\n",
                (unsigned long)s->ordinal, command,
                (unsigned long)s->audio_native.command_calls[command],
                (unsigned long long)s->audio_native.command_us[command],
                (unsigned long)s->audio_native.nested_calls);
        }
        if (!s->game_packet_valid || at >= PSP_CANVAS_GAME_SAMPLE_LIMIT) continue;
        char line[1536];
        int used = snprintf(line, sizeof(line),
            "tilefinch-canvas-game: n=%lu origin=%lluus words=",
            (unsigned long)s->ordinal, (unsigned long long)s->game_origin_us);
        if (used < 0 || (size_t)used >= sizeof(line)) continue;
        bool complete = true;
        for (size_t word = 0; word < PSP_CANVAS_GAME_WORDS; word++) {
            int added = snprintf(line + used, sizeof(line) - (size_t)used,
                "%s%lu", word == 0 ? "" : ",",
                (unsigned long)metrics->game_words[at][word]);
            if (added < 0 || (size_t)added >= sizeof(line) - (size_t)used) {
                complete = false;
                break;
            }
            used += added;
        }
        if (complete) printf("%s\n", line);
    }
    uint32_t interval_p99 = psp_canvas_cadence_percentile(
        metrics->intervals_us, metrics->interval_count, 99u);
    uint32_t pipeline_p95 = psp_canvas_cadence_percentile(
        metrics->pipeline_us, metrics->pipeline_count, 95u);
    uint32_t pipeline_p99 = psp_canvas_cadence_percentile(
        metrics->pipeline_us, metrics->pipeline_count, 99u);
    printf(
        "tilefinch-canvas-cadence: presents=%zu intervals=%zu "
        "interval-p50=%luus interval-p95=%luus interval-p99=%luus "
        "interval-max=%lluus "
        "within-16.67ms=%zu within-33.33ms=%zu pipeline-samples=%zu "
        "pipeline-p50=%luus pipeline-p95=%luus pipeline-p99=%luus "
        "pipeline-max=%lluus interval-dropped=%zu pipeline-dropped=%zu "
        "causal-loops-without-sample=%zu\n",
        metrics->presentations, metrics->interval_count,
        (unsigned long) psp_canvas_cadence_percentile(
            metrics->intervals_us, metrics->interval_count, 50u),
        (unsigned long) psp_canvas_cadence_percentile(
            metrics->intervals_us, metrics->interval_count, 95u),
        (unsigned long) interval_p99,
        (unsigned long long) metrics->maximum_interval_us,
        metrics->intervals_at_60hz, metrics->intervals_at_30hz,
        metrics->pipeline_count,
        (unsigned long) psp_canvas_cadence_percentile(
            metrics->pipeline_us, metrics->pipeline_count, 50u),
        (unsigned long) pipeline_p95,
        (unsigned long) pipeline_p99,
        (unsigned long long) metrics->maximum_pipeline_us,
        metrics->interval_samples_dropped,
        metrics->pipeline_samples_dropped,
        metrics->causal_loops_without_sample);
    if (metrics->pipeline_count == 0) return;
    size_t tail_count = 0;
    uint64_t tail_runtime = 0, tail_callback = 0, tail_webgl = 0;
    uint64_t tail_microtask = 0, tail_render = 0, tail_present = 0;
    uint64_t tail_present_base = 0, tail_present_composite = 0;
    uint64_t tail_present_publish = 0;
    uint64_t tail_present_flush = 0, tail_present_set = 0;
    uint64_t tail_present_vblank = 0;
    uint64_t tail_ui_focus = 0, tail_ui_chrome = 0, tail_ui_cursor = 0;
    uint64_t tail_ui_screen = 0, tail_ui_toast = 0, tail_ui_loading = 0;
    uint64_t tail_other = 0;
    size_t p99_index = SIZE_MAX, maximum_index = 0;
    for (size_t at = 0; at < metrics->pipeline_count; at++) {
        const PspCanvasPhaseSample *sample = &metrics->phases[at];
        if (sample->pipeline_us >= pipeline_p95) {
            uint64_t accounted = (uint64_t) sample->runtime_us
                + sample->render_us + sample->present_us;
            tail_count++;
            tail_runtime += sample->runtime_us;
            tail_callback += sample->callback_us;
            tail_webgl += sample->webgl_us;
            tail_microtask += sample->microtask_us;
            tail_render += sample->render_us;
            tail_present += sample->present_us;
            tail_present_base += sample->present_base_us;
            tail_present_composite += sample->present_composite_us;
            tail_present_publish += sample->present_publish_us;
            tail_present_flush += sample->present_flush_us;
            tail_present_set += sample->present_set_framebuffer_us;
            tail_present_vblank += sample->present_vblank_us;
            tail_ui_focus += sample->ui_focus_scroll_us;
            tail_ui_chrome += sample->ui_chrome_us;
            tail_ui_cursor += sample->ui_cursor_us;
            tail_ui_screen += sample->ui_screen_us;
            tail_ui_toast += sample->ui_toast_us;
            tail_ui_loading += sample->ui_loading_us;
            if (sample->pipeline_us > accounted)
                tail_other += sample->pipeline_us - accounted;
        }
        if (p99_index == SIZE_MAX && sample->pipeline_us == pipeline_p99)
            p99_index = at;
        if (sample->pipeline_us
                > metrics->phases[maximum_index].pipeline_us)
            maximum_index = at;
    }
    if (p99_index == SIZE_MAX) p99_index = maximum_index;
    printf(
        "tilefinch-canvas-tail: over-34ms=%zu/%zu p95-count=%zu "
        "p95-avg-runtime=%lluus callback=%lluus webgl=%lluus "
        "microtask=%lluus render=%lluus present=%lluus other=%lluus\n",
        metrics->pipelines_over_34ms, metrics->pipeline_total, tail_count,
        (unsigned long long) (tail_count == 0 ? 0
            : tail_runtime / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_callback / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_webgl / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_microtask / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_render / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_other / tail_count));
    printf(
        "tilefinch-canvas-present-tail: p95-count=%zu "
        "base=%lluus composite=%lluus publish=%lluus "
        "flush=%lluus set=%lluus vblank=%lluus\n",
        tail_count,
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_base / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_composite / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_publish / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_flush / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_set / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_present_vblank / tail_count));
    printf(
        "tilefinch-canvas-ui-tail: p95-count=%zu focus-scroll=%lluus "
        "chrome=%lluus cursor=%lluus screen=%lluus toast=%lluus "
        "loading=%lluus\n",
        tail_count,
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_focus / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_chrome / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_cursor / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_screen / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_toast / tail_count),
        (unsigned long long) (tail_count == 0 ? 0
            : tail_ui_loading / tail_count));
    const PspCanvasPhaseSample *p99 = &metrics->phases[p99_index];
    const PspCanvasPhaseSample *maximum = metrics->maximum_phase_valid
        ? &metrics->maximum_phase : &metrics->phases[maximum_index];
    uint64_t p99_accounted = (uint64_t) p99->runtime_us
        + p99->render_us + p99->present_us;
    uint64_t maximum_accounted = (uint64_t) maximum->runtime_us
        + maximum->render_us + maximum->present_us;
    printf(
        "tilefinch-canvas-p99-frame: total=%luus runtime=%luus "
        "callback=%luus webgl=%luus microtask=%luus render=%luus "
        "present=%luus base=%luus composite=%luus publish=%luus "
        "flush=%luus set=%luus vblank=%luus "
        "ui=%lu/%lu/%lu/%lu/%lu/%luus "
        "other=%lluus\n",
        (unsigned long) p99->pipeline_us,
        (unsigned long) p99->runtime_us,
        (unsigned long) p99->callback_us,
        (unsigned long) p99->webgl_us,
        (unsigned long) p99->microtask_us,
        (unsigned long) p99->render_us,
        (unsigned long) p99->present_us,
        (unsigned long) p99->present_base_us,
        (unsigned long) p99->present_composite_us,
        (unsigned long) p99->present_publish_us,
        (unsigned long) p99->present_flush_us,
        (unsigned long) p99->present_set_framebuffer_us,
        (unsigned long) p99->present_vblank_us,
        (unsigned long) p99->ui_focus_scroll_us,
        (unsigned long) p99->ui_chrome_us,
        (unsigned long) p99->ui_cursor_us,
        (unsigned long) p99->ui_screen_us,
        (unsigned long) p99->ui_toast_us,
        (unsigned long) p99->ui_loading_us,
        (unsigned long long) (p99->pipeline_us > p99_accounted
            ? p99->pipeline_us - p99_accounted : 0));
    printf(
        "tilefinch-canvas-worst-frame: total=%luus runtime=%luus "
        "callback=%luus webgl=%luus microtask=%luus render=%luus "
        "present=%luus base=%luus composite=%luus publish=%luus "
        "flush=%luus set=%luus vblank=%luus "
        "ui=%lu/%lu/%lu/%lu/%lu/%luus "
        "other=%lluus\n",
        (unsigned long) maximum->pipeline_us,
        (unsigned long) maximum->runtime_us,
        (unsigned long) maximum->callback_us,
        (unsigned long) maximum->webgl_us,
        (unsigned long) maximum->microtask_us,
        (unsigned long) maximum->render_us,
        (unsigned long) maximum->present_us,
        (unsigned long) maximum->present_base_us,
        (unsigned long) maximum->present_composite_us,
        (unsigned long) maximum->present_publish_us,
        (unsigned long) maximum->present_flush_us,
        (unsigned long) maximum->present_set_framebuffer_us,
        (unsigned long) maximum->present_vblank_us,
        (unsigned long) maximum->ui_focus_scroll_us,
        (unsigned long) maximum->ui_chrome_us,
        (unsigned long) maximum->ui_cursor_us,
        (unsigned long) maximum->ui_screen_us,
        (unsigned long) maximum->ui_toast_us,
        (unsigned long) maximum->ui_loading_us,
        (unsigned long long) (maximum->pipeline_us > maximum_accounted
            ? maximum->pipeline_us - maximum_accounted : 0));
    for (size_t rank = 0; rank < metrics->slow_phase_count; rank++) {
        const PspCanvasPhaseSample *slow = &metrics->slow_phases[rank];
        uint64_t accounted = (uint64_t) slow->runtime_us
            + slow->render_us + slow->present_us;
        printf(
            "tilefinch-canvas-over34: rank=%zu frame=%lu step=%u "
            "buttons=0x%04x total=%luus "
            "turns=%lu slices=%lu "
            "runtime=%luus script=%luus navigation=%luus damage=%luus "
            "callback=%luus webgl=%luus microtask=%luus "
            "render=%luus present=%luus base=%luus composite=%luus "
            "vblank=%luus other=%lluus alloc=%lu/%luB free=%lu/%luB "
            "live=%lu->%luB\n",
            rank + 1u, (unsigned long) slow->ordinal,
            (unsigned) slow->input_step, (unsigned) slow->input_buttons,
            (unsigned long) slow->pipeline_us,
            (unsigned long) slow->runtime_turns,
            (unsigned long) slow->render_slices,
            (unsigned long) slow->runtime_us,
            (unsigned long) slow->script_runtime_us,
            (unsigned long) slow->navigation_runtime_us,
            (unsigned long) slow->damage_apply_us,
            (unsigned long) slow->callback_us,
            (unsigned long) slow->webgl_us,
            (unsigned long) slow->microtask_us,
            (unsigned long) slow->render_us,
            (unsigned long) slow->present_us,
            (unsigned long) slow->present_base_us,
            (unsigned long) slow->present_composite_us,
            (unsigned long) slow->present_vblank_us,
            (unsigned long long) (slow->pipeline_us > accounted
                ? slow->pipeline_us - accounted : 0),
            (unsigned long) slow->js_allocations,
            (unsigned long) slow->js_allocated_bytes,
            (unsigned long) slow->js_frees,
            (unsigned long) slow->js_freed_bytes,
            (unsigned long) slow->js_live_before,
            (unsigned long) slow->js_live_after);
    }
    printf(
        "tilefinch-canvas-js-p99: alloc=%lu/%luB free=%lu/%luB "
        "realloc=%lu live=%lu->%luB\n",
        (unsigned long) p99->js_allocations,
        (unsigned long) p99->js_allocated_bytes,
        (unsigned long) p99->js_frees,
        (unsigned long) p99->js_freed_bytes,
        (unsigned long) p99->js_reallocations,
        (unsigned long) p99->js_live_before,
        (unsigned long) p99->js_live_after);
    printf(
        "tilefinch-canvas-js-worst: alloc=%lu/%luB free=%lu/%luB "
        "realloc=%lu live=%lu->%luB\n",
        (unsigned long) maximum->js_allocations,
        (unsigned long) maximum->js_allocated_bytes,
        (unsigned long) maximum->js_frees,
        (unsigned long) maximum->js_freed_bytes,
        (unsigned long) maximum->js_reallocations,
        (unsigned long) maximum->js_live_before,
        (unsigned long) maximum->js_live_after);
}

static TILEFINCH_OUT_OF_LINE void psp_game_audio_report_blocks(
    const TilefinchGameAudioMixMetrics *metrics)
{
    printf("tilefinch-audio-block-records: count=%lu overflow=%lu\n",
        (unsigned long)metrics->block_records,
        (unsigned long)metrics->record_overflow);
    for (size_t at = 0; at < metrics->block_records; at++) {
        TilefinchGameAudioBlockRecord record;
        if (!tilefinch_game_audio_validation_mix_block(at, &record)) {
            printf("tilefinch-audio-block-records: read-failed=%lu\n", (unsigned long)at);
            return;
        }
        printf("tilefinch-audio-block-record: n=%lu output-frame=%lu frames=%lu "
            "begin-min=%llu begin-max=%llu mix-min=%llu mix-max=%llu "
            "end-min=%llu end-max=%llu cpu-begin=%llu cpu-mix=%llu cpu-end=%llu "
            "silent=%lu audible=%lu constant=%lu automated=%lu fast-forwarded=%lu "
            "result=%ld valid=%u counted=%u\n",
            (unsigned long)at, (unsigned long)record.output_frame,
            (unsigned long)record.frames,
            (unsigned long long)record.begin_before_us,
            (unsigned long long)record.begin_after_us,
            (unsigned long long)record.mixed_before_us,
            (unsigned long long)record.mixed_after_us,
            (unsigned long long)record.submitted_before_us,
            (unsigned long long)record.submitted_after_us,
            (unsigned long long)record.begin_cpu_us,
            (unsigned long long)record.mixed_cpu_us,
            (unsigned long long)record.submitted_cpu_us,
            (unsigned long)record.silent, (unsigned long)record.audible,
            (unsigned long)record.constant, (unsigned long)record.automated,
            (unsigned long)record.fast_forwarded, (long)record.output_result,
            record.clocks_valid ? 1u : 0u, record.work_counted ? 1u : 0u);
    }
}

/* Set by webgl-measure-start; the counters reset on the next frame. */
static bool psp_webgl_measurement_reset_pending;

static void psp_webgl_measurement_reset(const NavigationSession *navigation)
{
    memset(&psp_canvas_cadence, 0, sizeof(psp_canvas_cadence));
    memset(&psp_canvas_budget_begin, 0, sizeof(psp_canvas_budget_begin));
    memset(&psp_canvas_budget_end, 0, sizeof(psp_canvas_budget_end));
    psp_canvas_cadence.causal_window = true;
    psp_present_validation_pre_publish_hook(psp_canvas_peer_enabled
        ? psp_canvas_budget_pre_publish : NULL);
    psp_present_validation_measure_cpu(true);
    script_runtime_game_audio_native_measure(psp_validation_native_detail);
    psp_runtime_advance_total_us = 0;
    psp_runtime_advance_maximum_us = 0;
    psp_runtime_advance_calls = 0;
    psp_runtime_layout_changes = 0;
    psp_runtime_render_deferrals = 0;
    psp_runtime_handoff_deferrals = 0;
    psp_runtime_reclaim_deferrals = 0;
    script_runtime_webgl_native_metrics_reset();
    script_runtime_timing_metrics_reset(
        navigation == NULL ? NULL : navigation->page.runtime);
    printf("tilefinch-webgl-measurement: event=reset\n");
    if (!psp_validation_canvas_live_trace) {
        bool retained = psp_log_begin_timing_window();
        script_runtime_suppress_checkpoint_trace(
            navigation == NULL ? NULL : navigation->page.runtime, retained);
        printf("tilefinch-webgl-measurement: buffered-log=%d\n", retained);
    }
}

bool psp_validation_video_timing_window;
unsigned psp_validation_video_suppressed_loops;
static unsigned psp_validation_video_suppressed_publications;
static atomic_int psp_validation_video_window_requested;
static atomic_int psp_validation_video_copy_requested;

/* The main loop's input step, before its script turn. Only here, never from
   the busy-frame supervisor, so the reset cannot split a frame. */
void psp_webgl_measurement_main_frame(void)
{
    /* Live marks can be consumed by the busy-frame supervisor. It may queue
       a request, but must not reset owner-thread presentation counters or
       change a GE copy while that owner's draw is in progress. */
    int copy = atomic_exchange(&psp_validation_video_copy_requested, 0);
    if (copy != 0) psp_media_present_ge_stage_rows_enabled(copy == 2);
    int window = atomic_exchange(&psp_validation_video_window_requested, 0);
    if (window == 1) {
        psp_validation_video_timing_window = psp_log_begin_timing_window();
        psp_validation_video_suppressed_loops = 0;
        psp_validation_video_suppressed_publications = 0;
        printf("tilefinch-video-timing: buffered=%d\n",
               psp_validation_video_timing_window ? 1 : 0);
        psp_video_scanout_measurement(true);
    } else if (window == 2) {
        psp_validation_video_timing_window = false;
        /* Snapshots end at the last presentation. Flush before reporting so
           a full diagnostic buffer cannot silently drop the summary. */
        psp_log_end_timing_window();
        printf("tilefinch-video-timing: suppressed-loops=%u "
               "suppressed-publications=%u\n",
               psp_validation_video_suppressed_loops,
               psp_validation_video_suppressed_publications);
        psp_video_scanout_measurement(false);
        psp_report_presentation_cadence("video-window");
    }
    if (!psp_webgl_measurement_reset_pending) return;
    psp_webgl_measurement_reset_pending = false;
    psp_webgl_measurement_reset(psp_webgl_measurement_navigation);
}

void psp_webgl_measurement_mark(const char *mark)
{
    if (mark != NULL && strcmp(mark, "queue-perf") == 0) {
        bool passed = psp_webgl_measurement_navigation != NULL
            && psp_queue_menu_profile(psp_webgl_measurement_navigation->budget);
        printf("tilefinch-menu-profile: outcome=%s\n", passed ? "pass" : "fail");
    }
    if (mark != NULL && strcmp(mark, "queue-shortcut") == 0) {
        bool passed = psp_queue_shortcut_qualify();
        printf("tilefinch-queue-shortcut: outcome=%s checked-states=%u\n",
            passed ? "pass" : "fail", passed ? 4u : 0u);
    }
    if (mark != NULL && strcmp(mark, "copy-vfpu-on") == 0)
        psp_present_validation_vfpu_copy(true, false);
    else if (mark != NULL && strcmp(mark, "copy-vfpu-off") == 0)
        psp_present_validation_vfpu_copy(false, false);
    else if (mark != NULL && strcmp(mark, "copy-vfpu-check") == 0)
        psp_present_validation_vfpu_copy(true, true);
    else if (mark != NULL && strcmp(mark, "copy-vfpu-stat") == 0) {
        uint32_t copies, checks, errors;
        psp_present_validation_vfpu_copy_counts(&copies, &checks, &errors);
        printf("tilefinch-copy-vfpu: copies=%lu checks=%lu errors=%lu\n",
            (unsigned long) copies, (unsigned long) checks,
            (unsigned long) errors);
    }
    if (mark != NULL && (strcmp(mark, "audio-slots-pre") == 0
                        || strcmp(mark, "audio-slots-post") == 0
                        || strcmp(mark, "audio-slots-end") == 0)) {
        unsigned phase = strcmp(mark, "audio-slots-pre") == 0 ? 1u
            : strcmp(mark, "audio-slots-post") == 0 ? 2u : 3u;
        unsigned empty = 0u;
        bool queued = atomic_compare_exchange_strong_explicit(
            &psp_audio_slots_receipt_phase, &empty, phase,
            memory_order_release, memory_order_relaxed);
        printf("tilefinch-audio-slots-request: mark=%s deferred=1 queued=%u\n",
               mark, queued ? 1u : 0u);
        return;
    }
    if (mark != NULL && strcmp(mark, "webgl-tint-off") == 0)
        script_runtime_webgl_validation_tint_share(false);
    else if (mark != NULL && strcmp(mark, "webgl-tint-on") == 0)
        script_runtime_webgl_validation_tint_share(true);
    else if (mark != NULL && strcmp(mark, "webgl-direct-off") == 0)
        script_runtime_webgl_validation_direct_geometry(false);
    else if (mark != NULL && strcmp(mark, "webgl-direct-on") == 0)
        script_runtime_webgl_validation_direct_geometry(true);
    else if (mark != NULL && strcmp(mark, "webgl-tint-stat") == 0) {
        uint64_t instances = 0, slices = 0, direct = 0, retired = 0;
        script_runtime_webgl_validation_tint_counts(&instances, &slices);
        script_runtime_webgl_validation_direct_counts(&direct, &retired);
        printf("tilefinch-webgl-tint: instances=%llu slices=%llu "
               "direct=%llu retired=%llu\n",
               (unsigned long long) instances, (unsigned long long) slices,
               (unsigned long long) direct, (unsigned long long) retired);
    }
    if (mark != NULL && strcmp(mark, "canvas-ge-on") == 0)
        psp_present_canvas_ge_enable(true, false);
    else if (mark != NULL && strcmp(mark, "canvas-ge-verify") == 0)
        psp_present_canvas_ge_enable(true, true);
    else if (mark != NULL && (strcmp(mark, "canvas-ge-off") == 0
                              || strcmp(mark, "canvas-cpu") == 0))
        psp_present_canvas_ge_enable(false, false);
    else if (mark != NULL && strcmp(mark, "canvas-ge-stat") == 0) {
        const CanvasGePresenterStats *ge = psp_present_canvas_ge_stats();
        printf("tilefinch-canvas-ge: presents=%lu ge-refusals=%lu "
               "failures=%lu foreign-refusals=%lu busy-refusals=%lu "
               "materializations=%lu materialize-failures=%lu "
               "verify-frames=%lu verify-bad-frames=%lu "
               "verify-unavailable=%lu verify-pixels=%llu "
               "verify-mismatches=%llu ge-avg=%lluus ge-max=%lluus "
               "defer=%u\n",
               (unsigned long) ge->presents, (unsigned long) ge->ge_refusals,
               (unsigned long) ge->failures,
               (unsigned long) ge->foreign_refusals,
               (unsigned long) ge->busy_refusals,
               (unsigned long) ge->materializations,
               (unsigned long) ge->materialize_failures,
               (unsigned long) ge->verify_frames,
               (unsigned long) ge->verify_bad_frames,
               (unsigned long) ge->verify_unavailable,
               (unsigned long long) ge->verify_pixels,
               (unsigned long long) ge->verify_mismatches,
               (unsigned long long) (ge->presents == 0 ? 0
                   : ge->ge_us / ge->presents),
               (unsigned long long) ge->ge_max_us,
               tile_cache_canvas_defer_enabled() ? 1u : 0u);
    }
    if (mark != NULL && strcmp(mark, "canvas-setup-on") == 0)
        tile_cache_validation_canvas_setup(true);
    else if (mark != NULL && strcmp(mark, "canvas-setup-off") == 0)
        tile_cache_validation_canvas_setup(false);
    else if (mark != NULL && strcmp(mark, "canvas-setup-probe") == 0)
        tile_cache_validation_canvas_setup_probe(psp_log_file());
    if (mark != NULL && strcmp(mark, "canvas-vfpu-on") == 0)
        tile_cache_validation_vfpu_conversion(true);
    else if (mark != NULL && strcmp(mark, "canvas-vfpu-off") == 0)
        tile_cache_validation_vfpu_conversion(false);
    else if (mark != NULL && strcmp(mark, "canvas-vfpu-check") == 0) {
        RenderCanvasVfpuProbe probe = tile_cache_validation_probe_vfpu_conversion();
        printf("tilefinch-canvas-vfpu-check: available=%u rows=%lu pixels=%lu "
               "mismatches=%lu state-mismatches=%lu\n",
               probe.available ? 1u : 0u, (unsigned long) probe.rows,
               (unsigned long) probe.pixels, (unsigned long) probe.mismatches,
               (unsigned long) probe.state_mismatches);
    }
    if (mark != NULL && strcmp(mark, "canvas-audio-on") == 0)
        psp_validation_native_detail = true;
    else if (mark != NULL && strcmp(mark, "canvas-audio-off") == 0)
        psp_validation_native_detail = false;
    if (mark != NULL && (strcmp(mark, "audio-mix-probe") == 0
                        || strcmp(mark, "audio-const-probe") == 0
                        || strcmp(mark, "audio-count-probe") == 0
                        || strcmp(mark, "audio-auto-probe") == 0)) {
        bool constant = strcmp(mark, "audio-const-probe") == 0;
        bool automation = strcmp(mark, "audio-auto-probe") == 0;
        bool observer = strcmp(mark, "audio-count-probe") == 0;
        TilefinchGameAudioMixProbe probe;
        bool okay = observer ? tilefinch_game_audio_validation_observer_probe(&probe)
            : automation ? tilefinch_game_audio_validation_automation_probe(&probe)
            : constant ? tilefinch_game_audio_validation_constant_probe(&probe)
            : tilefinch_game_audio_validation_mix_probe(&probe);
        if (observer) printf("tilefinch-audio-probe-kind: observer=1\n");
        else if (automation) printf("tilefinch-audio-probe-kind: automation=1\n");
        else printf("tilefinch-audio-probe-kind: constant=%u\n", constant ? 1u : 0u);
        printf("tilefinch-audio-probe: okay=%u blocks=%lu pcm-mismatches=%lu state-mismatches=%lu\n",
            okay ? 1u : 0u, (unsigned long) probe.blocks,
            (unsigned long) probe.pcm_mismatches, (unsigned long) probe.state_mismatches);
        for (size_t run = 0; run < 6u; run++)
            printf("tilefinch-audio-probe-run: run=%lu %s=%u blocks=256 mix-cpu=%lu "
                "mix-max=%lu observer=%lu\n", (unsigned long) run,
                observer ? "counted" : automation ? "automation" : constant ? "constant" : "fast", (run & 1u) ? 1u : 0u,
                (unsigned long) probe.mix_cpu_us[run], (unsigned long) probe.mix_max_us[run],
                (unsigned long) probe.observer_us[run]);
    }
    if (mark != NULL && strcmp(mark, "audio-mix-on") == 0)
        tilefinch_game_audio_validation_mix_fast(true);
    else if (mark != NULL && strcmp(mark, "audio-mix-off") == 0)
        tilefinch_game_audio_validation_mix_fast(false);
    if (mark != NULL && strcmp(mark, "audio-const-on") == 0)
        tilefinch_game_audio_validation_constant(true);
    else if (mark != NULL && strcmp(mark, "audio-const-off") == 0)
        tilefinch_game_audio_validation_constant(false);
    if (mark != NULL && strcmp(mark, "audio-auto-on") == 0)
        tilefinch_game_audio_validation_automation(true);
    else if (mark != NULL && strcmp(mark, "audio-auto-off") == 0)
        tilefinch_game_audio_validation_automation(false);
    if (mark != NULL && strcmp(mark, "phase-stat") == 0) {
        const NavigationSession *navigation = psp_webgl_measurement_navigation;
        bool okay = script_runtime_validation_phase_report(
            navigation == NULL ? NULL : navigation->page.runtime, psp_log_file());
        printf("tilefinch-phase-report: okay=%u\n", okay ? 1u : 0u);
    }
    if (mark != NULL && (strcmp(mark, "runtime-cpu-on") == 0
                         || strcmp(mark, "runtime-cpu-off") == 0)) {
        const NavigationSession *n = psp_webgl_measurement_navigation;
        bool enabled = strcmp(mark, "runtime-cpu-on") == 0;
        bool okay = script_runtime_validation_cpu_measure(n == NULL ? NULL : n->page.runtime, enabled);
        printf("tilefinch-runtime-cpu: enabled=%u okay=%u\n", enabled, okay);
    }
    if (mark != NULL && strcmp(mark, "phase-select-misses") == 0) {
        /* Post-window selection only. The private replay hook stores a
           fixed bitmap; no script evaluation occurs in the timed frame. */
        const NavigationSession *n = psp_webgl_measurement_navigation;
        bool okay = n != NULL && n->page.runtime != NULL
            && psp_canvas_cadence.pipeline_samples_dropped == 0u;
        unsigned misses = 0;
        char source[128];
        for (size_t at = 0; okay && at < psp_canvas_cadence.pipeline_count; at++) {
            const PspCanvasPhaseSample *s = &psp_canvas_cadence.phases[at];
            if (s->previous_latched_us == 0u
                || s->ready_us <= s->previous_latched_us
                || s->ready_us - s->previous_latched_us <= UINT64_C(33369)) continue;
            /* One recorded callback/presentation per sequence is required
               by this diagnostic hook, not assumed for arbitrary pages. */
            if (s->runtime_turns != 1u || s->ordinal == 0u) {okay=false; break;}
            snprintf(source, sizeof(source),
                "__treadlineDebug.fixedSelectFrame(%lu)", (unsigned long)(s->ordinal - 1u));
            okay = script_runtime_evaluate_probe(n->page.runtime, source,
                "<tilefinch-validation-frame-select>", NULL);
            misses++;
        }
        printf("tilefinch-phase-selection: okay=%u misses=%u\n", okay, misses);
    }
    if (mark != NULL && strcmp(mark, "canvas-stat") == 0)
        psp_report_canvas_cadence();
    if (mark != NULL && strcmp(mark, "audio-mix-start") == 0) {
        bool started = tilefinch_game_audio_validation_mix_measure(true);
        printf("tilefinch-audio-block: event=start okay=%u\n", started ? 1u : 0u);
    } else if (mark != NULL && (strcmp(mark, "audio-work-on") == 0
                              || strcmp(mark, "audio-work-off") == 0)) {
        bool enabled = strcmp(mark, "audio-work-on") == 0;
        bool okay = tilefinch_game_audio_validation_mix_work(enabled);
        printf("tilefinch-audio-work: enabled=%u okay=%u\n", enabled, okay);
    } else if (mark != NULL && strcmp(mark, "audio-mix-stat") == 0) {
        (void) tilefinch_game_audio_validation_mix_measure(false);
        TilefinchGameAudioMixMetrics m;
        bool read = tilefinch_game_audio_validation_mix_metrics(&m);
        printf("tilefinch-audio-block: event=stop okay=%u\n", read ? 1u : 0u);
        if (read) {
            printf("tilefinch-audio-block: blocks=%lu mix-cpu=%lu output-cpu=%lu "
                "observer=%lu mix-max=%lu output-max=%lu failed=%lu dropped=%lu\n",
                (unsigned long) m.blocks, (unsigned long) m.mix_cpu_us,
                (unsigned long) m.output_cpu_us, (unsigned long) m.observer_us,
                (unsigned long) m.mix_cpu_max_us, (unsigned long) m.output_cpu_max_us,
                (unsigned long) m.failed_clock_samples, (unsigned long) m.dropped_blocks);
            printf("tilefinch-audio-block-work: silent=%lu audible=%lu constant=%lu "
                "automated=%lu fast-forwarded=%lu\n",
                (unsigned long) m.silent_samples, (unsigned long) m.audible_samples,
                (unsigned long) m.constant_samples, (unsigned long) m.automated_samples,
                (unsigned long) m.fast_forwarded_samples);
            for (size_t bin = 0; bin < TILEFINCH_GAME_AUDIO_CPU_HISTOGRAM_BINS; bin++)
                if (m.mix_cpu_histogram[bin] != 0u)
                    printf("tilefinch-audio-block-bin: lower-us=%lu count=%lu\n",
                        (unsigned long) (bin * 100u),
                        (unsigned long) m.mix_cpu_histogram[bin]);
            psp_game_audio_report_blocks(&m);
        }
    }
    if (mark != NULL && strcmp(mark, "canvas-thread-on") == 0)
        psp_canvas_peer_begin(true);
    else if (mark != NULL && strcmp(mark, "canvas-thread-off") == 0)
        psp_canvas_peer_begin(false);
    if (mark != NULL && strcmp(mark, "canvas-trace-live") == 0)
        psp_validation_canvas_live_trace = true;
    else if (mark != NULL && strcmp(mark, "canvas-trace-quiet") == 0)
        psp_validation_canvas_live_trace = false;
    if (mark != NULL && strcmp(mark, "canvas-followup-off") == 0)
        psp_validation_canvas_followup_enabled = false;
    else if (mark != NULL && strcmp(mark, "canvas-followup-on") == 0)
        psp_validation_canvas_followup_enabled = true;
    if (mark != NULL && strcmp(mark, "media-stage-cpu") == 0)
        atomic_store(&psp_validation_video_copy_requested, 1);
    else if (mark != NULL && strcmp(mark, "media-stage-ge") == 0)
        atomic_store(&psp_validation_video_copy_requested, 2);
    if (mark != NULL && strcmp(mark, "video-measure-start") == 0)
        atomic_store(&psp_validation_video_window_requested, 1);
    else if (mark != NULL && strcmp(mark, "video-measure-end") == 0)
        atomic_store(&psp_validation_video_window_requested, 2);
    /* The start mark's own loop frame also runs that mark's diagnostics
       (page report, .mark.js) before its script turn; measuring from the
       next frame keeps that work out of the window. The page's profile hook
       still runs now, so a .mark.js on the same mark can override it. */
    if (mark != NULL && strcmp(mark, "webgl-measure-start") == 0) {
        const NavigationSession *navigation = psp_webgl_measurement_navigation;
        if (navigation != NULL && navigation->page.runtime != NULL) {
            (void) script_runtime_evaluate_diagnostic(
                navigation->page.runtime,
                "globalThis.__tilefinchStartInputProfile?.()",
                "<tilefinch-validation-profile-start>",
                NULL);
        }
        psp_webgl_measurement_reset_pending = true;
    }
    if (mark != NULL && strcmp(mark, "webgl-measure-end") == 0) {
        psp_canvas_cadence.causal_window = false;
        psp_canvas_cadence.window_closed = true;
        psp_present_validation_pre_publish_hook(NULL);
        psp_present_validation_measure_cpu(false);
        script_runtime_game_audio_native_measure(false);
        script_runtime_suppress_checkpoint_trace(
            psp_webgl_measurement_navigation == NULL ? NULL
                : psp_webgl_measurement_navigation->page.runtime, false);
        psp_log_end_timing_window();
    }
}

/* psp_advance_page_runtime's probes. The skipped-advance counters feed the
   runtime-cadence summary and the frame's loop-work record. */
void psp_vprobe_runtime_deferred(PspVprobeDeferral kind)
{
    switch (kind) {
    case PSP_VPROBE_DEFER_HANDOFF:
        psp_runtime_handoff_deferrals++;
        psp_frame_runtime.deferred_other++;
        break;
    case PSP_VPROBE_DEFER_RECLAIM:
        psp_runtime_reclaim_deferrals++;
        psp_frame_runtime.deferred_other++;
        break;
    case PSP_VPROBE_DEFER_RENDER:
        psp_runtime_render_deferrals++;
        psp_frame_runtime.deferred_render++;
        break;
    }
}

/* Brackets one browser_engine_advance_runtime call: the engine, script
   realm, WebGL and CPU clocks before it, then the differences after it,
   charged to the frame's loop-work record and, while the page has a canvas
   frame pending, to the causal canvas cadence. */
void psp_vprobe_runtime_begin(PspVprobeRuntime *probe, BrowserEngine *engine)
{
    PspCanvasPeerSample peer_before = psp_canvas_cadence.causal_window
        ? psp_canvas_peer_sample() : (PspCanvasPeerSample) {0};
    PspCanvasCpuSample cpu_before = psp_canvas_cadence.causal_window
        ? psp_canvas_cpu_sample() : (PspCanvasCpuSample) {0};
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    ScriptRuntimeTimingMetrics timing_before = {0};
    ScriptRuntimeCpuMetrics runtime_cpu_before = {0};
    ScriptWebglNativeMetrics webgl_before = {0};
    bool webgl_before_valid = !psp_canvas_cadence.causal_window
        || script_runtime_webgl_native_counters(&webgl_before);
    NavigationSession *timed_navigation =
        browser_engine_navigation(engine);
    uint64_t navigation_runtime_before = timed_navigation == NULL ? 0
        : timed_navigation->performance.runtime_us;
    ScriptRuntime *timed_runtime = timed_navigation == NULL
        ? NULL : timed_navigation->page.runtime;
    (void) script_runtime_timing_metrics(timed_runtime, &timing_before);
    bool runtime_cpu_before_valid = script_runtime_validation_cpu_metrics(timed_runtime, &runtime_cpu_before);
    uint64_t layout_before = timed_navigation == NULL ? 0
        : timed_navigation->performance.layout_us;
    uint64_t resource_before = timed_navigation == NULL ? 0
        : timed_navigation->performance.resource_us;
    NavigationPerformance phases_before = {0};
    if (timed_navigation != NULL)
        phases_before = timed_navigation->performance;
    uint64_t fetch_wait_before = 0;
    fetch_sync_wait_totals(&fetch_wait_before, NULL);
    *probe = (PspVprobeRuntime) {
        .peer_before = peer_before,
        .cpu_before = cpu_before,
        .started_us = started_us,
        .timing_before = timing_before,
        .runtime_cpu_before = runtime_cpu_before,
        .webgl_before = webgl_before,
        .webgl_before_valid = webgl_before_valid,
        .timed_navigation = timed_navigation,
        .navigation_runtime_before = navigation_runtime_before,
        .runtime_cpu_before_valid = runtime_cpu_before_valid,
        .layout_before = layout_before,
        .resource_before = resource_before,
        .phases_before = phases_before,
        .fetch_wait_before = fetch_wait_before
    };
}

void psp_vprobe_runtime_end(
    const PspVprobeRuntime *probe, BrowserEngine *engine,
    const bool *layout_changed)
{
    PspCanvasPeerSample peer_before = probe->peer_before;
    PspCanvasCpuSample cpu_before = probe->cpu_before;
    uint64_t started_us = probe->started_us;
    ScriptRuntimeTimingMetrics timing_before = probe->timing_before;
    ScriptRuntimeCpuMetrics runtime_cpu_before = probe->runtime_cpu_before;
    ScriptWebglNativeMetrics webgl_before = probe->webgl_before;
    bool webgl_before_valid = probe->webgl_before_valid;
    NavigationSession *timed_navigation = probe->timed_navigation;
    uint64_t navigation_runtime_before = probe->navigation_runtime_before;
    bool runtime_cpu_before_valid = probe->runtime_cpu_before_valid;
    uint64_t layout_before = probe->layout_before;
    uint64_t resource_before = probe->resource_before;
    NavigationPerformance phases_before = probe->phases_before;
    uint64_t fetch_wait_before = probe->fetch_wait_before;
    ScriptRuntime *timed_runtime;
    uint64_t elapsed_us =
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    PspCanvasCpuSample cpu_after = psp_canvas_cadence.causal_window
        ? psp_canvas_cpu_sample() : (PspCanvasCpuSample) {0};
    PspCanvasPeerSample peer_after = psp_canvas_cadence.causal_window
        ? psp_canvas_peer_sample() : (PspCanvasPeerSample) {0};
    ScriptRuntimeTimingMetrics timing_after = {0};
    ScriptRuntimeCpuMetrics runtime_cpu_after = {0};
    ScriptWebglNativeMetrics webgl_after = {0};
    bool webgl_after_valid = !psp_canvas_cadence.causal_window
        || script_runtime_webgl_native_counters(&webgl_after);
    /* Cancellation/allocation refusal can retire the runtime in this call. */
    timed_runtime = timed_navigation == NULL ? NULL
        : timed_navigation->page.runtime;
    (void) script_runtime_timing_metrics(timed_runtime, &timing_after);
    bool runtime_cpu_after_valid = script_runtime_validation_cpu_metrics(timed_runtime, &runtime_cpu_after);
    uint64_t callback_us = timing_after.timer_callback_us
            >= timing_before.timer_callback_us
        ? timing_after.timer_callback_us - timing_before.timer_callback_us : 0;
    uint64_t webgl_us = timing_after.webgl_native_us
            >= timing_before.webgl_native_us
        ? timing_after.webgl_native_us - timing_before.webgl_native_us : 0;
    uint64_t microtask_us = timing_after.microtask_us
            >= timing_before.microtask_us
        ? timing_after.microtask_us - timing_before.microtask_us : 0;
    uint64_t js_allocations = timing_after.js_allocation_calls
            >= timing_before.js_allocation_calls
        ? timing_after.js_allocation_calls
            - timing_before.js_allocation_calls : 0;
    uint64_t js_frees = timing_after.js_free_calls
            >= timing_before.js_free_calls
        ? timing_after.js_free_calls - timing_before.js_free_calls : 0;
    uint64_t js_reallocations = timing_after.js_reallocation_calls
            >= timing_before.js_reallocation_calls
        ? timing_after.js_reallocation_calls
            - timing_before.js_reallocation_calls : 0;
    uint64_t js_allocated_bytes = timing_after.js_allocated_bytes
            >= timing_before.js_allocated_bytes
        ? timing_after.js_allocated_bytes - timing_before.js_allocated_bytes
        : 0;
    uint64_t js_freed_bytes = timing_after.js_freed_bytes
            >= timing_before.js_freed_bytes
        ? timing_after.js_freed_bytes - timing_before.js_freed_bytes : 0;
    uint64_t script_runtime_us = timing_after.total_us
            >= timing_before.total_us
        ? timing_after.total_us - timing_before.total_us : 0;
    uint64_t navigation_runtime_us = timed_navigation != NULL
            && timed_navigation->performance.runtime_us
                   >= navigation_runtime_before
        ? timed_navigation->performance.runtime_us
            - navigation_runtime_before : 0;
    uint64_t damage_apply_us = elapsed_us > navigation_runtime_us
        ? elapsed_us - navigation_runtime_us : 0;
    uint64_t fetch_wait_after = 0;
    fetch_sync_wait_totals(&fetch_wait_after, NULL);
    if (elapsed_us > UINT64_C(100000)) {
        const NavigationPerformance *after = timed_navigation == NULL
            ? &phases_before : &timed_navigation->performance;
        printf("tilefinch-page-slow-advance: call=%zu elapsed=%lluus "
               "script=%lluus layout=%lluus resource=%lluus damage=%lluus "
               "page-call=%lluus frames=%lluus frame-script=%lluus "
               "messages=%lluus frame-setup=%lluus relayout=%lluus "
               "navigation=%lluus fetch-wait=%lluus\n",
               psp_runtime_advance_calls, (unsigned long long) elapsed_us,
               (unsigned long long) script_runtime_us,
               (unsigned long long) (timed_navigation == NULL ? 0
                   : timed_navigation->performance.layout_us - layout_before),
               (unsigned long long) (timed_navigation == NULL ? 0
                   : timed_navigation->performance.resource_us - resource_before),
               (unsigned long long) damage_apply_us,
               (unsigned long long) (after->advance_page_script_us
                   - phases_before.advance_page_script_us),
               (unsigned long long) (after->advance_frames_us
                   - phases_before.advance_frames_us),
               (unsigned long long) (after->advance_frame_script_us
                   - phases_before.advance_frame_script_us),
               (unsigned long long) (after->advance_messages_us
                   - phases_before.advance_messages_us),
               (unsigned long long) (after->advance_frame_setup_us
                   - phases_before.advance_frame_setup_us),
               (unsigned long long) (after->advance_relayout_us
                   - phases_before.advance_relayout_us),
               (unsigned long long) navigation_runtime_us,
               (unsigned long long) (fetch_wait_after - fetch_wait_before));
    }
    {
        const NavigationPerformance *after = timed_navigation == NULL
            ? &phases_before : &timed_navigation->performance;
        psp_frame_runtime.advances++;
        psp_frame_runtime.elapsed_us += elapsed_us;
        psp_frame_runtime.realm_us += script_runtime_us;
        psp_frame_runtime.prelude_us +=
            timing_after.prelude_us - timing_before.prelude_us;
        psp_frame_runtime.timer_prepare_us +=
            timing_after.timer_prepare_us - timing_before.timer_prepare_us;
        psp_frame_runtime.timer_us += callback_us;
        psp_frame_runtime.timers +=
            timing_after.timer_callbacks - timing_before.timer_callbacks;
        psp_frame_runtime.jobs_us += microtask_us;
        psp_frame_runtime.network_us +=
            timing_after.network_us - timing_before.network_us;
        psp_frame_runtime.refresh_us +=
            timing_after.refresh_us - timing_before.refresh_us;
        psp_frame_runtime.child_us += after->advance_frame_script_us
            - phases_before.advance_frame_script_us;
        psp_frame_runtime.setup_us += after->advance_frame_setup_us
            - phases_before.advance_frame_setup_us;
        psp_frame_runtime.messages_us += after->advance_messages_us
            - phases_before.advance_messages_us;
        psp_frame_runtime.relayout_us += after->advance_relayout_us
            - phases_before.advance_relayout_us;
        psp_frame_runtime.damage_us += damage_apply_us;
        psp_frame_runtime.late_us +=
            timing_after.timer_late_us - timing_before.timer_late_us;
        if (timing_after.advance_timer_late_max_us
                > psp_frame_runtime.late_max_us)
            psp_frame_runtime.late_max_us =
                timing_after.advance_timer_late_max_us;
        psp_frame_runtime.late_frames +=
            timing_after.timer_late_frames - timing_before.timer_late_frames;
        psp_frame_runtime.frame_callbacks +=
            timing_after.frame_callbacks - timing_before.frame_callbacks;
        psp_frame_runtime.frame_callback_late_us +=
            timing_after.frame_callback_late_us
            - timing_before.frame_callback_late_us;
    }
    psp_runtime_advance_calls++;
    psp_runtime_advance_total_us += elapsed_us;
    if (elapsed_us > psp_runtime_advance_maximum_us)
        psp_runtime_advance_maximum_us = elapsed_us;
    if (layout_changed != NULL && *layout_changed)
        psp_runtime_layout_changes++;
    if (browser_engine_canvas_frame_pending(engine)) {
        psp_canvas_cadence_started(
            started_us, elapsed_us, callback_us, webgl_us, microtask_us,
            script_runtime_us, navigation_runtime_us, damage_apply_us,
            js_allocations, js_frees, js_reallocations,
            js_allocated_bytes, js_freed_bytes,
            timing_before.js_live_bytes, timing_after.js_live_bytes);
        psp_canvas_cadence_cpu(&cpu_before, &cpu_after, &peer_before, &peer_after);
        psp_canvas_cadence_webgl(&webgl_before, &webgl_after,
                                webgl_before_valid && webgl_after_valid);
        psp_canvas_cadence_runtime_cpu(&runtime_cpu_before, &runtime_cpu_after,
                                     runtime_cpu_before_valid && runtime_cpu_after_valid);
    }
}

/* Files one interactive-loop record with the causal canvas window: beside
   the canvas sample that loop produced, or as a gap when it produced none.
   Returns false outside the window, where the caller prints the record. */
bool psp_canvas_cadence_take_loop(
    uint64_t start_us, uint64_t end_us,
    const uint64_t phase_us[PSP_CANVAS_LOOP_PHASES], int64_t due_us,
    bool timer_known, bool timer_frame, bool fast_followup)
{
    PspCanvasPhaseSample *sample = psp_canvas_cadence.pipeline_count == 0
        ? NULL : &psp_canvas_cadence.phases[
            psp_canvas_cadence.pipeline_count - 1u];
    bool matches = sample != NULL && sample->causal_window
        && sample->started_us >= start_us
        && sample->latched_us <= end_us;
    if (!psp_canvas_cadence.causal_window && !matches) return false;
    if (matches) {
        sample->loop_start_us = start_us;
        sample->loop_end_us = end_us;
        for (size_t phase = 0; phase < LOOP_PHASE_COUNT; phase++)
            sample->loop_phase_us[phase] = psp_canvas_cadence_u32(
                phase_us[phase]);
    } else {
        size_t at = psp_canvas_cadence.causal_loops_without_sample++;
        if (at < PSP_CANVAS_LOOP_GAP_LIMIT)
            psp_canvas_cadence.causal_loop_gaps[at] = (PspCanvasLoopGap) {
                .start_us = start_us,
                .end_us = end_us,
                .wait_us = psp_canvas_cadence_u32(phase_us[LOOP_WAIT]),
                .runtime_us = psp_canvas_cadence_u32(phase_us[LOOP_RUNTIME]),
                .callbacks = psp_canvas_cadence_u32(psp_frame_runtime.frame_callbacks),
                .due_us = due_us,
                .timer_known = timer_known,
                .timer_frame = timer_frame,
                .fast_followup = fast_followup
            };
    }
    return true;
}

void psp_report_runtime_cadence(void)
{
    if (psp_runtime_advance_calls != 0) {
        printf("tilefinch-runtime-cadence: calls=%zu changes=%zu "
               "render-deferrals=%zu handoff-deferrals=%zu "
               "reclaim-deferrals=%zu average=%lluus maximum=%lluus\n",
               psp_runtime_advance_calls, psp_runtime_layout_changes,
               psp_runtime_render_deferrals, psp_runtime_handoff_deferrals,
               psp_runtime_reclaim_deferrals,
               (unsigned long long) (psp_runtime_advance_total_us
                   / psp_runtime_advance_calls),
               (unsigned long long) psp_runtime_advance_maximum_us);
    }
}

/* Brackets a page publication: its cadence sample, and its
   tilefinch-page-publication line outside the measured windows. */
void psp_vprobe_present_begin(PspVprobePresent *probe)
{
    probe->presents_before = psp_display.presents;
    probe->started_us = (uint64_t) sceKernelGetSystemTimeWide();
}

void psp_vprobe_present_end(
    const PspVprobePresent *probe, unsigned render_visual_state,
    bool published, const BrowserEngine *engine)
{
    uint64_t canvas_present_finished_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    psp_canvas_cadence_observe_present(
        render_visual_state, probe->presents_before,
        probe->started_us,
        canvas_present_finished_us);
    if (psp_input_script_running()
        && psp_validation_video_timing_window)
        psp_validation_video_suppressed_publications++;
    if (psp_input_script_running()
        && !psp_validation_video_timing_window
        && (!psp_canvas_cadence.causal_window
            || psp_validation_canvas_live_trace)) {
        BrowserValidationCounters counters;
        (void) browser_engine_validation_counters(engine, &counters);
        printf("tilefinch-page-publication: at-us=%llu success=%d "
               "presents=%u layout-text=%llu\n",
               (unsigned long long) canvas_present_finished_us,
               published ? 1 : 0, psp_display.presents,
               (unsigned long long) counters.layout_text_fingerprint);
    }
}

/* Brackets one render-job slice of a canvas frame. */
void psp_vprobe_render_begin(
    PspVprobeRender *probe, bool canvas_render, const TileCache *render_stats)
{
    uint64_t canvas_raster_before = canvas_render && render_stats != NULL
        ? render_stats->canvas_fast_raster_us : 0;
    uint64_t canvas_overlay_before = canvas_render && render_stats != NULL
        ? render_stats->canvas_fast_overlay_us : 0;
    size_t canvas_frames_before = canvas_render && render_stats != NULL
        ? render_stats->canvas_fast_frames : 0;
    PspCanvasCpuSample render_cpu_before = {0};
    if (canvas_render && psp_canvas_cadence.causal_window)
        render_cpu_before = psp_canvas_cpu_sample();
    uint64_t canvas_render_started_us = canvas_render
        ? (uint64_t) sceKernelGetSystemTimeWide() : 0;
    *probe = (PspVprobeRender) {
        .canvas_raster_before = canvas_raster_before,
        .canvas_overlay_before = canvas_overlay_before,
        .canvas_frames_before = canvas_frames_before,
        .cpu_before = render_cpu_before,
        .started_us = canvas_render_started_us
    };
}

void psp_vprobe_render_end(
    const PspVprobeRender *probe, bool canvas_render,
    const TileCache *render_stats, uint64_t render_now_us)
{
    uint64_t canvas_raster_before = probe->canvas_raster_before;
    uint64_t canvas_overlay_before = probe->canvas_overlay_before;
    size_t canvas_frames_before = probe->canvas_frames_before;
    PspCanvasCpuSample render_cpu_before = probe->cpu_before;
    if (canvas_render) {
        PspCanvasCpuSample render_cpu_after = {0};
        if (psp_canvas_cadence.causal_window)
            render_cpu_after = psp_canvas_cpu_sample();
        psp_canvas_cadence_rendered(
            render_now_us - probe->started_us,
            render_stats == NULL
                || render_stats->canvas_fast_raster_us < canvas_raster_before ? 0
                : render_stats->canvas_fast_raster_us - canvas_raster_before,
            render_stats == NULL
                || render_stats->canvas_fast_overlay_us < canvas_overlay_before ? 0
                : render_stats->canvas_fast_overlay_us - canvas_overlay_before,
            &render_cpu_before, &render_cpu_after,
            render_stats != NULL && render_stats->canvas_fast_frames > canvas_frames_before
                ? &render_stats->canvas_fast_conversion : NULL);
    }
}

void psp_vprobe_frame_begin(
    const NavigationSession *navigation, bool requested_native_audio)
{
    psp_webgl_measurement_navigation = navigation;
    /* The main owner has not entered this frame's JS/native work. A busy
       supervisor must wait for this checkpoint, never inspect the realm. */
    psp_audio_slots_receipt(navigation, requested_native_audio);
}

void psp_webgl_measurement_bind(const NavigationSession *navigation)
{
    psp_webgl_measurement_navigation = navigation;
}

bool psp_validation_canvas_followup_disabled(void)
{
    return !psp_validation_canvas_followup_enabled;
}

/* The controlled exit's measurement summaries: render suppression and
   animation cadence, image, WebGL and JavaScript totals, then the canvas
   and runtime cadence records. */
void psp_report_session_measurements(
    PspBrowserResources *browser, PspEngineViews *engine_views)
{
    BrowserEngineMetrics exit_engine_metrics = {0};
    if (browser_engine_metrics(browser->engine, &exit_engine_metrics)) {
        printf("tilefinch-render-suppression: "
               "unchanged-runtime-frames=%zu rendered-frames=%zu\n",
               exit_engine_metrics.unchanged_runtime_frames_suppressed,
               exit_engine_metrics.rendered_frames);
    }
    const TileCache *exit_render =
        browser_engine_render_metrics_view(browser->engine);
    if (exit_render != NULL) {
        printf("tilefinch-animation-cadence: rendered=%zu scheduled=%zu "
               "completed=%zu cancelled=%zu slices=%zu units=%zu "
               "budget-exhausted=%zu max-slice=%lluus max-unit=%lluus "
               "scaled-builds=%zu scaled-us=%lluus "
               "canvas-fast=%zu/%lluus max=%lluus refusals=%zu "
               "canvas-raster=%lluus canvas-overlay=%lluus "
               "frame-us=%lluus tile-us=%lluus overflow-us=%lluus "
               "fixed-us=%lluus\n",
               exit_render->frames_rendered,
               exit_render->frame_jobs_scheduled,
               exit_render->frame_jobs_completed,
               exit_render->frame_jobs_cancelled,
               exit_render->frame_job_slices,
               exit_render->frame_job_units,
               exit_render->frame_job_budget_exhaustions,
               (unsigned long long) exit_render->max_frame_job_slice_us,
               (unsigned long long) exit_render->max_frame_job_unit_us,
               exit_render->scaled_image_builds,
               (unsigned long long) exit_render->scaled_image_us,
               exit_render->canvas_fast_frames,
               (unsigned long long) exit_render->canvas_fast_us,
               (unsigned long long) exit_render->canvas_fast_max_us,
               exit_render->canvas_fast_refusals,
               (unsigned long long) exit_render->canvas_fast_raster_us,
               (unsigned long long) exit_render->canvas_fast_overlay_us,
               (unsigned long long) exit_render->frame_us,
               (unsigned long long) exit_render->frame_tile_us,
               (unsigned long long) exit_render->frame_overflow_us,
               (unsigned long long) exit_render->frame_fixed_us);
    }
    ScriptWebglNativeMetrics webgl_metrics = {0};
    if (script_runtime_webgl_native_metrics(&webgl_metrics)) {
        printf("tilefinch-webgl-cadence: frames=%zu vertices=%zu "
               "seed-avg=%lluus command-avg=%lluus sync-avg=%lluus "
               "readback-avg=%lluus total-avg=%lluus total-max=%lluus "
               "max-phases=%llu/%llu/%llu/%lluus max-vertices=%zu "
               "max-aa=%zu/%zu "
               "total-p50=%lluus total-p95=%lluus sampled=%zu "
               "scratch-avg=%llu scratch-max=%zu owner-resets=%zu "
               "capacity-resets=%zu texture-upload=%zu fast-vertices=%zu "
               "geometry-hit-vertices=%zu geometry-misses=%zu "
               "command-phases=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%lluus "
               "instance-matrix-color=%llu/%lluus "
               "commands=%zu draws=%zu matrices=%zu avoided=%zu "
               "viewport-states=%zu avoided=%zu list-bytes=%zu max=%zu "
               "antialias-draws=%zu antialias-edge-indices=%zu "
               "aa-compact=%zu aa-radius-lt2-lt3-lt4=%zu/%zu/%zu\n",
               webgl_metrics.frames, webgl_metrics.vertices,
               (unsigned long long) (webgl_metrics.seed_us
                   / webgl_metrics.frames),
               (unsigned long long) (webgl_metrics.command_us
                   / webgl_metrics.frames),
               (unsigned long long) (webgl_metrics.sync_us
                   / webgl_metrics.frames),
               (unsigned long long) (webgl_metrics.readback_us
                   / webgl_metrics.frames),
               (unsigned long long) (webgl_metrics.total_us
                   / webgl_metrics.frames),
               (unsigned long long) webgl_metrics.maximum_total_us,
               (unsigned long long) webgl_metrics.maximum_total_seed_us,
               (unsigned long long) webgl_metrics.maximum_total_command_us,
               (unsigned long long) webgl_metrics.maximum_total_sync_us,
               (unsigned long long) webgl_metrics.maximum_total_readback_us,
               webgl_metrics.maximum_total_vertices,
               webgl_metrics.maximum_total_antialias_draws,
               webgl_metrics.maximum_total_antialias_edge_indices,
               (unsigned long long) webgl_metrics.median_total_us,
               (unsigned long long) webgl_metrics.p95_total_us,
               webgl_metrics.sampled_frames,
               (unsigned long long) (webgl_metrics.scratch_bytes
                   / webgl_metrics.frames),
               webgl_metrics.maximum_scratch_bytes,
               webgl_metrics.texture_cache_owner_resets,
               webgl_metrics.texture_cache_capacity_resets,
               webgl_metrics.texture_upload_bytes,
               webgl_metrics.fast_vertices,
               webgl_metrics.geometry_cache_hit_vertices,
               webgl_metrics.geometry_cache_misses,
               (unsigned long long) webgl_metrics.command_decode_us,
               (unsigned long long) webgl_metrics.command_cache_us,
               (unsigned long long) webgl_metrics.command_vertex_us,
               (unsigned long long) webgl_metrics.command_state_us,
               (unsigned long long) webgl_metrics.command_emit_us,
               (unsigned long long) webgl_metrics.command_antialias_us,
               (unsigned long long) webgl_metrics.command_writeback_us,
               (unsigned long long) webgl_metrics.command_finalize_us,
               (unsigned long long)
                   webgl_metrics.command_instance_matrix_us,
               (unsigned long long)
                   webgl_metrics.command_instance_color_us,
               webgl_metrics.commands,
               webgl_metrics.draw_calls,
               webgl_metrics.matrix_loads,
               webgl_metrics.matrix_loads_avoided,
               webgl_metrics.state_changes,
               webgl_metrics.state_changes_avoided,
               webgl_metrics.display_list_bytes,
               webgl_metrics.maximum_display_list_bytes,
               webgl_metrics.antialias_draws,
               webgl_metrics.antialias_edge_indices,
               webgl_metrics.antialias_compact_instances,
               webgl_metrics.antialias_radius_lt2,
               webgl_metrics.antialias_radius_lt3,
               webgl_metrics.antialias_radius_lt4);
    }
    ScriptRuntimeTimingMetrics runtime_timing = {0};
    ScriptRuntime *active_runtime = engine_views->navigation == NULL
        ? NULL : engine_views->navigation->page.runtime;
    if (script_runtime_timing_metrics(active_runtime, &runtime_timing)) {
        uint64_t native_webgl_us = runtime_timing.webgl_native_us;
        uint64_t advance_exclusive_us =
            runtime_timing.total_us > native_webgl_us
                ? runtime_timing.total_us - native_webgl_us : 0;
        printf(
            "tilefinch-js-cadence: advances=%zu callbacks=%zu "
            "advance-avg=%lluus advance-max=%lluus callback-avg=%lluus "
            "callback-max=%lluus advance-exclusive-avg=%lluus "
            "advance-webgl=%zu/%lluus "
            "prelude-total=%lluus timer-prepare-total=%lluus "
            "network-total=%lluus microtask-total=%lluus "
            "refresh-total=%lluus\n",
            runtime_timing.advances, runtime_timing.timer_callbacks,
            (unsigned long long) (runtime_timing.total_us
                / runtime_timing.advances),
            (unsigned long long) runtime_timing.maximum_total_us,
            (unsigned long long) (runtime_timing.timer_callbacks == 0 ? 0
                : runtime_timing.timer_callback_us
                    / runtime_timing.timer_callbacks),
            (unsigned long long)
                runtime_timing.maximum_timer_callback_us,
            (unsigned long long) (runtime_timing.advances == 0 ? 0
                : advance_exclusive_us / runtime_timing.advances),
            runtime_timing.webgl_native_frames,
            (unsigned long long) runtime_timing.webgl_native_us,
            (unsigned long long) runtime_timing.prelude_us,
            (unsigned long long) runtime_timing.timer_prepare_us,
            (unsigned long long) runtime_timing.network_us,
            (unsigned long long) runtime_timing.microtask_us,
            (unsigned long long) runtime_timing.refresh_us);
        /* Garbage census: risen bytes are heap-garbage production invisible
           to pool-block counters; drops are full collections caught between
           advances; the probe times one forced collection on this realm so
           the pause a paced GC costs here is a measurement, not a claim. */
        printf(
            "tilefinch-js-heap: samples=%zu end=%zuB low=%zuB high=%zuB "
            "risen-total=%lluB risen-per-advance=%lluB gc-drops=%zu "
            "gc-drop-max=%zuB gc-probe=%lluus\n",
            runtime_timing.js_heap_samples,
            runtime_timing.js_heap_last_bytes,
            runtime_timing.js_heap_low_bytes,
            runtime_timing.js_heap_high_bytes,
            (unsigned long long) runtime_timing.js_heap_risen_bytes,
            (unsigned long long) (runtime_timing.js_heap_samples > 1
                ? runtime_timing.js_heap_risen_bytes
                    / (runtime_timing.js_heap_samples - 1u)
                : 0),
            runtime_timing.js_heap_gc_drops,
            runtime_timing.js_heap_gc_drop_max_bytes,
            (unsigned long long) script_runtime_gc_probe_us(active_runtime));
    }
    if (engine_views->navigation != NULL
        && engine_views->navigation->page.script_result.summary[0] != '\0') {
        printf("tilefinch-js-summary: %.2047s\n",
               engine_views->navigation->page.script_result.summary);
    }
    psp_report_canvas_cadence();
    psp_report_runtime_cadence();
}
