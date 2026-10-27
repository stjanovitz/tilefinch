/* Seams between the browser EBOOT and its validation instrumentation.
 *
 * Measurement, qualification and soak code lives in the
 * psp_app_validation_*.c translation units, which are compiled into
 * psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is on (see
 * cmake/TilefinchTargets.cmake). Production steps reach it through the
 * hooks below. In a shipping build each hook is a static inline no-op, so a
 * step carries one call instead of an #ifdef block and the compiler emits
 * nothing for it: the shipping EBOOT's code is the same with or without the
 * call.
 */
#ifndef TILEFINCH_PSP_APP_VALIDATION_H
#define TILEFINCH_PSP_APP_VALIDATION_H

#include "psp_app_internal.h"
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include <time.h>
#include "tilefinch/psp_time.h"
#endif

/* A production-file function the validation units also call. It keeps
   internal linkage in shipping builds, so moving its callers out of the
   file cannot change how the shipping compiler treats it. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#define PSP_VALIDATION_SHARED
#else
#define PSP_VALIDATION_SHARED static
#endif

/* The interactive loop's carried state (psp_app_loop.h). */
typedef struct PspLoop PspLoop;

/* Why psp_advance_page_runtime skipped an advance. */
typedef enum {
    PSP_VPROBE_DEFER_HANDOFF,
    PSP_VPROBE_DEFER_RECLAIM,
    PSP_VPROBE_DEFER_RENDER
} PspVprobeDeferral;

#ifdef TILEFINCH_PSP_VALIDATION_LOG

/* src/psp_script_main.c, shared with the validation units. */
void psp_exit_console(const char *exit_to);

/* src/psp_app/psp_app_validation_qualify.c: boot.cfg qualification modes
   that replace the interactive session. Each returns true when it ran a
   qualification, with the process exit status in *exit_code; false means
   boot continues. */
bool psp_validation_engine_qualifications(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code);
bool psp_validation_raster_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code);
bool psp_validation_network_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    int *exit_code);

/* Boot reports (psp_app_validation_qualify.c). main starts the boot clock
   with psp_boot_timing_begin; PSP_BOOT_TIMING_MARK then works in main and
   in the boot phases it calls (psp_app_boot.c) alike. */
void psp_boot_timing_begin(uint64_t entered_us);
#define PSP_BOOT_TIMING_MARK(stage_name) psp_boot_timing_mark((stage_name))
void psp_boot_timing_mark(const char *stage);
void psp_boot_report_handoff(
    int argc, char *argv[], uint64_t entered_us, uint64_t surface_us);
void psp_log_engine_creation(const BrowserEngine *engine);
void psp_boot_input_probe(uint64_t sample_us);
void psp_report_heap_capacity_lower_bound(void);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
void psp_preload_validation_ca_bundle(const TilefinchInstallPaths *paths);
#endif
void psp_report_tls_assets(
    size_t bundle_bytes,
    const uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES],
    time_t tls_time, const PspTimeFields *rtc, PspTimeStatus rtc_status);
void psp_dump_final_frame(const char *argv0);
void psp_validation_apply_env_knobs(const PspBootConfig *config);

/* src/psp_app/psp_app_validation_cadence.c */

/* Wraps the system display backend with publication timing (flush,
   framebuffer set, vblank) and latch observations. */
const PspDisplayBackend *psp_validation_display_wrap(
    const PspDisplayBackend *inner);

/* The interactive loop's phases (psp_loop_frame's PSP_LOOP_PHASE marks):
   one tilefinch-loop-timing column each, and the loop columns of a causal
   canvas sample. */
enum {
    LOOP_WAIT, LOOP_INPUT, LOOP_ACTION, LOOP_PROBE, LOOP_NAVIGATION,
    LOOP_RUNTIME, LOOP_IDLE, LOOP_RASTER, LOOP_PRESENT, LOOP_IMAGES,
    LOOP_TAIL, LOOP_PHASE_COUNT
};
#define PSP_CANVAS_LOOP_PHASES 11u
_Static_assert(LOOP_PHASE_COUNT == PSP_CANVAS_LOOP_PHASES,
               "causal loop record must cover every phase");

/* One frame's runtime advances, summed for its tilefinch-loop-work record:
   where an advance's time goes (the page realm's event-loop phases, child
   frames, relayout, render damage, and the PSP shell around them), how
   late its timer tasks ran, and why an advance was skipped. */
typedef struct {
    unsigned advances;
    unsigned deferred_render, deferred_other;
    uint64_t elapsed_us;      /* psp_advance_page_runtime's advance call */
    uint64_t realm_us;        /* script_runtime_advance, page realm */
    uint64_t prelude_us, timer_prepare_us, timer_us, jobs_us;
    uint64_t network_us, refresh_us;
    size_t timers;
    uint64_t child_us, setup_us, messages_us, relayout_us, damage_us;
    uint64_t late_us, late_max_us;
    size_t late_frames;
    size_t frame_callbacks;
    uint64_t frame_callback_late_us;
} PspFrameRuntime;
extern PspFrameRuntime psp_frame_runtime;

/* A video-measure window is open: routine loop and publication rows are
   counted, not formatted. */
extern bool psp_validation_video_timing_window;
extern unsigned psp_validation_video_suppressed_loops;

bool psp_canvas_cadence_take_loop(
    uint64_t start_us, uint64_t end_us,
    const uint64_t phase_us[PSP_CANVAS_LOOP_PHASES], int64_t due_us,
    bool timer_known, bool timer_frame, bool fast_followup);
void psp_report_canvas_cadence(void);
void psp_report_runtime_cadence(void);

typedef struct {
    uint64_t run_us;
    uint64_t query_begin_us, query_end_us;
    uint32_t interrupt_preempts, thread_preempts, releases;
    bool valid;
} PspCanvasCpuSample;

#define PSP_CANVAS_PEER_LIMIT 8u
typedef struct {
    uint64_t run_us[PSP_CANVAS_PEER_LIMIT];
    uint64_t query_begin_us, query_end_us;
    uint32_t valid_mask, span_us;
} PspCanvasPeerSample;

/* The probe hooks' saved state (see the hooks below). */
typedef struct {
    PspCanvasPeerSample peer_before;
    PspCanvasCpuSample cpu_before;
    uint64_t started_us;
    ScriptRuntimeTimingMetrics timing_before;
    ScriptRuntimeCpuMetrics runtime_cpu_before;
    ScriptWebglNativeMetrics webgl_before;
    bool webgl_before_valid;
    NavigationSession *timed_navigation;
    uint64_t navigation_runtime_before;
    bool runtime_cpu_before_valid;
    uint64_t layout_before;
    uint64_t resource_before;
    NavigationPerformance phases_before;
    uint64_t fetch_wait_before;
} PspVprobeRuntime;

typedef struct {
    unsigned presents_before;
    uint64_t started_us;
} PspVprobePresent;

typedef struct {
    uint64_t canvas_raster_before;
    uint64_t canvas_overlay_before;
    size_t canvas_frames_before;
    PspCanvasCpuSample cpu_before;
    uint64_t started_us;
} PspVprobeRender;

/* Probe hooks (psp_app_validation_cadence.c). Each begin/end pair brackets
   one production call: a page runtime advance, a page publication, a
   render-job slice. */
void psp_vprobe_runtime_deferred(PspVprobeDeferral kind);
void psp_vprobe_runtime_begin(PspVprobeRuntime *probe, BrowserEngine *engine);
void psp_vprobe_runtime_end(
    const PspVprobeRuntime *probe, BrowserEngine *engine,
    const bool *layout_changed);
void psp_vprobe_present_begin(PspVprobePresent *probe);
void psp_vprobe_present_end(
    const PspVprobePresent *probe, unsigned render_visual_state,
    bool published, const BrowserEngine *engine);
void psp_vprobe_render_begin(
    PspVprobeRender *probe, bool canvas_render, const TileCache *stats);
void psp_vprobe_render_end(
    const PspVprobeRender *probe, bool canvas_render,
    const TileCache *stats, uint64_t now_us);
/* The frame's first step and boot's hand-off to the loop: input-script
   marks follow the frontend's current navigation, and a queued
   audio-slots receipt is read at this owner checkpoint. */
void psp_vprobe_frame_begin(
    const NavigationSession *navigation, bool requested_native_audio);
void psp_webgl_measurement_bind(const NavigationSession *navigation);
/* The canvas-followup-off mark: no early wake for a canvas follow-up. */
bool psp_validation_canvas_followup_disabled(void);

/* src/psp_app/psp_app_validation_loop.c */

/* Per-frame loop timing: begin and end bracket psp_loop_frame, and
   PSP_LOOP_PHASE marks each phase boundary inside it. */
void psp_loop_timing_bind(PspBrowserResources *browser);
void psp_loop_timing_begin(void);
void psp_loop_timing_phase(unsigned phase);
void psp_loop_timing_note_wait(bool task_yield, bool fast_followup);
void psp_loop_timing_end(void);
#define PSP_LOOP_PHASE(phase) psp_loop_timing_phase(phase)

/* Validation frame steps, measured with the frame sets
   (cmake/CheckPspHotSymbolSizes.cmake). */
bool psp_loop_scripted_input(
    PspLoop *loop, PspUiInput *input, bool supervisor_page_input);
bool psp_loop_power_test_input(
    PspLoop *loop, PspUiInput *input, uint64_t now_us,
    bool *abort_requested);
bool psp_loop_power_test_tick(
    PspLoop *loop, uint64_t now_us, unsigned ui_elapsed_ms,
    PspClockWorkerSnapshot *power_snapshot, bool power_auto_abort_requested);

/* boot.cfg-knob drivers. psp_loop_frame calls them behind
   PSP_VALIDATION_KNOB, which folds to 0 in shipping builds. */
void psp_loop_inject_media_lifecycle(PspLoop *loop);
void psp_loop_count_validation_tick(PspLoop *loop);
void psp_loop_media_validation(PspLoop *loop, bool *media_visual_changed);

/* psp_app_run_interactive, psp_loop_frame and psp_browser_close hooks
   (psp_app_validation_loop.c). */
void psp_validation_interactive_begin(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, const char *argv0);
void psp_validation_interactive_end(
    PspProcessResources *process, PspInteractiveState *interactive);
bool psp_validation_scripted_frame(
    PspLoop *loop, PspUiInput *input, bool supervisor_page_input,
    PspAppFrameState *frame, unsigned *ui_elapsed_ms);
void psp_report_frame_pumps(const PspInteractiveState *interactive);
/* psp_app_validation_cadence.c */
void psp_report_session_measurements(
    PspBrowserResources *browser, PspEngineViews *engine_views);

/* src/psp_app/psp_app_validation_present.c */

typedef struct {
    uint64_t before, after, cpu;
    bool valid;
} PspMenuPhaseClock;

/* psp_app_runtime.c: the accepted scanout reused the front picture. */
extern bool psp_media_present_last_skipped;
/* psp_app_runtime.c: what each scanout buffer holds. */
extern PspMediaPresentRecord
    psp_media_present_records[PSP_DISPLAY_PAGE_BUFFER_COUNT];

typedef struct {
    uint64_t presentation_started_us;
    unsigned menu_key;
    unsigned menu_input_us;
    bool measure_menu;
    PspMenuPhaseClock menu_clocks[4];
    unsigned published_index;
} PspVprobeVideo;

typedef struct {
    uint64_t presentation_started_us;
    uint64_t base_finished_us;
    uint64_t composite_finished_us;
    uint64_t base_cpu_before, base_cpu_after;
    bool base_cpu_before_valid, base_cpu_after_valid;
    uint32_t base_bytes;
    PspUiCompositeTiming ui_timing;
    unsigned published_index;
    PspDisplayBackendTiming display_timing_before;
} PspVprobePhase;

typedef struct {
    PspMenuPhaseClock before;
} PspVprobeFonts;

/* Present-path probe hooks (psp_app_runtime.c). */
void psp_vprobe_track_menu_blit(
    bool blitted, const PspMediaPresentGeCost *cost,
    const uint16_t *source, int source_width, int source_height,
    int source_stride, bool source_changed, const uint32_t *destination,
    int destination_stride, int left, int top);
void psp_vprobe_video_begin(PspVprobeVideo *probe);
void psp_vprobe_video_skipped(bool skipped);
void psp_vprobe_video_drawn(PspVprobeVideo *probe);
void psp_vprobe_video_composed(PspVprobeVideo *probe);
void psp_vprobe_video_publish_returned(bool published);
void psp_vprobe_video_published(
    PspVprobeVideo *probe, bool published, bool menu_feedback,
    const uint32_t *vram);
void psp_vprobe_player_start(bool media_replaces_page);
void psp_vprobe_player_start_abandon(void);
void psp_vprobe_phase_begin(PspVprobePhase *phase);
void psp_vprobe_phase_base_bytes(PspVprobePhase *phase, uint32_t base_bytes);
void psp_vprobe_phase_copied_rows(
    PspVprobePhase *phase, unsigned opaque_top, unsigned copy_end);
void psp_vprobe_phase_base_done(PspVprobePhase *phase);
void psp_vprobe_phase_ui_composited(PspVprobePhase *phase);
void psp_vprobe_phase_composed(PspVprobePhase *phase, bool native_surface);
void psp_vprobe_phase_published(
    const PspVprobePhase *phase, bool published, bool media_visible,
    bool media_replaces_page, const uint16_t *frame, const PspUiState *ui,
    const uint16_t *vram);
void psp_vprobe_fonts_begin(PspVprobeFonts *probe);
void psp_vprobe_fonts_end(const PspVprobeFonts *probe);
void psp_vprobe_focus_first_visible(
    const BrowserEngine *engine, const NavigationSession *nav,
    uint64_t now_us);
/* The media presenter's mode line, once per opened stream or change of
   presenter, its cost totals, and load-time page-scroll responsiveness
   (psp_load_experience.h). */
void psp_media_present_report(
    PspMediaPresentMode mode, const char *reason,
    const MediaVideoFrame *frame, const PspMediaPresentPlan *plan,
    const PspMediaPresentTexture *texture);
void psp_vprobe_present_account(
    uint64_t started_us, const PspMediaPresentGeCost *cost, bool drawn);
void psp_load_scroll_note_press(void);
void psp_load_scroll_note_moved(void);

#else

static inline bool psp_validation_engine_qualifications(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code)
{
    (void) browser; (void) process; (void) argv0; (void) exit_code;
    return false;
}
static inline bool psp_validation_raster_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    const char *argv0, int *exit_code)
{
    (void) browser; (void) process; (void) argv0; (void) exit_code;
    return false;
}
static inline bool psp_validation_network_qualification(
    PspBrowserResources *browser, PspProcessResources *process,
    int *exit_code)
{
    (void) browser; (void) process; (void) exit_code;
    return false;
}

#define PSP_BOOT_TIMING_MARK(stage_name) do { (void) (stage_name); } while (0)
static inline void psp_log_engine_creation(const BrowserEngine *engine)
{
    (void) engine;
}
static inline void psp_boot_input_probe(uint64_t sample_us)
{
    (void) sample_us;
}
static inline void psp_validation_apply_env_knobs(const PspBootConfig *config)
{
    (void) config;
}


static inline const PspDisplayBackend *psp_validation_display_wrap(
    const PspDisplayBackend *inner)
{
    return inner;
}

/* In a shipping build a probe saves nothing; the hooks never touch it. */
typedef struct { char unused; } PspVprobeRuntime;
typedef struct { char unused; } PspVprobePresent;
typedef struct { char unused; } PspVprobeRender;

static inline void psp_vprobe_runtime_deferred(PspVprobeDeferral kind)
{
    (void) kind;
}
static inline void psp_vprobe_runtime_begin(
    PspVprobeRuntime *probe, BrowserEngine *engine)
{
    (void) probe; (void) engine;
}
static inline void psp_vprobe_runtime_end(
    const PspVprobeRuntime *probe, BrowserEngine *engine,
    const bool *layout_changed)
{
    (void) probe; (void) engine; (void) layout_changed;
}
static inline void psp_vprobe_present_begin(PspVprobePresent *probe)
{
    (void) probe;
}
static inline void psp_vprobe_present_end(
    const PspVprobePresent *probe, unsigned render_visual_state,
    bool published, const BrowserEngine *engine)
{
    (void) probe; (void) render_visual_state; (void) published;
    (void) engine;
}
static inline void psp_vprobe_render_begin(
    PspVprobeRender *probe, bool canvas_render, const TileCache *stats)
{
    (void) probe; (void) canvas_render; (void) stats;
}
static inline void psp_vprobe_render_end(
    const PspVprobeRender *probe, bool canvas_render,
    const TileCache *stats, uint64_t now_us)
{
    (void) probe; (void) canvas_render; (void) stats; (void) now_us;
}
static inline void psp_vprobe_frame_begin(
    const NavigationSession *navigation, bool requested_native_audio)
{
    (void) navigation; (void) requested_native_audio;
}
static inline void psp_webgl_measurement_bind(
    const NavigationSession *navigation)
{
    (void) navigation;
}
static inline bool psp_validation_canvas_followup_disabled(void)
{
    return false;
}

#define PSP_LOOP_PHASE(phase) ((void) 0)

static inline void psp_loop_timing_begin(void) {}
static inline void psp_loop_timing_end(void) {}
static inline void psp_validation_interactive_begin(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, const char *argv0)
{
    (void) process; (void) browser; (void) interactive; (void) argv0;
}
static inline void psp_validation_interactive_end(
    PspProcessResources *process, PspInteractiveState *interactive)
{
    (void) process; (void) interactive;
}
static inline bool psp_validation_scripted_frame(
    PspLoop *loop, PspUiInput *input, bool supervisor_page_input,
    PspAppFrameState *frame, unsigned *ui_elapsed_ms)
{
    (void) loop; (void) input; (void) supervisor_page_input; (void) frame;
    (void) ui_elapsed_ms;
    return true;
}
static inline void psp_report_frame_pumps(
    const PspInteractiveState *interactive)
{
    (void) interactive;
}
static inline void psp_report_session_measurements(
    PspBrowserResources *browser, PspEngineViews *engine_views)
{
    (void) browser; (void) engine_views;
}

static inline void psp_loop_inject_media_lifecycle(PspLoop *loop)
{
    (void) loop;
}
static inline void psp_loop_count_validation_tick(PspLoop *loop)
{
    (void) loop;
}
static inline void psp_loop_media_validation(
    PspLoop *loop, bool *media_visual_changed)
{
    (void) loop; (void) media_visual_changed;
}

typedef struct { char unused; } PspVprobeVideo;
typedef struct { char unused; } PspVprobePhase;
typedef struct { char unused; } PspVprobeFonts;

static inline void psp_vprobe_track_menu_blit(
    bool blitted, const PspMediaPresentGeCost *cost,
    const uint16_t *source, int source_width, int source_height,
    int source_stride, bool source_changed, const uint32_t *destination,
    int destination_stride, int left, int top)
{
    (void) blitted; (void) cost; (void) source; (void) source_width;
    (void) source_height; (void) source_stride; (void) source_changed;
    (void) destination; (void) destination_stride; (void) left; (void) top;
}
static inline void psp_vprobe_video_begin(PspVprobeVideo *probe)
{
    (void) probe;
}
static inline void psp_vprobe_video_skipped(bool skipped)
{
    (void) skipped;
}
static inline void psp_vprobe_video_drawn(PspVprobeVideo *probe)
{
    (void) probe;
}
static inline void psp_vprobe_video_composed(PspVprobeVideo *probe)
{
    (void) probe;
}
static inline void psp_vprobe_video_publish_returned(bool published)
{
    (void) published;
}
static inline void psp_vprobe_video_published(
    PspVprobeVideo *probe, bool published, bool menu_feedback,
    const uint32_t *vram)
{
    (void) probe; (void) published; (void) menu_feedback; (void) vram;
}
static inline void psp_vprobe_player_start(bool media_replaces_page)
{
    (void) media_replaces_page;
}
static inline void psp_vprobe_player_start_abandon(void) {}
static inline void psp_vprobe_phase_begin(PspVprobePhase *phase)
{
    (void) phase;
}
static inline void psp_vprobe_phase_base_bytes(
    PspVprobePhase *phase, uint32_t base_bytes)
{
    (void) phase; (void) base_bytes;
}
static inline void psp_vprobe_phase_copied_rows(
    PspVprobePhase *phase, unsigned opaque_top, unsigned copy_end)
{
    (void) phase; (void) opaque_top; (void) copy_end;
}
static inline void psp_vprobe_phase_base_done(PspVprobePhase *phase)
{
    (void) phase;
}
static inline void psp_vprobe_phase_ui_composited(PspVprobePhase *phase)
{
    (void) phase;
}
static inline void psp_vprobe_phase_composed(
    PspVprobePhase *phase, bool native_surface)
{
    (void) phase; (void) native_surface;
}
static inline void psp_vprobe_phase_published(
    const PspVprobePhase *phase, bool published, bool media_visible,
    bool media_replaces_page, const uint16_t *frame, const PspUiState *ui,
    const uint16_t *vram)
{
    (void) phase; (void) published; (void) media_visible;
    (void) media_replaces_page; (void) frame; (void) ui; (void) vram;
}
static inline void psp_vprobe_fonts_begin(PspVprobeFonts *probe)
{
    (void) probe;
}
static inline void psp_vprobe_fonts_end(const PspVprobeFonts *probe)
{
    (void) probe;
}
static inline void psp_vprobe_focus_first_visible(
    const BrowserEngine *engine, const NavigationSession *nav,
    uint64_t now_us)
{
    (void) engine; (void) nav; (void) now_us;
}
static inline void psp_media_present_report(
    PspMediaPresentMode mode, const char *reason,
    const MediaVideoFrame *frame, const PspMediaPresentPlan *plan,
    const PspMediaPresentTexture *texture)
{
    (void) mode;
    (void) reason;
    (void) frame;
    (void) plan;
    (void) texture;
}
static inline void psp_vprobe_present_account(
    uint64_t started_us, const PspMediaPresentGeCost *cost, bool drawn)
{
    (void) started_us;
    (void) cost;
    (void) drawn;
}
static inline void psp_load_scroll_note_press(void) {}
static inline void psp_load_scroll_note_moved(void) {}

#endif

#endif
