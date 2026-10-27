/* The interactive loop's validation drivers.
 *
 * Per-frame loop timing and the tilefinch-loop-timing / tilefinch-loop-work
 * records, the scripted-input and power-test frame steps, the boot.cfg
 * interactive tick limit, and the media play, stability and lifecycle
 * soak. psp_loop_frame reaches them through psp_app_validation.h; the
 * knob-gated steps there are static inline no-ops in shipping builds.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_loop.h"
#include "tilefinch_compiler.h"
#include "tilefinch/work_ledger.h"

#include <stdarg.h>

/* Stability telemetry excludes the first two seconds after priming, seeking,
   or buffering from its steady-state skew maximum. The all-phase maximum is
   retained beside it so discontinuities remain visible rather than hidden. */
#define PSP_MEDIA_STABILITY_STEADY_DELAY_US UINT64_C(2000000)

/* The stability report runs once at the end of an explicitly requested
   device soak. Keep its formatting and teardown out of the interactive
   loop's already-large instruction footprint. */
static TILEFINCH_COLD_PATH void
psp_finish_media_stability(PspApp *app, uint64_t elapsed_us)
{
    PspMediaSession *media = &app->browser->media;
    const PspBootConfig *config = &app->process->config;
    MediaBackendStats final_stats = {0};
    bool final_stats_ready = psp_media_backend_stats_snapshot(
        media, &final_stats);
    size_t decoded = media->accumulated_decoded_video_frames
        + final_stats.decoded_video_frames;
    size_t pipeline_skips = media->accumulated_dropped_video_frames
        + final_stats.dropped_video_frames;
    size_t seek_preroll_skips =
        media->accumulated_discarded_seek_video_frames
        + final_stats.discarded_seek_video_frames;
    size_t display_claims = media->accumulated_video_claims
        + final_stats.video_claims;
    size_t displayed = media->accumulated_video_claims_displayed
        + final_stats.video_claims_displayed;
    size_t display_drops = media->accumulated_video_claims_dropped
        + final_stats.video_claims_dropped;
    size_t quiesce_drops = media->accumulated_video_claims_quiesced
        + final_stats.video_claims_quiesced;
    size_t audio_packets = media->accumulated_audio_packets
        + final_stats.submitted_audio_packets;
    unsigned native_errors = media->accumulated_native_errors
        + (final_stats.last_native_error != 0 ? 1u : 0u);
    size_t peak_external = final_stats.external_bytes
            > media->peak_external_bytes
        ? final_stats.external_bytes : media->peak_external_bytes;
    int ending_capacity = scePowerGetBatteryRemainCapacity();
    int ending_percent = scePowerGetBatteryLifePercent();
    int capacity_delta = ending_capacity >= 0
            && app->interactive->media_stability_start_capacity >= 0
        ? ending_capacity - app->interactive->media_stability_start_capacity
        : INT_MIN;
    int percent_delta = ending_percent >= 0
            && app->interactive->media_stability_start_percent >= 0
        ? ending_percent - app->interactive->media_stability_start_percent
        : INT_MIN;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_power_log_battery(
        "media-finish", PSP_POWER_TEST_FIXED_HIGH, elapsed_us / 1000u);
#endif
    printf(
        "tilefinch-media-stability: event=skew-peak steady=%lluus "
        "at=%lluus audio=%lluus video=%lluus no-frame-ms=%u\n",
        (unsigned long long) app->interactive->media_stability_skew.steady_maximum_us,
        (unsigned long long)
            app->interactive->media_stability_skew.steady_peak_elapsed_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_peak_audio_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_peak_video_us,
        app->interactive->media_stability_skew.steady_peak_no_frame_ms);
    printf(
        "tilefinch-media-stability: event=complete stats-ready=%d "
        "elapsed=%lluus requested=%up actual=%dx%d decoded=%zu "
        "pipeline-skips=%zu seek-preroll-skips=%zu "
        "display-claims=%zu displayed=%zu "
        "display-drops=%zu quiesce-drops=%zu audio-packets=%zu "
        "native-errors=%u last-native=%d "
        "seeks=%u loops=%u fallbacks=%u peak-external=%zu min-free=%u "
        "min-largest=%u max-av-skew=%lluus steady-max-av-skew=%lluus "
        "steady-skew-samples=%u capacity-start=%d "
        "capacity-end=%d capacity-delta=%d percent-start=%d percent-end=%d "
        "percent-delta=%d scale-frames=%zu scale-total=%lluus "
        "scale-max=%lluus ge-frames=%zu ge-submit-total=%lluus "
        "ge-sync-total=%lluus ge-sync-max=%lluus skipped=%zu\n",
        final_stats_ready ? 1 : 0, (unsigned long long) elapsed_us,
        (unsigned) browser_profile_youtube_quality(app->browser->profile),
        media->stream.width, media->stream.height, decoded, pipeline_skips,
        seek_preroll_skips,
        display_claims, displayed, display_drops, quiesce_drops,
        audio_packets, native_errors, final_stats.last_native_error,
        media->seek_completions, app->interactive->media_stability_loops,
        media->quality_fallbacks, peak_external,
        app->interactive->media_stability_min_free,
        app->interactive->media_stability_min_largest,
        (unsigned long long) app->interactive->media_stability_skew.maximum_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_maximum_us,
        app->interactive->media_stability_skew.steady_samples,
        app->interactive->media_stability_start_capacity, ending_capacity,
        capacity_delta, app->interactive->media_stability_start_percent, ending_percent,
        percent_delta, media->present_scale_frames,
        (unsigned long long) media->present_scale_total_us,
        (unsigned long long) media->present_scale_max_us,
        media->present_ge_frames,
        (unsigned long long) media->present_ge_submit_total_us,
        (unsigned long long) media->present_ge_sync_total_us,
        (unsigned long long) media->present_ge_sync_max_us,
        media->present_skipped_frames);
    app->interactive->media_stability_active = false;
    app->process->presentation.ui.validation_media_test_phase = 0;
    media->ui.playing = false;
    if (media->playback != NULL)
        media_playback_set_playing(media->playback, false);
    char completed_status[64];
    snprintf(
        completed_status, sizeof(completed_status),
        "%ld SEC VIDEO TEST COMPLETE",
        config->validation_media_stability_seconds);
    psp_ui_show_status(&app->process->presentation.ui, completed_status, 600);
    if (app->interactive->media_stability_auto_exit)
        psp_exit_plan_request(
            &app->interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
}

static TILEFINCH_COLD_PATH void
psp_sample_media_stability(PspApp *app, uint64_t now_us)
{
    PspMediaSession *media = &app->browser->media;
    PspMediaStabilitySkew *skew = &app->interactive->media_stability_skew;
    bool steady_candidate =
        media->machine.state == PSP_MEDIA_SESSION_PLAYING
        && media->job_phase == PSP_MEDIA_JOB_NONE
        && !media->audio_hold.applied
        && !media->ui.ended;
    if (!steady_candidate) {
        skew->steady_since_us = 0;
    } else if (skew->steady_since_us == 0) {
        skew->steady_since_us = now_us;
    }
    if (app->interactive->media_stability_started_us == 0
        || now_us < app->interactive->media_stability_next_sample_us) return;

    unsigned free_bytes = sceKernelTotalFreeMemSize();
    unsigned largest_bytes = sceKernelMaxFreeMemSize();
    if (free_bytes < app->interactive->media_stability_min_free)
        app->interactive->media_stability_min_free = free_bytes;
    if (largest_bytes < app->interactive->media_stability_min_largest)
        app->interactive->media_stability_min_largest = largest_bytes;
    MediaBackendStats live = {0};
    uint64_t audio_us = 0;
    if (media_playback_backend_stats(media->playback, &live)
        && live.presented_video_us != 0
        && media_playback_audio_cursor_us(media->playback, &audio_us)) {
        /* Successful sceAudioOutputBlocking calls put blocks into the PSP's
           firmware queue; they do not mean those samples are audible yet.
           The session clock and teardown telemetry both use the elapsed,
           queue-capped audio cursor. Sample that same clock here so startup
           preroll and seek refill do not masquerade as hundreds of
           milliseconds of steady A/V skew. */
        uint64_t sample_us = audio_us > live.presented_video_us
            ? audio_us - live.presented_video_us
            : live.presented_video_us - audio_us;
        if (sample_us > skew->maximum_us) skew->maximum_us = sample_us;
        if (skew->steady_since_us != 0
            && now_us - skew->steady_since_us
                   >= PSP_MEDIA_STABILITY_STEADY_DELAY_US) {
            if (sample_us > skew->steady_maximum_us) {
                skew->steady_maximum_us = sample_us;
                skew->steady_peak_elapsed_us =
                    now_us - app->interactive->media_stability_started_us;
                skew->steady_peak_audio_us = audio_us;
                skew->steady_peak_video_us = live.presented_video_us;
                skew->steady_peak_no_frame_ms = media->no_frame_ms;
            }
            if (skew->steady_samples != UINT_MAX) skew->steady_samples++;
        }
    }
    app->interactive->media_stability_next_sample_us =
        now_us + UINT64_C(250000);
}

static __attribute__((noinline)) void psp_media_stability_schedule_seeks(
    PspApp *app, uint64_t elapsed_us, uint64_t duration_us)
{
    PspMediaSession *media = &app->browser->media;
    int permille = (int) app->process->config.validation_media_seek_permille;
    if (app->interactive->media_stability_started_us != 0 && permille != 0
            && !app->interactive->media_stability_forward_seek
            && elapsed_us >= UINT64_C(15000000)
            && media->playback != NULL
            && media->have_frame
            && media->job_phase == PSP_MEDIA_JOB_NONE) {
        uint64_t target_us = psp_media_stability_seek_target_us(
            duration_us, permille);
        if (psp_media_request_seek(media, target_us, false)) {
            psp_video_scanout_note_discontinuity();
            media->job_resume_playing = true;
            app->interactive->media_stability_forward_seek = true;
            app->interactive->media_stability_skew.steady_since_us = 0;
            printf(
                "tilefinch-media-stability: event=seek "
                "direction=forward target=%lluus\n",
                (unsigned long long) target_us);
        }
    }
    if (app->interactive->media_stability_started_us != 0 && permille != 0
            && !app->interactive->media_stability_backward_seek
            && elapsed_us >= UINT64_C(45000000)
            && media->playback != NULL
            && media->have_frame
            && media->job_phase == PSP_MEDIA_JOB_NONE) {
        uint64_t target_us = duration_us > UINT64_C(6000000)
            ? UINT64_C(3000000) : 0;
        if (psp_media_request_seek(media, target_us, false)) {
            psp_video_scanout_note_discontinuity();
            media->job_resume_playing = true;
            app->interactive->media_stability_backward_seek = true;
            app->interactive->media_stability_skew.steady_since_us = 0;
            printf(
                "tilefinch-media-stability: event=seek "
                "direction=backward target=%lluus\n",
                (unsigned long long) target_us);
        }
    }
}


/* Main-thread-only exclusive wall accounting. Closing in the caller covers
   every early return; nested JS/native/curl timings remain separate views.
   No URLs, DOM text or retained page pointers enter these records. */
static struct {
    uint64_t start, last, us[LOOP_PHASE_COUNT];
    /* The work ledger's exclusive kinds inside each phase; what a phase
       leaves unclaimed is browser bookkeeping. */
    uint64_t kinds[LOOP_PHASE_COUNT][WORK_LEDGER_KIND_COUNT];
    WorkLedgerTotals ledger;
    unsigned phase;
    bool active;
    /* The frame's wait was the page-task yield, not a vblank. */
    bool task_yield;
    bool fast_followup;
    bool timer_known, timer_frame;
    int64_t due_us;
} psp_loop_timing;

static const char *const psp_loop_phase_names[LOOP_PHASE_COUNT] = {
    "wait", "input", "action", "probe", "navigation", "runtime", "idle",
    "raster", "present", "images", "tail"
};

static void psp_loop_ready_sample(void);

void psp_loop_timing_begin(void)
{
    memset(&psp_loop_timing, 0, sizeof(psp_loop_timing));
    psp_loop_timing.active = psp_input_script_running()
        && !psp_validation_video_timing_window;
    work_ledger_set_enabled(psp_loop_timing.active);
    if (psp_loop_timing.active) {
        psp_loop_timing.start = psp_loop_timing.last =
            (uint64_t) sceKernelGetSystemTimeWide();
        work_ledger_snapshot(&psp_loop_timing.ledger);
        psp_loop_ready_sample();
    }
}

void psp_loop_timing_phase(unsigned phase)
{
    if (!psp_loop_timing.active) return;
    uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
    psp_loop_timing.us[psp_loop_timing.phase] += now - psp_loop_timing.last;
    psp_loop_timing.last = now;
    WorkLedgerTotals ledger;
    work_ledger_snapshot(&ledger);
    for (unsigned kind = 0; kind < WORK_LEDGER_KIND_COUNT; kind++)
        psp_loop_timing.kinds[psp_loop_timing.phase][kind] +=
            ledger.us[kind] - psp_loop_timing.ledger.us[kind];
    psp_loop_timing.ledger = ledger;
    psp_loop_timing.phase = phase;
}

static void psp_loop_work_print(void);

void psp_loop_timing_end(void)
{
    if (!psp_loop_timing.active) return;
    psp_loop_timing_phase(LOOP_TAIL);
    /* Video cadence has its own bounded presentation records. Formatting
       every owner turn adds CPU cost and fills the small RAM log, which then
       loses diagnostics. Count deliberately suppressed routine rows; retain
       all errors, checkpoints, media summaries and presentation measurements. */
    if (psp_validation_video_timing_window) {
        psp_validation_video_suppressed_loops++;
        psp_loop_timing.active = false;
        return;
    }
    /* Live USB log flushes can cost several milliseconds after publication
       and steal the next frame's deadline. Retain causal-window loop rows
       alongside their canvas sample; format them only in the final report. */
    if (psp_canvas_cadence_take_loop(
            psp_loop_timing.start, psp_loop_timing.last, psp_loop_timing.us,
            psp_loop_timing.due_us, psp_loop_timing.timer_known,
            psp_loop_timing.timer_frame, psp_loop_timing.fast_followup)) {
        psp_loop_timing.active = false;
        return;
    }
    printf("tilefinch-loop-timing: begin-us=%llu end-us=%llu "
           "wait=%llu input=%llu action=%llu probe=%llu navigation=%llu "
           "runtime=%llu idle=%llu raster=%llu present=%llu images=%llu tail=%llu\n",
           (unsigned long long) psp_loop_timing.start,
           (unsigned long long) psp_loop_timing.last,
           (unsigned long long) psp_loop_timing.us[LOOP_WAIT],
           (unsigned long long) psp_loop_timing.us[LOOP_INPUT],
           (unsigned long long) psp_loop_timing.us[LOOP_ACTION],
           (unsigned long long) psp_loop_timing.us[LOOP_PROBE],
           (unsigned long long) psp_loop_timing.us[LOOP_NAVIGATION],
           (unsigned long long) psp_loop_timing.us[LOOP_RUNTIME],
           (unsigned long long) psp_loop_timing.us[LOOP_IDLE],
           (unsigned long long) psp_loop_timing.us[LOOP_RASTER],
           (unsigned long long) psp_loop_timing.us[LOOP_PRESENT],
           (unsigned long long) psp_loop_timing.us[LOOP_IMAGES],
           (unsigned long long) psp_loop_timing.us[LOOP_TAIL]);
    psp_loop_work_print();
    psp_loop_timing.active = false;
}
/* The page's runnable work as the frame starts, before its wait: a wait
   with work already due is the loop's delay, not the page's. */
static PspBrowserResources *psp_loop_timing_browser;
static bool psp_loop_ready_valid;
static ScriptRunnableState psp_loop_ready;

void psp_loop_timing_bind(PspBrowserResources *browser)
{
    psp_loop_timing_browser = browser;
}

/* How the frame's wait went: the page-task yield instead of a vblank, and
   whether a fast follow-up skipped it. */
void psp_loop_timing_note_wait(bool task_yield, bool fast_followup)
{
    psp_loop_timing.task_yield = task_yield;
    psp_loop_timing.fast_followup = fast_followup;
}

static void psp_loop_ready_sample(void)
{
    memset(&psp_frame_runtime, 0, sizeof(psp_frame_runtime));
    psp_loop_ready_valid = psp_loop_timing_browser != NULL
        && psp_loop_timing_browser->engine != NULL
        && browser_engine_runnable_state(
               psp_loop_timing_browser->engine, &psp_loop_ready);
    if (psp_loop_ready_valid) {
        psp_loop_timing.timer_known = psp_loop_ready.timer_known;
        psp_loop_timing.timer_frame = psp_loop_ready.timer_frame_callback;
        psp_loop_timing.due_us = psp_loop_ready.timer_due_in_us;
    }
}

static int psp_loop_work_append(char *line, size_t capacity, int used,
                                const char *format, ...)
    __attribute__((format(__printf__, 4, 5)));
static int psp_loop_work_append(char *line, size_t capacity, int used,
                                const char *format, ...)
{
    if (used < 0 || (size_t) used >= capacity) return used;
    va_list arguments;
    va_start(arguments, format);
    int wrote = vsnprintf(line + used, capacity - (size_t) used, format,
                          arguments);
    va_end(arguments);
    return wrote < 0 ? used : used + wrote;
}

/* One line per frame, beside its tilefinch-loop-timing record (same
   begin-us): for each phase that saw any, the work ledger's exclusive
   script/native/layout/sleep microseconds (the phase's remainder is
   bookkeeping); the runnable state sampled before the frame's wait; and
   the frame's runtime advances broken down. Field reference:
   docs/engineering/INPUT_SCRIPT_HARNESS.md, "Where a frame's time goes". */
static void psp_loop_work_print(void)
{
    char line[1024];
    const char *step_kind = NULL;
    unsigned step = psp_input_script_step_now(&step_kind);
    int used = snprintf(line, sizeof(line),
                        "tilefinch-loop-work: begin-us=%llu step=%u:%s",
                        (unsigned long long) psp_loop_timing.start, step,
                        step_kind);
    for (unsigned phase = 0; phase < LOOP_PHASE_COUNT; phase++) {
        const uint64_t *kinds = psp_loop_timing.kinds[phase];
        if (kinds[WORK_LEDGER_SCRIPT] == 0 && kinds[WORK_LEDGER_NATIVE] == 0
            && kinds[WORK_LEDGER_LAYOUT] == 0 && kinds[WORK_LEDGER_SLEEP] == 0)
            continue;
        used = psp_loop_work_append(
            line, sizeof(line), used, " %s=%llu/%llu/%llu/%llu",
            psp_loop_phase_names[phase],
            (unsigned long long) kinds[WORK_LEDGER_SCRIPT],
            (unsigned long long) kinds[WORK_LEDGER_NATIVE],
            (unsigned long long) kinds[WORK_LEDGER_LAYOUT],
            (unsigned long long) kinds[WORK_LEDGER_SLEEP]);
    }
    if (psp_loop_ready_valid) {
        used = psp_loop_work_append(
            line, sizeof(line), used,
            " timers=%u jobs=%d cleanup=%d ready=%u flight=%u cont=%d",
            (unsigned) psp_loop_ready.timers,
            psp_loop_ready.jobs_pending ? 1 : 0,
            psp_loop_ready.cleanup_pending ? 1 : 0,
            (unsigned) psp_loop_ready.responses_ready,
            (unsigned) psp_loop_ready.requests_in_flight,
            psp_loop_ready.continuation_pending ? 1 : 0);
        if (psp_loop_ready.timer_known)
            used = psp_loop_work_append(
                line, sizeof(line), used, " due-us=%lld due-kind=%s",
                (long long) psp_loop_ready.timer_due_in_us,
                psp_loop_ready.timer_frame_callback ? "frame" : "task");
        if (psp_loop_ready.task_known)
            used = psp_loop_work_append(
                line, sizeof(line), used, " task-due-us=%lld",
                (long long) psp_loop_ready.task_due_in_us);
        else
            used = psp_loop_work_append(line, sizeof(line), used,
                                        " task-due-us=none");
    }
    if (psp_loop_timing.task_yield)
        used = psp_loop_work_append(line, sizeof(line), used,
                                    " task-yield=1");
    if (psp_frame_runtime.deferred_render != 0
        || psp_frame_runtime.deferred_other != 0)
        used = psp_loop_work_append(
            line, sizeof(line), used, " deferred=%u/%u",
            psp_frame_runtime.deferred_render,
            psp_frame_runtime.deferred_other);
    if (psp_frame_runtime.advances != 0)
        used = psp_loop_work_append(
            line, sizeof(line), used,
            " advances=%u advance=%llu realm=%llu prelude=%llu "
            "timer-prepare=%llu tasks=%u/%llu microtasks=%llu network=%llu "
            "refresh=%llu child=%llu setup=%llu messages=%llu "
            "relayout=%llu damage=%llu late=%llu/%llu/%u "
            "frame-callbacks=%u/%llu",
            psp_frame_runtime.advances,
            (unsigned long long) psp_frame_runtime.elapsed_us,
            (unsigned long long) psp_frame_runtime.realm_us,
            (unsigned long long) psp_frame_runtime.prelude_us,
            (unsigned long long) psp_frame_runtime.timer_prepare_us,
            (unsigned) psp_frame_runtime.timers,
            (unsigned long long) psp_frame_runtime.timer_us,
            (unsigned long long) psp_frame_runtime.jobs_us,
            (unsigned long long) psp_frame_runtime.network_us,
            (unsigned long long) psp_frame_runtime.refresh_us,
            (unsigned long long) psp_frame_runtime.child_us,
            (unsigned long long) psp_frame_runtime.setup_us,
            (unsigned long long) psp_frame_runtime.messages_us,
            (unsigned long long) psp_frame_runtime.relayout_us,
            (unsigned long long) psp_frame_runtime.damage_us,
            (unsigned long long) psp_frame_runtime.late_us,
            (unsigned long long) psp_frame_runtime.late_max_us,
            (unsigned) psp_frame_runtime.late_frames,
            (unsigned) psp_frame_runtime.frame_callbacks,
            (unsigned long long) psp_frame_runtime.frame_callback_late_us);
    if (used > 0) printf("%s\n", line);
}

/* A PSP cannot schedule its own wake from a user-mode suspend.
   This validation-only cycle therefore publishes both callbacks
   together: lifecycle_poll still orders QUIESCE before RECOVER,
   exercising the exact media teardown/reopen, display rearm and
   network revalidation paths without claiming to test firmware
   sleep itself. Do not wait for an idle codec slot; split-state
   teardown is part of the contract this probe exists to cover. */
TILEFINCH_OUT_OF_LINE void psp_loop_inject_media_lifecycle(
    PspLoop *loop)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    if (process->config.validation_media_lifecycle_auto != 0
        && interactive->media_stability_active
        && !loop->media_stability_lifecycle_injected
        && interactive->media_stability_started_us != 0
        && (uint64_t) sceKernelGetSystemTimeWide()
               - interactive->media_stability_started_us
               >= UINT64_C(30000000)
        && psp_lifecycle_state(&psp_lifecycle)
               == PSP_LIFECYCLE_RUNNING) {
        loop->media_stability_lifecycle_injected = true;
        printf(
            "tilefinch-media-stability: "
            "event=lifecycle-injected kind=logical "
            "clock=%lluus job=%d\n",
            (unsigned long long) browser->media.clock_us,
            (int) browser->media.job_phase);
        psp_lifecycle_notify_suspend(&psp_lifecycle);
        psp_lifecycle_notify_resume(&psp_lifecycle);
    }
}


/* This frame's input from the running script, or the person who just
   took over from it. False when the script has finished: the loop
   then leaves through its exit plan. */
TILEFINCH_HOT_BOUNDARY bool psp_loop_scripted_input(
    PspLoop *loop, PspUiInput *input, bool supervisor_page_input)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    /* The script is the sole controller source until a person
       touches the real pad. A physical press is an explicit handoff:
       stop every automatic driver and deliver that same press now.
       Merging sources would let a later scripted Cross immediately
       undo a manual Pause. */
    if (input->held != 0u) {
        psp_input_script_interrupt_by_user();
        psp_boot_config_disable_automation(&process->config);
        interactive->media_stability_active = false;
        interactive->media_stability_auto_exit = false;
        process->presentation.ui.validation_media_test_phase = 0;
        input->pressed = input->held;
    } else if (supervisor_page_input) {
        /* The scripted edge was already advanced and captured by
           the callback supervisor. Replay it before asking the
           script for another command, preserving one receiver and
           one browser action per authored edge. */
        memset(input, 0, sizeof(*input));
        input->analog_x = 128;
        input->analog_y = 128;
    } else {
        uint32_t scripted_download_id = 0;
        bool scripted_background_idle =
            interactive->screenshot.writer.status
                != SCREENSHOT_PNG_PENDING
            && !psp_update_session_active(&browser->update_session)
            && !offline_download_manager_active(
                   &browser->offline_store.download,
                   &scripted_download_id);
        bool supervisor_owns_script =
            psp_navigation_cooperate_supervised();
        bool scripted_media_ready = browser->media.ui.visible
            && !browser->media.ui.resolving
            && !browser->media.ui.failed
            && browser->media.playback != NULL;
        bool scripted_surface_ready =
            scripted_background_idle
            && !process->presentation.ui.loading
            && !loop->render_job_pending
            && (!browser->media.ui.visible || scripted_media_ready)
            && !browser_engine_navigation_pending(browser->engine);
        bool scripted_page_ready = scripted_surface_ready
            && process->presentation.ui.base_screen == PSP_UI_SCREEN_PAGE
            && browser_engine_page_loaded(browser->engine);
        bool scripted_ready = scripted_surface_ready
            && !navigation_background_resources_pending(
                   engine_views->navigation);
        if (supervisor_owns_script) {
            psp_log_set_stage("frame-supervised-script");
            memset(input, 0, sizeof(*input));
            input->analog_x = 128;
            input->analog_y = 128;
        } else if (!psp_input_script_frame(
                       input, scripted_ready, scripted_page_ready)) {
            psp_exit_plan_request(
                &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
            return false;
        }
    }
    return true;
}

/* The automatic power test: starts it once the page is idle, and
   while it runs keeps ordinary controls off the measured page
   (Circle aborts). True when the test changed what is shown. */
TILEFINCH_OUT_OF_LINE bool psp_loop_power_test_input(
    PspLoop *loop, PspUiInput *input, uint64_t now_us,
    bool *abort_requested)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
    bool power_auto_started_visual = false;
    bool power_auto_start_ready =
        interactive->power_auto_boot_pending
        && !process->presentation.ui.loading && !loop->render_job_pending
        && !browser->media.ui.visible
        && !browser_engine_navigation_pending(browser->engine)
        && !navigation_background_resources_pending(engine_views->navigation)
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        && !psp_network_lifecycle_warming(network_lifecycle)
#endif
        ;
    if (power_auto_start_ready) {
        PspClockWorkerSnapshot initial_power = {0};
        if (process->clock_live)
            psp_clock_worker_snapshot(
                &process->clock_worker, &initial_power);
        interactive->power_auto_boot_pending = false;
        bool power_auto_started = psp_power_auto_start(
            &interactive->power_auto, &interactive->power_test, now_us,
            process->clock_live ? &initial_power : NULL,
            &process->presentation.ui, process->config.validation_power_test_auto != 0,
            process->config.validation_power_test_auto != 0
                ? "boot-config" : "menu");
        if (!power_auto_started)
            process->presentation.ui.validation_power_test_phase = 0;
        power_auto_started_visual = true;
    }
    bool power_auto_abort_requested = false;
    if (interactive->power_auto_boot_pending || interactive->power_auto.active) {
        power_auto_abort_requested =
            (input->pressed & PSP_UI_BUTTON_CANCEL) != 0;
        if (power_auto_abort_requested
            && interactive->power_auto_boot_pending
            && !interactive->power_auto.active) {
            interactive->power_auto_boot_pending = false;
            process->presentation.ui.validation_power_test_phase = 0;
            psp_ui_show_status(
                &process->presentation.ui, "POWER TEST STOPPED", 240);
            power_auto_started_visual = true;
            power_auto_abort_requested = false;
        }
        /* This benchmark isolates the idle-reading state. Ignore
           ordinary controls so every segment presents the same page
           and follows the same browser path; Circle remains an
           immediate escape hatch. */
        input->pressed = 0;
        input->held = 0;
        input->analog_x = 128;
        input->analog_y = 128;
    }
    *abort_requested = power_auto_abort_requested;
    return power_auto_started_visual;
}

/* Advances the power test; true when an automatic segment ended. */
TILEFINCH_HOT_BOUNDARY bool psp_loop_power_test_tick(
    PspLoop *loop, uint64_t now_us, unsigned ui_elapsed_ms,
    PspClockWorkerSnapshot *power_snapshot, bool power_auto_abort_requested)
{
    PspProcessResources *const process = loop->app.process;
    PspInteractiveState *const interactive = loop->app.interactive;
    psp_power_test_tick(
        &interactive->power_test, now_us, ui_elapsed_ms,
        process->clock_live ? power_snapshot : NULL);
    bool power_auto_visual_changed = false;
    if (interactive->power_auto.active
        && (power_auto_abort_requested
            || now_us - interactive->power_test.started_us
                   >= PSP_POWER_AUTO_SEGMENT_US)) {
        PspPowerTestResult segment_result =
            psp_power_test_finish(
                &interactive->power_test, now_us,
                process->clock_live ? power_snapshot : NULL,
                power_auto_abort_requested
                    ? "user-abort" : "automatic-segment");
        psp_power_auto_accumulate(
            &interactive->power_auto, &segment_result);
        printf(
            "tilefinch-power-auto: event=segment-finish "
            "segment=%u/4 phase=%s\n",
            interactive->power_auto.segment + 1u,
            psp_power_test_phase_name(segment_result.phase));
        if (power_auto_abort_requested) {
            interactive->power_auto.active = false;
            process->presentation.ui.validation_power_test_phase = 0;
            psp_power_auto_log_summary(
                &interactive->power_auto, now_us, "user-abort");
            psp_ui_show_status(
                &process->presentation.ui, "POWER TEST STOPPED", 240);
        } else if (++interactive->power_auto.segment
                   < PSP_POWER_AUTO_SEGMENTS) {
            psp_power_auto_begin_segment(
                &interactive->power_auto, &interactive->power_test, now_us,
                process->clock_live ? power_snapshot : NULL,
                &process->presentation.ui);
        } else {
            interactive->power_auto.active = false;
            process->presentation.ui.validation_power_test_phase = 0;
            psp_power_auto_log_summary(
                &interactive->power_auto, now_us, "complete");
            psp_ui_show_status(
                &process->presentation.ui,
                "2 MIN POWER TEST COMPLETE - EXIT TO SAVE LOG",
                900);
            if (process->config.validation_power_test_auto != 0) {
                psp_log_checkpoint(
                    "automatic-power-test-complete");
                psp_exit_plan_request(
                    &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
            }
        }
        power_auto_visual_changed = true;
    }
    return power_auto_visual_changed;
}

TILEFINCH_OUT_OF_LINE void psp_loop_count_validation_tick(PspLoop *loop)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    if (process->config.interactive_validation_ticks > 0
        && ++loop->interactive_validation_ticks
               >= process->config.interactive_validation_ticks) {
        if (process->config.validation_media_play != 0
            && !interactive->validation_media_play_confirmed) {
            MediaBackendStats backend_stats = {0};
            bool have_stats = media_playback_backend_stats(
                browser->media.playback, &backend_stats);
            printf("tilefinch-media-validation: "
                   "playback-not-confirmed injected=%d "
                   "clock=%lluus have-stats=%d video-frames=%zu "
                   "audio-packets=%zu native-error=%d\n",
                   interactive->validation_media_play_injected ? 1 : 0,
                   (unsigned long long) browser->media.clock_us,
                   have_stats ? 1 : 0,
                   backend_stats.decoded_video_frames,
                   backend_stats.submitted_audio_packets,
                   backend_stats.last_native_error);
        }
        printf("tilefinch-validation: interactive-loops=%ld "
               "background-pending=%d render-pending=%d\n",
               loop->interactive_validation_ticks,
               navigation_background_resources_pending(engine_views->navigation)
                   ? 1 : 0,
               loop->render_job_pending ? 1 : 0);
        psp_log_checkpoint("interactive-validation-complete");
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
    }
}

/* Validation-only media drivers: the play-after-open knob, its playback
   confirmation, and the long-run stability test (loops, seeks, end). */
TILEFINCH_OUT_OF_LINE void psp_loop_media_validation(
    PspLoop *loop, bool *media_visual_changed)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    bool stability_needs_play =
        interactive->media_stability_active
        && process->config.input_script[0] == '\0'
        && browser->media.playback != NULL && browser->media.have_frame
        && !browser->media.pause_boundary_pending && !browser->media.ui.playing
        && browser->media.job_phase == PSP_MEDIA_JOB_NONE
        && !browser->media.ui.ended;
    /* Autoplay for the validation measurement engages the instant the
       open settles -- playback live, no job in flight -- rather than
       waiting for a first frame. The resume-open leg this path takes
       decodes nothing on its own, so a have-frame gate here left
       playing=0 forever and the panel never rendered, which is useless
       to measure. PLAY_PAUSE clears pause-after-next-frame and starts
       the decoder against the connection media-open already warmed, so
       there is no idle gap and no handshake inside the measured window.
       The injected flag and the not-already-playing gate keep this to a
       single press even if a script also sends play. */
    bool validation_autoplay =
        process->config.validation_media_play != 0
        && !interactive->validation_media_play_injected
        && browser->media.playback != NULL
        && browser->media.job_phase == PSP_MEDIA_JOB_NONE
        && !browser->media.ui.playing
        && !browser->media.ui.ended;
    bool natural_autoplay_ready =
        ((process->config.validation_media_play != 0
          && !interactive->validation_media_play_injected)
         || (interactive->media_stability_active
             && interactive->media_stability_started_us == 0))
        && browser->media.playback != NULL
        && browser->media.job_phase == PSP_MEDIA_JOB_NONE
        && browser->media.ui.playing
        && !browser->media.ui.ended;
    if (validation_autoplay || stability_needs_play
        || natural_autoplay_ready) {
        bool injected_play = validation_autoplay || stability_needs_play;
        if (injected_play) {
            PspUiMediaIntent play_intent = {
                .action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE
            };
            psp_media_execute_intent(&browser->media, play_intent);
        }
        if (process->config.validation_media_play != 0)
            interactive->validation_media_play_injected = true;
        if (interactive->media_stability_active
            && interactive->media_stability_started_us == 0) {
            interactive->media_stability_started_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            interactive->media_stability_next_sample_us =
                interactive->media_stability_started_us + UINT64_C(250000);
            interactive->media_stability_start_capacity =
                scePowerGetBatteryRemainCapacity();
            interactive->media_stability_start_percent =
                scePowerGetBatteryLifePercent();
            unsigned starting_free = sceKernelTotalFreeMemSize();
            unsigned starting_largest = sceKernelMaxFreeMemSize();
            interactive->media_stability_min_free = starting_free;
            interactive->media_stability_min_largest = starting_largest;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_power_log_battery(
                "media-start", PSP_POWER_TEST_FIXED_HIGH, 0);
#endif
            printf(
                "tilefinch-media-stability: event=started "
                "requested=%up actual=%dx%d itag=%d "
                "system-free=%u system-largest=%u\n",
                (unsigned) browser->media.requested_quality,
                browser->media.stream.width, browser->media.stream.height,
                browser->media.stream.itag,
                starting_free, starting_largest);
        }
        *media_visual_changed = true;
        printf("tilefinch-media-validation: play-source=%s "
               "after-open=1\n",
               injected_play ? "validation-input" : "route-autoplay");
    }
    if (process->config.validation_media_play != 0
        && !interactive->media_stability_active
        && interactive->validation_media_play_injected
        && !interactive->validation_media_play_confirmed
        && browser->media.clock_us >= UINT64_C(500000)) {
        MediaBackendStats backend_stats = {0};
        if (media_playback_backend_stats(
                browser->media.playback, &backend_stats)
            && backend_stats.decoded_video_frames >= 2
            && backend_stats.submitted_audio_packets != 0) {
            interactive->validation_media_play_confirmed = true;
            printf("tilefinch-media-validation: playback-confirmed "
                   "clock=%lluus video-frames=%zu "
                   "audio-packets=%zu video-us=%lluus "
                   "audio-origin-us=%lluus audio-blocks=%lu "
                   "audio-rate=%u native-error=%d\n",
                   (unsigned long long) browser->media.clock_us,
                   backend_stats.decoded_video_frames,
                   backend_stats.submitted_audio_packets,
                   (unsigned long long)
                       backend_stats.presented_video_us,
                   (unsigned long long)
                       backend_stats.audio_origin_us,
                   backend_stats.audio_output_blocks,
                   backend_stats.audio_sample_rate,
                   backend_stats.last_native_error);
            psp_exit_plan_request(
                &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
        }
    }
    if (interactive->media_stability_active) {
        if (browser->media.ui.failed
            && !psp_media_open_work_pending(&browser->media)
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE) {
            printf(
                "tilefinch-media-stability: event=failed "
                "reason=\"%.160s\"\n",
                browser->media.ui.status);
            interactive->media_stability_active = false;
            process->presentation.ui.validation_media_test_phase = 0;
            if (interactive->media_stability_auto_exit)
                psp_exit_plan_request(
                    &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
        }
    }
    if (interactive->media_stability_active) {
        uint64_t stability_now_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        uint64_t stability_elapsed_us =
            interactive->media_stability_started_us == 0 ? 0
                : stability_now_us - interactive->media_stability_started_us;
        psp_sample_media_stability(app, stability_now_us);
        uint64_t duration_us =
            psp_media_duration_us(&browser->media);
        psp_media_stability_schedule_seeks(
            app, stability_elapsed_us, duration_us);
        /* Loop the way a viewer restarts a finished video: Play at the end,
           which rewinds to 0:00 and keeps playing. A clip of 30 s or more
           makes that rewind a full backend reopen, whose resume state is
           taken when the seek is requested -- so it must be asked for as
           playing then, not patched into job_resume_playing afterwards. */
        /* A playlist owns its end policy. The single-video soak must not
           restart its incumbent before automatic-next can observe the end. */
        if (browser->media.playlist == NULL
            && browser->media.ui.ended && browser->media.playback != NULL
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE
            && stability_elapsed_us
                   < loop->media_stability_target_us) {
            psp_media_execute_intent(&browser->media, (PspUiMediaIntent) {
                .action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE
            });
            interactive->media_stability_skew.steady_since_us = 0;
            interactive->media_stability_loops++;
            printf(
                "tilefinch-media-stability: event=loop count=%u\n",
                interactive->media_stability_loops);
        }
        if (interactive->media_stability_started_us != 0
            && stability_elapsed_us
                   >= loop->media_stability_target_us) {
            psp_finish_media_stability(
                app, stability_elapsed_us);
        }
    }
}

/* The power test and clock probes (formerly in psp_app_runtime.c). */
static bool psp_set_policy_clock(
    void *context, unsigned cpu_mhz, unsigned bus_mhz,
    uint64_t *transition_us)
{
    (void) context;
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    int result = scePowerSetClockFrequency(
        (int) cpu_mhz, (int) cpu_mhz, (int) bus_mhz);
    uint64_t finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (transition_us != NULL)
        *transition_us = finished_us - started_us;
    return result >= 0
        && scePowerGetCpuClockFrequencyInt() == (int) cpu_mhz
        && scePowerGetBusClockFrequencyInt() == (int) bus_mhz;
}

static bool psp_set_split_clock(
    unsigned cpu_mhz, unsigned bus_mhz, uint64_t *transition_us)
{
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    int first_result;
    int second_result;
    if (cpu_mhz >= (unsigned) scePowerGetCpuClockFrequencyInt()) {
        first_result = scePowerSetBusClockFrequency((int) bus_mhz);
        second_result = scePowerSetCpuClockFrequency((int) cpu_mhz);
    } else {
        first_result = scePowerSetCpuClockFrequency((int) cpu_mhz);
        second_result = scePowerSetBusClockFrequency((int) bus_mhz);
    }
    uint64_t finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (transition_us != NULL)
        *transition_us = finished_us - started_us;
    return first_result >= 0 && second_result >= 0
        && scePowerGetCpuClockFrequencyInt() == (int) cpu_mhz
        && scePowerGetBusClockFrequencyInt() == (int) bus_mhz;
}

static uint64_t psp_clock_work_probe(uint32_t *checksum)
{
    uint32_t value = UINT32_C(0x5a17c9e3);
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    for (unsigned at = 0; at < 400000u; at++)
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
    uint64_t finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (checksum != NULL) *checksum = value;
    return finished_us - started_us;
}

void psp_clock_validation_probe(void)
{
    uint64_t low_transition_us = 0;
    uint64_t high_transition_us = 0;
    uint64_t split_low_transition_us = 0;
    uint64_t split_high_transition_us = 0;
    uint32_t low_checksum = 0;
    uint32_t high_checksum = 0;
    bool low_ok = psp_set_policy_clock(
        NULL, PSP_POWER_POLICY_IDLE_CPU_MHZ,
        PSP_POWER_POLICY_IDLE_BUS_MHZ, &low_transition_us);
    uint64_t low_work_us = psp_clock_work_probe(&low_checksum);
    bool high_ok = psp_set_policy_clock(
        NULL, PSP_POWER_POLICY_HIGH_CPU_MHZ,
        PSP_POWER_POLICY_HIGH_BUS_MHZ, &high_transition_us);
    uint64_t high_work_us = psp_clock_work_probe(&high_checksum);
    bool split_low_ok = psp_set_split_clock(
        PSP_POWER_POLICY_IDLE_CPU_MHZ,
        PSP_POWER_POLICY_IDLE_BUS_MHZ, &split_low_transition_us);
    bool split_high_ok = psp_set_split_clock(
        PSP_POWER_POLICY_HIGH_CPU_MHZ,
        PSP_POWER_POLICY_HIGH_BUS_MHZ, &split_high_transition_us);
    printf(
        "tilefinch-power-probe: low-ok=%d low-transition=%lluus "
        "low-work=%lluus high-ok=%d high-transition=%lluus "
        "high-work=%lluus split-low-ok=%d split-low=%lluus "
        "split-high-ok=%d split-high=%lluus checksum-match=%d\n",
        low_ok ? 1 : 0, (unsigned long long) low_transition_us,
        (unsigned long long) low_work_us, high_ok ? 1 : 0,
        (unsigned long long) high_transition_us,
        (unsigned long long) high_work_us, split_low_ok ? 1 : 0,
        (unsigned long long) split_low_transition_us,
        split_high_ok ? 1 : 0,
        (unsigned long long) split_high_transition_us,
        low_checksum == high_checksum ? 1 : 0);
}

const char *psp_power_test_phase_name(PspPowerTestPhase phase)
{
    switch (phase) {
        case PSP_POWER_TEST_ADAPTIVE: return "adaptive";
        case PSP_POWER_TEST_FIXED_HIGH: return "fixed-333";
        case PSP_POWER_TEST_OFF:
        default: return "off";
    }
}

void psp_power_log_battery(
    const char *event, PspPowerTestPhase phase, uint64_t elapsed_ms)
{
    printf(
        "tilefinch-battery: event=%s phase=%s elapsed=%llums "
        "present=%d ac=%d charging=%d low=%d capacity=%d "
        "full=%d percent=%d life-min=%d voltage-mv=%d temp-c=%d "
        "cpu=%d bus=%d\n",
        event, psp_power_test_phase_name(phase),
        (unsigned long long) elapsed_ms,
        scePowerIsBatteryExist(), scePowerIsPowerOnline(),
        scePowerIsBatteryCharging(), scePowerIsLowBattery(),
        scePowerGetBatteryRemainCapacity(),
        scePowerGetBatteryFullCapacity(),
        scePowerGetBatteryLifePercent(),
        scePowerGetBatteryLifeTime(),
        scePowerGetBatteryVolt(), scePowerGetBatteryTemp(),
        scePowerGetCpuClockFrequencyInt(),
        scePowerGetBusClockFrequencyInt());
}

static void psp_power_test_begin(
    PspPowerTest *test, PspPowerTestPhase phase, uint64_t now_us,
    const PspClockWorkerSnapshot *worker)
{
    if (test == NULL || phase == PSP_POWER_TEST_OFF) return;
    memset(test, 0, sizeof(*test));
    test->phase = phase;
    test->started_us = now_us;
    test->next_sample_us = now_us + UINT64_C(60000000);
    test->starting_completions =
        worker == NULL ? 0 : worker->completions;
    test->starting_failures =
        worker == NULL ? 0 : worker->failures;
    test->starting_capacity = scePowerGetBatteryRemainCapacity();
    test->starting_percent = scePowerGetBatteryLifePercent();
    psp_power_log_battery("start", phase, 0);
    printf(
        "tilefinch-power-test: event=start phase=%s "
        "workload=automatic-idle-reading\n",
        psp_power_test_phase_name(phase));
}

void psp_power_test_tick(
    PspPowerTest *test, uint64_t now_us, unsigned elapsed_ms,
    const PspClockWorkerSnapshot *worker)
{
    if (test == NULL || test->phase == PSP_POWER_TEST_OFF) return;
    if (worker != NULL && worker->transitioning)
        test->transition_ms += elapsed_ms;
    else if (worker != NULL && worker->applied_idle)
        test->idle_ms += elapsed_ms;
    else
        test->high_ms += elapsed_ms;
    if (now_us >= test->next_sample_us) {
        uint64_t elapsed = (now_us - test->started_us) / 1000u;
        psp_power_log_battery("sample", test->phase, elapsed);
        printf(
            "tilefinch-power-test: event=sample phase=%s "
            "high=%llums idle=%llums transition=%llums\n",
            psp_power_test_phase_name(test->phase),
            (unsigned long long) test->high_ms,
            (unsigned long long) test->idle_ms,
            (unsigned long long) test->transition_ms);
        test->next_sample_us = now_us + UINT64_C(60000000);
    }
}

PspPowerTestResult psp_power_test_finish(
    PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, const char *reason)
{
    PspPowerTestResult result = {
        .capacity_delta = INT_MIN,
        .percent_delta = INT_MIN
    };
    if (test == NULL || test->phase == PSP_POWER_TEST_OFF) return result;
    uint64_t elapsed_ms = (now_us - test->started_us) / 1000u;
    int ending_capacity = scePowerGetBatteryRemainCapacity();
    int ending_percent = scePowerGetBatteryLifePercent();
    unsigned completions = worker == NULL ? 0
        : worker->completions - test->starting_completions;
    unsigned failures = worker == NULL ? 0
        : worker->failures - test->starting_failures;
    result.phase = test->phase;
    result.elapsed_ms = elapsed_ms;
    result.high_ms = test->high_ms;
    result.idle_ms = test->idle_ms;
    result.transition_ms = test->transition_ms;
    if (ending_capacity >= 0 && test->starting_capacity >= 0)
        result.capacity_delta =
            ending_capacity - test->starting_capacity;
    if (ending_percent >= 0 && test->starting_percent >= 0
        && ending_capacity >= 0 && test->starting_capacity >= 0)
        result.percent_delta = ending_percent - test->starting_percent;
    psp_power_log_battery("finish", test->phase, elapsed_ms);
    printf(
        "tilefinch-power-test: event=finish phase=%s reason=%s "
        "elapsed=%llums high=%llums idle=%llums transition=%llums "
        "clock-completions=%u clock-failures=%u "
        "capacity-start=%d capacity-end=%d capacity-delta=%d "
        "percent-start=%d percent-end=%d percent-delta=%d\n",
        psp_power_test_phase_name(test->phase),
        reason == NULL ? "user" : reason,
        (unsigned long long) elapsed_ms,
        (unsigned long long) test->high_ms,
        (unsigned long long) test->idle_ms,
        (unsigned long long) test->transition_ms,
        completions, failures, test->starting_capacity, ending_capacity,
        result.capacity_delta,
        test->starting_percent, ending_percent,
        result.percent_delta);
    test->phase = PSP_POWER_TEST_OFF;
    return result;
}

static PspPowerTestPhase psp_power_auto_phase(unsigned segment)
{
    /* Counterbalance clock mode against battery settling and temperature. */
    return segment == 0u || segment == 3u
        ? PSP_POWER_TEST_ADAPTIVE : PSP_POWER_TEST_FIXED_HIGH;
}

void psp_power_auto_begin_segment(
    PspPowerAutoTest *automatic, PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, PspUiState *ui)
{
    if (automatic == NULL || test == NULL || ui == NULL
        || automatic->segment >= PSP_POWER_AUTO_SEGMENTS) return;
    PspPowerTestPhase phase =
        psp_power_auto_phase(automatic->segment);
    psp_power_test_begin(test, phase, now_us, worker);
    ui->validation_power_test_phase = 1;
    char status[80];
    snprintf(
        status, sizeof(status), "POWER TEST %u/4: %s - PLEASE WAIT",
        automatic->segment + 1u,
        phase == PSP_POWER_TEST_ADAPTIVE ? "ADAPTIVE" : "FIXED 333");
    psp_ui_show_status(ui, status, 900);
    printf(
        "tilefinch-power-auto: event=segment-start segment=%u/4 "
        "phase=%s duration=30000ms workload=idle-reading\n",
        automatic->segment + 1u, psp_power_test_phase_name(phase));
}

void psp_power_auto_accumulate(
    PspPowerAutoTest *automatic, const PspPowerTestResult *result)
{
    if (automatic == NULL || result == NULL) return;
    bool adaptive = result->phase == PSP_POWER_TEST_ADAPTIVE;
    uint64_t *elapsed = adaptive
        ? &automatic->adaptive_elapsed_ms : &automatic->fixed_elapsed_ms;
    uint64_t *high = adaptive
        ? &automatic->adaptive_high_ms : &automatic->fixed_high_ms;
    uint64_t *idle = adaptive
        ? &automatic->adaptive_idle_ms : &automatic->fixed_idle_ms;
    uint64_t *transition = adaptive
        ? &automatic->adaptive_transition_ms
        : &automatic->fixed_transition_ms;
    *elapsed += result->elapsed_ms;
    *high += result->high_ms;
    *idle += result->idle_ms;
    *transition += result->transition_ms;
    if (result->capacity_delta != INT_MIN) {
        int *delta = adaptive
            ? &automatic->adaptive_capacity_delta
            : &automatic->fixed_capacity_delta;
        bool *valid = adaptive
            ? &automatic->adaptive_capacity_valid
            : &automatic->fixed_capacity_valid;
        *delta += result->capacity_delta;
        *valid = true;
    }
    if (result->percent_delta != INT_MIN) {
        int *delta = adaptive
            ? &automatic->adaptive_percent_delta
            : &automatic->fixed_percent_delta;
        bool *valid = adaptive
            ? &automatic->adaptive_percent_valid
            : &automatic->fixed_percent_valid;
        *delta += result->percent_delta;
        *valid = true;
    }
}

void psp_power_auto_log_summary(
    const PspPowerAutoTest *automatic, uint64_t now_us,
    const char *reason)
{
    if (automatic == NULL) return;
    printf(
        "tilefinch-power-auto: event=summary reason=%s elapsed=%llums "
        "adaptive-ms=%llums fixed-ms=%llums "
        "adaptive-high=%llums adaptive-idle=%llums "
        "adaptive-transition=%llums "
        "fixed-high=%llums fixed-idle=%llums fixed-transition=%llums "
        "adaptive-capacity-delta=%d adaptive-capacity-valid=%d "
        "fixed-capacity-delta=%d fixed-capacity-valid=%d "
        "adaptive-percent-delta=%d adaptive-percent-valid=%d "
        "fixed-percent-delta=%d fixed-percent-valid=%d\n",
        reason == NULL ? "complete" : reason,
        (unsigned long long) ((now_us - automatic->started_us) / 1000u),
        (unsigned long long) automatic->adaptive_elapsed_ms,
        (unsigned long long) automatic->fixed_elapsed_ms,
        (unsigned long long) automatic->adaptive_high_ms,
        (unsigned long long) automatic->adaptive_idle_ms,
        (unsigned long long) automatic->adaptive_transition_ms,
        (unsigned long long) automatic->fixed_high_ms,
        (unsigned long long) automatic->fixed_idle_ms,
        (unsigned long long) automatic->fixed_transition_ms,
        automatic->adaptive_capacity_delta,
        automatic->adaptive_capacity_valid ? 1 : 0,
        automatic->fixed_capacity_delta,
        automatic->fixed_capacity_valid ? 1 : 0,
        automatic->adaptive_percent_delta,
        automatic->adaptive_percent_valid ? 1 : 0,
        automatic->fixed_percent_delta,
        automatic->fixed_percent_valid ? 1 : 0);
}

bool psp_power_auto_start(
    PspPowerAutoTest *automatic, PspPowerTest *test, uint64_t now_us,
    const PspClockWorkerSnapshot *worker, PspUiState *ui,
    bool allow_unmeasured, const char *trigger)
{
    if (automatic == NULL || test == NULL || ui == NULL) return false;
    int battery_present = scePowerIsBatteryExist();
    int ac_online = scePowerIsPowerOnline();
    if (!allow_unmeasured
        && (battery_present <= 0 || ac_online != 0)) {
        printf(
            "tilefinch-power-test: event=refused battery=%d ac=%d "
            "reason=%s\n",
            battery_present, ac_online,
            ac_online > 0 ? "disconnect-ac" : "battery-unavailable");
        psp_ui_show_status(
            ui,
            ac_online > 0
                ? "UNPLUG AC POWER, THEN START TEST"
                : "BATTERY TEST UNAVAILABLE",
            600);
        return false;
    }
    memset(automatic, 0, sizeof(*automatic));
    automatic->active = true;
    automatic->started_us = now_us;
    psp_power_auto_begin_segment(
        automatic, test, now_us, worker, ui);
    printf(
        "tilefinch-power-auto: event=start trigger=%s "
        "duration=120000ms segments=4 "
        "schedule=adaptive,fixed-333,fixed-333,adaptive "
        "workload=idle-reading input=circle-aborts "
        "measurements=%s\n",
        trigger == NULL ? "menu" : trigger,
        allow_unmeasured ? "optional" : "required");
    return true;
}

/* psp_app_run_interactive's start: the loop timing's page, and the boot.cfg
   input script. */
void psp_validation_interactive_begin(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, const char *argv0)
{
    psp_loop_timing_bind(browser);
    bool input_script_loaded = psp_input_script_begin(
        &process->install_paths, argv0, process->config.input_script);
    if (process->config.input_script[0] != '\0' && !input_script_loaded) {
        /* A named scenario is an automation contract, not an optional
           controller aid. Falling back to the real pad makes the PPSSPP
           runner wait for its full timeout and can accidentally exercise
           unrelated user input. Leave through normal cleanup instead;
           the harness outcome makes the run fail immediately. */
        printf("tilefinch-input-script: outcome=load-failed "
               "script=%s\n", process->config.input_script);
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
    }
}

/* psp_app_run_interactive's end: the input script's summary, and a power
   test still running. */
void psp_validation_interactive_end(
    PspProcessResources *process, PspInteractiveState *interactive)
{
    psp_input_script_summary();
    if (interactive->power_test.phase != PSP_POWER_TEST_OFF) {
        PspClockWorkerSnapshot current_power = {0};
        if (process->clock_live)
            psp_clock_worker_snapshot(
                &process->clock_worker, &current_power);
        uint64_t now_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        PspPowerTestResult result = psp_power_test_finish(
            &interactive->power_test,
            now_us,
            process->clock_live ? &current_power : NULL,
            "browser-exit");
        if (interactive->power_auto.active) {
            psp_power_auto_accumulate(&interactive->power_auto, &result);
            interactive->power_auto.active = false;
            psp_power_auto_log_summary(
                &interactive->power_auto, now_us, "browser-exit");
        }
        process->presentation.ui.validation_power_test_phase = 0;
    }
}

/* psp_loop_frame's input source while an input script runs. False ends the
   frame: the script is not ready for this one. */
bool psp_validation_scripted_frame(
    PspLoop *loop, PspUiInput *input, bool supervisor_page_input,
    PspAppFrameState *frame, unsigned *ui_elapsed_ms)
{
    /*
     * The scripted source replaces the sampled pad outright, and only
     * steps forward on a frame the browser could actually have taken
     * the press on. That readiness gate is what makes one script walk
     * the identical sequence under PPSSPP and on a PSP-3000, where
     * the same navigation costs a different number of frames.
     */
    if (psp_input_script_running()) {
        if (!psp_loop_scripted_input(loop, input, supervisor_page_input))
            return false;
        if (input->pressed != 0 && frame->replayed_press_us == 0)
            frame->replayed_press_us =
                psp_input_script_request_time(frame->ui_sample_us);
        /* The loop's own millisecond consumers -- the reader-mode
           hold, the power policy -- read this rather than the input,
           and a frame that took 800 ms under a loaded host would
           otherwise turn a scripted tap into a long press.
           The media presentation clock is deliberately NOT one of
           them: see media_elapsed_ms in psp_loop_frame. */
        *ui_elapsed_ms = 16u;
    }
    return true;
}

TILEFINCH_COLD_PATH void psp_report_frame_pumps(
    const PspInteractiveState *interactive)
{
    if (interactive == NULL) return;
    const FramePumpFrame *pumps = &interactive->frame_pumps;
    printf("tilefinch-frame-pumps: order-violations=%lu "
           "wiring-violations=%lu\n",
           (unsigned long) pumps->order_violations,
           (unsigned long) pumps->wiring_violations);
    for (unsigned id = 0; id < FRAME_PUMP_COUNT; id++) {
        printf("tilefinch-frame-pump: name=%s admissions=%lu slices=%lu "
               "yields=%lu\n",
               frame_pump_policy((FramePumpId) id)->name,
               (unsigned long) pumps->admissions[id],
               (unsigned long) pumps->slices[id],
               (unsigned long) pumps->yields[id]);
    }
#if defined(TILEFINCH_OWNER_CHECKS)
    /* Every worker thread exists only on this target, so this is where the
       single-owner ledger rule meets real concurrency. */
    printf("tilefinch-owner-checks: budget-violations=%lu\n",
           budget_owner_violations());
#endif
}
