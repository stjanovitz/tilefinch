/* Presentation measurement for the validation EBOOT.
 *
 * The presentation cadence record (compose/publish intervals, cursor and
 * video scanout cadence), present-phase timing, first-picture and
 * track-menu feedback phase clocks, and input-to-presentation focus
 * latency. psp_app_runtime.c's present paths reach it through the probe
 * hooks in psp_app_validation.h.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_validation.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/psp_load_experience.h"

#include "tilefinch/psp_input_cadence.h"
typedef struct {
    uint64_t compose_total_us;
    uint64_t compose_max_us;
    uint64_t start_previous_us;
    uint64_t start_gap_max_us;
    uint32_t presentations;
    uint32_t native_presentations;
    uint32_t compose_over_16ms;
    uint32_t compose_over_33ms;
    uint64_t track_menu_compose_total_us;
    uint64_t track_menu_compose_max_us;
    uint64_t track_menu_scanout_interval_total_us;
    uint64_t track_menu_scanout_interval_max_us;
    uint32_t track_menu_presentations;
    uint32_t track_menu_scanout_intervals;
    uint64_t track_menu_blit_total_us;
    uint64_t track_menu_blit_max_us;
    uint32_t track_menu_blits;
    uint32_t track_menu_blit_fallbacks;
    uint64_t track_menu_open_transition_us;
    uint64_t track_menu_close_transition_us;
    uint32_t track_menu_open_transitions;
    uint32_t track_menu_close_transitions;
    bool track_menu_state_known;
    bool track_menu_was_open;
    uint64_t cursor_sample_pending_us;
    PspInputCadence cursor_cadence;
    uint64_t cursor_compose_total_us, cursor_compose_max_us;
    uint64_t cursor_publish_total_us, cursor_publish_max_us;
    uint32_t cursor_work_samples;
    uint64_t cursor_latency_total_us;
    uint64_t cursor_latency_max_us;
    uint32_t cursor_samples;
    uint32_t cursor_presentations;
    uint32_t cursor_coalesced_samples;
    uint32_t cursor_over_33ms;
    /*
     * Wall-clock spacing between distinct decoded pictures that actually
     * reached scanout. Re-presenting the same picture for UI chrome does not
     * count: those swaps say nothing about playback cadence. Keep the raw
     * samples in bounded counters and print them only at the validation
     * boundary, because host0 output can stall the browser thread long enough
     * to manufacture the hitch this is meant to diagnose.
     */
    uint64_t video_scanout_interval_total_us;
    uint64_t video_scanout_interval_max_us;
    uint64_t video_scanout_previous_us;
    uint64_t video_scanout_identity;
    uint64_t video_scanout_epoch;
    uint32_t video_scanout_generation;
    int video_scanout_slot;
    uint32_t video_scanout_intervals;
    uint32_t video_scanout_buckets[8];
    bool video_scanout_frame_valid;
} PspPresentationCadence;

static PspPresentationCadence psp_presentation_cadence;
static bool psp_video_scanout_measuring = true;
typedef struct {
    uint64_t stage_us;
    size_t stage_frames;
    uint64_t draw_us;
    size_t draw_frames;
    uint32_t ge_copies;
} PspVideoDrawSnapshot;
static PspVideoDrawSnapshot psp_video_measure_begin;
static PspVideoDrawSnapshot psp_video_measure_end;
static bool psp_video_measure_valid;
static PspPresentPhaseTiming psp_present_last_timing;
static bool psp_present_cpu_measurement;
static void (*psp_present_pre_publish_hook)(uint32_t sequence);

void psp_present_validation_pre_publish_hook(void (*hook)(uint32_t sequence))
{
    psp_present_pre_publish_hook = hook;
}

void psp_present_validation_measure_cpu(bool enabled)
{
    psp_present_cpu_measurement = enabled;
}

static bool psp_present_thread_cpu(uint64_t *run_us)
{
    SceKernelThreadRunStatus status = {.size = sizeof(status)};
    if (sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status) < 0)
        return false;
    *run_us = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
    return true;
}

static atomic_uint psp_media_menu_feedback_time;
static atomic_uint psp_media_menu_feedback_key;

void psp_media_menu_feedback_input(const PspUiMediaState *media,
    bool was_open, unsigned old_tab, uint32_t started_us)
{
    const PspUiMediaPresentation *p = media == NULL ? NULL : media->presentation;
    if (p == NULL || (was_open == p->track_menu_open
        && (!p->track_menu_open || old_tab == p->track_menu_tab))) return;
    atomic_store_explicit(&psp_media_menu_feedback_key,
        p->track_menu_open ? p->track_menu_tab + 1u : 0u, memory_order_relaxed);
    atomic_store_explicit(&psp_media_menu_feedback_time,
        started_us, memory_order_release);
}


static PspMenuPhaseClock psp_menu_phase_clock(void)
{
    PspMenuPhaseClock clock = {0};
    clock.before = sceKernelGetSystemTimeWide();
    clock.valid = psp_present_thread_cpu(&clock.cpu);
    clock.after = sceKernelGetSystemTimeWide();
    return clock;
}

/* First-picture attribution is bounded, numeric-only, and flushed after the
   measured window. Include surface entry: timing only the overlay missed the
   initial format switch. The presentation fence serializes both painters. */
static struct {
    uint64_t generation;
    unsigned count, loading_count, playing_count;
    PspMenuPhaseClock clocks[8][5];
    bool published[8], resolving[8];
} psp_player_start;
static PspMenuPhaseClock *psp_player_start_clocks;

static void psp_player_start_report(void)
{
    static const char *const names[] = {"surface", "video", "overlay", "publish"};
    for (unsigned row = 0; row < psp_player_start.count; row++) {
        const PspMenuPhaseClock *clocks = psp_player_start.clocks[row];
        uint64_t query_us = 0;
        for (unsigned at = 0; at < 5; at++)
            query_us += clocks[at].after - clocks[at].before;
        for (unsigned at = 0; at < 4; at++) {
            bool valid = clocks[at].valid && clocks[at + 1].valid
                && clocks[at + 1].cpu >= clocks[at].cpu
                && clocks[at + 1].before >= clocks[at].after;
            printf("tilefinch-player-start: frame=%u resolving=%u phase=%s published=%u wall-low=%lluus wall-high=%lluus cpu=%lluus valid=%u clock-query=%lluus\n",
                row, psp_player_start.resolving[row], names[at], psp_player_start.published[row],
                (unsigned long long) (valid ? clocks[at + 1].before - clocks[at].after : 0),
                (unsigned long long) (valid ? clocks[at + 1].after - clocks[at].before : 0),
                (unsigned long long) (valid ? clocks[at + 1].cpu - clocks[at].cpu : 0),
                valid, (unsigned long long) query_us);
        }
    }
}

static void psp_menu_phase_report(unsigned key, uint32_t input_us,
    const PspMenuPhaseClock clocks[4])
{
    static const char *const phases[] = {"video", "overlay", "publish"};
    uint64_t query_us = 0;
    for (unsigned at = 0; at < 4; at++) query_us += clocks[at].after - clocks[at].before;
    printf("tilefinch-menu-feedback: open=%u tab=%u elapsed=%uus clock-query=%lluus\n",
        key != 0u, key == 0u ? 0u : key - 1u,
        (unsigned) ((uint32_t) clocks[3].after - input_us), (unsigned long long) query_us);
    for (unsigned at = 0; at < 3; at++) {
        bool valid = clocks[at].valid && clocks[at + 1].valid
            && clocks[at + 1].cpu >= clocks[at].cpu;
        printf("tilefinch-menu-phase: phase=%s wall-low=%lluus wall-high=%lluus cpu=%lluus valid=%u\n",
            phases[at],
            (unsigned long long) (clocks[at + 1].before - clocks[at].after),
            (unsigned long long) (clocks[at + 1].after - clocks[at].before),
            (unsigned long long) (valid ? clocks[at + 1].cpu - clocks[at].cpu : 0), valid);
    }
}

bool psp_present_validation_last_timing(PspPresentPhaseTiming *timing)
{
    if (timing == NULL || psp_present_last_timing.sequence == 0) return false;
    *timing = psp_present_last_timing;
    return true;
}

void psp_report_presentation_cadence(const char *phase)
{
    psp_player_start_report();
    const PspPresentationCadence *metrics = &psp_presentation_cadence;
    printf(
        "tilefinch-ui-cadence: phase=%s presents=%lu native=%lu "
        "compose-total=%lluus compose-max=%lluus compose-average=%lluus "
        "over-16ms=%lu over-33ms=%lu start-gap-max=%lluus "
        "track-menu-presents=%lu track-menu-compose-average=%lluus "
        "track-menu-compose-max=%lluus track-menu-blits=%lu/%lu "
        "track-menu-blit-average=%lluus track-menu-blit-max=%lluus "
        "cursor-samples=%lu cursor-presents=%lu cursor-coalesced=%lu "
        "cursor-average=%lluus cursor-max=%lluus cursor-over-33ms=%lu\n",
        phase == NULL ? "unknown" : phase, metrics->presentations,
        metrics->native_presentations,
        (unsigned long long) metrics->compose_total_us,
        (unsigned long long) metrics->compose_max_us,
        (unsigned long long) (metrics->presentations == 0 ? 0
            : metrics->compose_total_us / metrics->presentations),
        metrics->compose_over_16ms, metrics->compose_over_33ms,
        (unsigned long long) metrics->start_gap_max_us,
        (unsigned long) metrics->track_menu_presentations,
        (unsigned long long) (metrics->track_menu_presentations == 0 ? 0
            : metrics->track_menu_compose_total_us
                / metrics->track_menu_presentations),
        (unsigned long long) metrics->track_menu_compose_max_us,
        (unsigned long) metrics->track_menu_blits,
        (unsigned long) metrics->track_menu_blit_fallbacks,
        (unsigned long long) (metrics->track_menu_blits == 0 ? 0
            : metrics->track_menu_blit_total_us
                / metrics->track_menu_blits),
        (unsigned long long) metrics->track_menu_blit_max_us,
        metrics->cursor_samples, metrics->cursor_presentations,
        metrics->cursor_coalesced_samples,
        (unsigned long long) (metrics->cursor_presentations == 0 ? 0
            : metrics->cursor_latency_total_us
                / metrics->cursor_presentations),
        (unsigned long long) metrics->cursor_latency_max_us,
        metrics->cursor_over_33ms);
    printf(
        "tilefinch-cursor-cadence: phase=%s intervals=%lu "
        "average=%lluus max=%lluus segments=%lu\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->cursor_cadence.intervals,
        (unsigned long long) (metrics->cursor_cadence.intervals == 0 ? 0
            : metrics->cursor_cadence.total_us
                / metrics->cursor_cadence.intervals),
        (unsigned long long) metrics->cursor_cadence.max_us,
        (unsigned long) metrics->cursor_cadence.segments);
    printf("tilefinch-cursor-work: phase=%s samples=%lu "
           "compose-average=%lluus compose-max=%lluus "
           "publish-average=%lluus publish-max=%lluus\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->cursor_work_samples,
        (unsigned long long) (metrics->cursor_work_samples == 0 ? 0
            : metrics->cursor_compose_total_us / metrics->cursor_work_samples),
        (unsigned long long) metrics->cursor_compose_max_us,
        (unsigned long long) (metrics->cursor_work_samples == 0 ? 0
            : metrics->cursor_publish_total_us / metrics->cursor_work_samples),
        (unsigned long long) metrics->cursor_publish_max_us);
    printf(
        "tilefinch-video-scanout: phase=%s intervals=%lu "
        "average=%lluus max=%lluus "
        "buckets-le20-le28-le36-le45-le67-le100-le250-gt250="
        "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->video_scanout_intervals,
        (unsigned long long) (metrics->video_scanout_intervals == 0 ? 0
            : metrics->video_scanout_interval_total_us
                / metrics->video_scanout_intervals),
        (unsigned long long) metrics->video_scanout_interval_max_us,
        (unsigned long) metrics->video_scanout_buckets[0],
        (unsigned long) metrics->video_scanout_buckets[1],
        (unsigned long) metrics->video_scanout_buckets[2],
        (unsigned long) metrics->video_scanout_buckets[3],
        (unsigned long) metrics->video_scanout_buckets[4],
        (unsigned long) metrics->video_scanout_buckets[5],
        (unsigned long) metrics->video_scanout_buckets[6],
        (unsigned long) metrics->video_scanout_buckets[7]);
    printf(
        "tilefinch-track-menu-cadence: phase=%s intervals=%lu "
        "average=%lluus max=%lluus\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->track_menu_scanout_intervals,
        (unsigned long long) (metrics->track_menu_scanout_intervals == 0 ? 0
            : metrics->track_menu_scanout_interval_total_us
                / metrics->track_menu_scanout_intervals),
        (unsigned long long) metrics->track_menu_scanout_interval_max_us);
    printf(
        "tilefinch-track-menu-transition: phase=%s open=%lu/%lluus "
        "close=%lu/%lluus\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->track_menu_open_transitions,
        (unsigned long long) metrics->track_menu_open_transition_us,
        (unsigned long) metrics->track_menu_close_transitions,
        (unsigned long long) metrics->track_menu_close_transition_us);
}

void psp_cursor_latency_sample(uint64_t sampled_us)
{
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    psp_input_cadence_observe(&metrics->cursor_cadence, true, sampled_us);
    metrics->cursor_samples++;
    if (metrics->cursor_sample_pending_us == 0) {
        metrics->cursor_sample_pending_us = sampled_us;
    } else {
        /* Retain the oldest unpresented sample: that is the input age the
           user actually experiences when work coalesces multiple moves. */
        metrics->cursor_coalesced_samples++;
    }
}

void psp_cursor_latency_idle(void)
{
    psp_input_cadence_observe(
        &psp_presentation_cadence.cursor_cadence, false, 0);
}

void psp_video_scanout_note_discontinuity(void)
{
    /* A seek/reopen gap is loading latency, not a display interval. Keep the
       accumulated cadence but let the first picture after the discontinuity
       establish a new origin instead of folding seconds of decoder work into
       the source-frame average and maximum. */
    psp_presentation_cadence.video_scanout_previous_us = 0;
    psp_presentation_cadence.video_scanout_frame_valid = false;
}

void psp_video_scanout_measurement(bool enabled)
{
    psp_video_scanout_measuring = enabled;
    /* Input marks run outside the presenter's scoped active-media pointer.
       Snapshot counters while presenting; never retain that session pointer. */
    if (!enabled && psp_video_measure_valid
        && psp_video_measure_end.stage_frames >= psp_video_measure_begin.stage_frames
        && psp_video_measure_end.draw_frames >= psp_video_measure_begin.draw_frames
        && psp_video_measure_end.stage_us >= psp_video_measure_begin.stage_us
        && psp_video_measure_end.draw_us >= psp_video_measure_begin.draw_us) {
        size_t stages = psp_video_measure_end.stage_frames - psp_video_measure_begin.stage_frames;
        size_t draws = psp_video_measure_end.draw_frames - psp_video_measure_begin.draw_frames;
        printf("tilefinch-video-draw-window: stages=%zu stage-average=%lluus "
               "draws=%zu draw-average=%lluus ge-copies=%lu\n", stages,
               (unsigned long long) (stages == 0 ? 0
                   : (psp_video_measure_end.stage_us - psp_video_measure_begin.stage_us) / stages),
               draws, (unsigned long long) (draws == 0 ? 0
                   : (psp_video_measure_end.draw_us - psp_video_measure_begin.draw_us) / draws),
               (unsigned long) (psp_video_measure_end.ge_copies - psp_video_measure_begin.ge_copies));
    }
    if (!enabled) return;
    psp_video_measure_valid = false;
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    metrics->video_scanout_interval_total_us = 0;
    metrics->video_scanout_interval_max_us = 0;
    metrics->video_scanout_intervals = 0;
    memset(metrics->video_scanout_buckets, 0, sizeof(metrics->video_scanout_buckets));
    psp_video_scanout_note_discontinuity();
}

/*
 * Both presenters report here.
 *
 * The 32-bit video path returns from psp_present_internal before the 16-bit
 * compositor is reached, and when it was first written that took its frames
 * out of this accounting entirely: a device cycle reported presents=270 for a
 * session that had actually presented 140 video frames on top of them, and
 * the most expensive frames in the run were the ones the cadence line could
 * not see.
 */
static void psp_cadence_composed(uint64_t started_us, bool native_surface)
{
    uint64_t compose_us =
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    metrics->presentations++;
    if (native_surface) metrics->native_presentations++;
    metrics->compose_total_us += compose_us;
    if (compose_us > metrics->compose_max_us)
        metrics->compose_max_us = compose_us;
    if (compose_us > UINT64_C(16000)) metrics->compose_over_16ms++;
    if (compose_us > UINT64_C(33000)) metrics->compose_over_33ms++;
    bool track_menu_open = psp_active_media != NULL
        && psp_active_media->ui.presentation != NULL
        && psp_active_media->ui.presentation->track_menu_open;
    if (metrics->track_menu_state_known
        && track_menu_open != metrics->track_menu_was_open) {
        if (track_menu_open) {
            metrics->track_menu_open_transitions++;
            metrics->track_menu_open_transition_us = compose_us;
        } else {
            metrics->track_menu_close_transitions++;
            metrics->track_menu_close_transition_us = compose_us;
        }
    }
    metrics->track_menu_state_known = true;
    metrics->track_menu_was_open = track_menu_open;
    if (track_menu_open) {
        metrics->track_menu_presentations++;
        metrics->track_menu_compose_total_us += compose_us;
        if (compose_us > metrics->track_menu_compose_max_us)
            metrics->track_menu_compose_max_us = compose_us;
    }
    if (metrics->start_previous_us != 0) {
        uint64_t gap = started_us - metrics->start_previous_us;
        if (gap > metrics->start_gap_max_us) metrics->start_gap_max_us = gap;
    }
    metrics->start_previous_us = started_us;
}

static void psp_cadence_published(bool published)
{
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    if (!published || metrics->cursor_sample_pending_us == 0) return;
    uint64_t accepted_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t latency_us = accepted_us >= metrics->cursor_sample_pending_us
        ? accepted_us - metrics->cursor_sample_pending_us : 0;
    metrics->cursor_presentations++;
    metrics->cursor_latency_total_us += latency_us;
    if (latency_us > metrics->cursor_latency_max_us)
        metrics->cursor_latency_max_us = latency_us;
    if (latency_us > UINT64_C(33000)) metrics->cursor_over_33ms++;
    metrics->cursor_sample_pending_us = 0;
}

static size_t psp_video_scanout_interval_bucket(uint64_t interval_us)
{
    static const uint32_t upper_us[] = {
        20000u, 28000u, 36000u, 45000u, 67000u, 100000u, 250000u
    };
    for (size_t at = 0; at < sizeof(upper_us) / sizeof(upper_us[0]); at++) {
        if (interval_us <= upper_us[at]) return at;
    }
    return sizeof(upper_us) / sizeof(upper_us[0]);
}

static void psp_cadence_video_published(
    bool published, const MediaVideoFrame *frame)
{
    if (!published || frame == NULL || !psp_video_scanout_measuring) return;
    if (psp_active_media != NULL) {
        psp_video_measure_end = (PspVideoDrawSnapshot) {
            psp_active_media->present_stage_total_us,
            psp_active_media->present_stage_frames,
            psp_active_media->present_scale_total_us,
            psp_active_media->present_scale_frames,
            psp_media_present_ge_stage_rows_copies()
        };
        if (!psp_video_measure_valid) {
            psp_video_measure_begin = psp_video_measure_end;
            psp_video_measure_valid = true;
        }
    }
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    bool same_picture = metrics->video_scanout_frame_valid
        && metrics->video_scanout_identity == frame->identity
        && metrics->video_scanout_epoch == frame->epoch
        && metrics->video_scanout_generation == frame->generation
        && metrics->video_scanout_slot == frame->slot;
    if (same_picture) return;

    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (metrics->video_scanout_previous_us != 0
        && now_us >= metrics->video_scanout_previous_us) {
        uint64_t interval_us = now_us - metrics->video_scanout_previous_us;
        metrics->video_scanout_intervals++;
        metrics->video_scanout_interval_total_us += interval_us;
        if (interval_us > metrics->video_scanout_interval_max_us)
            metrics->video_scanout_interval_max_us = interval_us;
        metrics->video_scanout_buckets[
            psp_video_scanout_interval_bucket(interval_us)]++;
        if (psp_active_media != NULL
            && psp_active_media->ui.presentation != NULL
            && psp_active_media->ui.presentation->track_menu_open) {
            metrics->track_menu_scanout_intervals++;
            metrics->track_menu_scanout_interval_total_us += interval_us;
            if (interval_us > metrics->track_menu_scanout_interval_max_us)
                metrics->track_menu_scanout_interval_max_us = interval_us;
        }
    }
    metrics->video_scanout_previous_us = now_us;
    metrics->video_scanout_identity = frame->identity;
    metrics->video_scanout_epoch = frame->epoch;
    metrics->video_scanout_generation = frame->generation;
    metrics->video_scanout_slot = frame->slot;
    metrics->video_scanout_frame_valid = true;
}

/* One outstanding input measurement, no DOM retention. The engine is the
   process-owned facade; a navigation generation change abandons the sample.
   In particular a supervisor publishing the old framebuffer is not success. */
static struct {
    BrowserEngine *engine;
    uint64_t generation, started_us, shell_serial;
    /* First present of any frame for this input, placeholder or complete. */
    uint64_t first_visible_us;
    size_t rendered_frames;
    int action;
    /* Page presses a later one replaced before their frame completed (a
       queued burst replays one per frame): each is reported, timed from
       its own press, when the one that replaced it completes. */
    struct {
        uint64_t started_us, first_visible_us;
    } superseded[8];
    uint8_t superseded_count;
} psp_focus_feedback;

static bool psp_focus_feedback_page_action(int action)
{
    return action == (int) PSP_UI_ACTION_PAGE_UP
        || action == (int) PSP_UI_ACTION_PAGE_DOWN;
}

/*
 * Per-focus-move frame identity (tilefinch-focus-frame), for device A/Bs of
 * focus paths that must not change what the reader sees. Each focus move
 * reports twice: phase=complete when its focus-feedback line completes, and
 * phase=settled from the last page frame on the panel just before the next
 * press or script mark (whatever idle completion ran in between).
 *
 * The hashes are FNV-1a over what scanout reads: the published VRAM buffer
 * through its uncached alias, after psp_ui_composite, so the native focus
 * ring and the compositor-drawn authored outline are both included.
 *   screen  all 480x272 pixels (chrome included)
 *   page    the rows below/above the opaque chrome bars
 *   outside page rows minus the focus band (the focus rect grown by
 *           PSP_FOCUS_FRAME_BAND px on every side, the same band in every
 *           build, wide enough for the native ring and a few px of outline)
 *   band    the focus band alone
 *   engine  the engine framebuffer the page rows were copied from
 *   engine-outside  the engine framebuffer's page rows minus the band:
 *           when outside differs and engine-outside does not, the
 *           difference is in what psp_ui_composite drew over the page
 * overlay=loading:progress:busy:toast-frames:cursor and scroll=y/max are
 * the UI state that composite was drawn with (loading strip, toast,
 * cursor, scrollbar thumb).
 * ring=1 says the browser drew its native ring (psp_ui draw_focus);
 * authored=1 says the controller resolved an authored focus outline, which
 * the engine compositor paints instead. relayouts= is the session's
 * cumulative incremental relayout count: its step says whether the move
 * relayouted.
 */
#define PSP_FOCUS_FRAME_BAND 8

static struct {
    const uint16_t *vram;
    const uint16_t *frame;
    int focus_x, focus_y, focus_width, focus_height;
    unsigned opaque_top, opaque_bottom;
    bool has_focus, valid;
    /* The overlay state psp_ui_composite drew this frame with. */
    bool loading, busy, cursor;
    int progress, scroll_y, maximum_scroll_y;
    unsigned toast;
    uint32_t presents;
} psp_focus_frame_last;
static uint32_t psp_focus_frame_moves;
static bool psp_focus_frame_settle_pending;

static void psp_focus_frame_note_present(
    const uint16_t *frame, const PspUiState *ui, const uint16_t *vram)
{
    if (ui == NULL || vram == NULL || ui->screen != PSP_UI_SCREEN_PAGE) {
        psp_focus_frame_last.valid = false;
        return;
    }
    psp_focus_frame_last.vram = vram;
    psp_focus_frame_last.frame = frame;
    psp_focus_frame_last.has_focus = ui->has_focus;
    psp_focus_frame_last.focus_x = ui->focus_x;
    psp_focus_frame_last.focus_y = ui->focus_y;
    psp_focus_frame_last.focus_width = ui->focus_width;
    psp_focus_frame_last.focus_height = ui->focus_height;
    psp_ui_opaque_chrome_rows(ui, &psp_focus_frame_last.opaque_top,
                              &psp_focus_frame_last.opaque_bottom);
    psp_focus_frame_last.loading = ui->loading;
    psp_focus_frame_last.busy = ui->page_activation_busy;
    psp_focus_frame_last.cursor = ui->cursor_visible;
    psp_focus_frame_last.progress = ui->progress_per_mille;
    psp_focus_frame_last.scroll_y = ui->scroll_y;
    psp_focus_frame_last.maximum_scroll_y = ui->maximum_scroll_y;
    psp_focus_frame_last.toast = ui->toast_frames;
    psp_focus_frame_last.valid = true;
    psp_focus_frame_last.presents++;
}

static void psp_focus_frame_report(const BrowserEngine *engine, const char *phase)
{
    if (!psp_focus_frame_last.valid || engine == NULL) {
        printf("tilefinch-focus-frame: move=%u phase=%s valid=0\n",
               (unsigned) psp_focus_frame_moves, phase);
        return;
    }
    uint64_t started_us = sceKernelGetSystemTimeWide();
    const uint16_t *vram = (const uint16_t *)
        ((uintptr_t) psp_focus_frame_last.vram | (uintptr_t) 0x40000000u);
    int band_left = psp_focus_frame_last.focus_x - PSP_FOCUS_FRAME_BAND;
    int band_top = psp_focus_frame_last.focus_y - PSP_FOCUS_FRAME_BAND;
    int band_right = psp_focus_frame_last.focus_x
        + psp_focus_frame_last.focus_width + PSP_FOCUS_FRAME_BAND;
    int band_bottom = psp_focus_frame_last.focus_y
        + psp_focus_frame_last.focus_height + PSP_FOCUS_FRAME_BAND;
    int page_top = (int) psp_focus_frame_last.opaque_top;
    int page_end = PSP_SCREEN_HEIGHT - (int) psp_focus_frame_last.opaque_bottom;
    uint64_t screen = 1469598103934665603ull, page = screen;
    uint64_t outside = screen, band = screen, engine_hash = screen;
    uint64_t engine_outside = screen;
    for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
        const uint16_t *row = vram + (size_t) y * PSP_VRAM_STRIDE;
        bool page_row = y >= page_top && y < page_end;
        bool band_row = y >= band_top && y < band_bottom;
        for (int x = 0; x < PSP_SCREEN_WIDTH; x++) {
            uint16_t pixel = row[x];
            screen = (screen ^ pixel) * 1099511628211ull;
            if (!page_row) continue;
            page = (page ^ pixel) * 1099511628211ull;
            if (band_row && x >= band_left && x < band_right)
                band = (band ^ pixel) * 1099511628211ull;
            else
                outside = (outside ^ pixel) * 1099511628211ull;
        }
    }
    BrowserFrameView view = {0};
    if (browser_engine_frame_view(engine, &view) && view.pixels != NULL
        && view.width == PSP_SCREEN_WIDTH && view.height == PSP_SCREEN_HEIGHT
        && view.stride >= view.width) {
        for (int y = 0; y < view.height; y++) {
            const uint16_t *row = view.pixels + (size_t) y * (size_t) view.stride;
            bool page_row = y >= page_top && y < page_end;
            bool band_row = y >= band_top && y < band_bottom;
            for (int x = 0; x < view.width; x++) {
                engine_hash = (engine_hash ^ row[x]) * 1099511628211ull;
                if (page_row && !(band_row && x >= band_left && x < band_right))
                    engine_outside =
                        (engine_outside ^ row[x]) * 1099511628211ull;
            }
        }
    }
    /* Facade queries only: the copied view and the validation counters. */
    static BrowserViewSnapshot view_state;
    bool authored = browser_engine_view_snapshot(engine, &view_state)
        && view_state.has_focus && view_state.focus_has_authored_outline;
    BrowserValidationCounters counters;
    size_t relayouts = browser_engine_validation_counters(engine, &counters)
        ? counters.incremental_relayouts : 0u;
    printf("tilefinch-focus-frame: move=%u phase=%s valid=1 "
           "screen=%016llx page=%016llx outside=%016llx band=%016llx "
           "engine=%016llx ring=%u authored=%u rect=%d,%d,%d,%d "
           "chrome=%u/%u relayouts=%zu presents=%u "
           "engine-outside=%016llx overlay=%u:%d:%u:%u:%u scroll=%d/%d "
           "hash-us=%llu\n",
           (unsigned) psp_focus_frame_moves, phase,
           (unsigned long long) screen, (unsigned long long) page,
           (unsigned long long) outside, (unsigned long long) band,
           (unsigned long long) engine_hash,
           psp_focus_frame_last.has_focus ? 1u : 0u, authored ? 1u : 0u,
           psp_focus_frame_last.focus_x, psp_focus_frame_last.focus_y,
           psp_focus_frame_last.focus_width, psp_focus_frame_last.focus_height,
           psp_focus_frame_last.opaque_top, psp_focus_frame_last.opaque_bottom,
           relayouts, (unsigned) psp_focus_frame_last.presents,
           (unsigned long long) engine_outside,
           psp_focus_frame_last.loading ? 1u : 0u,
           psp_focus_frame_last.progress,
           psp_focus_frame_last.busy ? 1u : 0u,
           psp_focus_frame_last.toast,
           psp_focus_frame_last.cursor ? 1u : 0u,
           psp_focus_frame_last.scroll_y,
           psp_focus_frame_last.maximum_scroll_y,
           (unsigned long long) (sceKernelGetSystemTimeWide() - started_us));
    psp_focus_frame_last.presents = 0;
}

/* The settled frame of the previous focus move, if one is pending: called
   before the next press is dispatched and at every input-script mark. */
void psp_focus_frame_settle(const BrowserEngine *engine)
{
    if (!psp_focus_frame_settle_pending) return;
    psp_focus_frame_settle_pending = false;
    psp_focus_frame_report(engine, "settled");
}

void psp_focus_feedback_begin(BrowserEngine *engine, int action, uint64_t started_us)
{
    psp_focus_frame_settle(engine);
    if (engine != NULL && !psp_focus_feedback_page_action(action)) {
        psp_focus_frame_moves++;
        psp_focus_frame_settle_pending = true;
    }
    if (engine == NULL) {
        psp_focus_feedback.engine = NULL;
        psp_focus_feedback.superseded_count = 0;
        return;
    }
    BrowserValidationCounters counters;
    bool counted = browser_engine_validation_counters(engine, &counters);
    const TileCache *render = browser_engine_render_metrics_view(engine);
    if (!counted || render == NULL) return;
    if (psp_focus_feedback.engine != NULL) {
        printf("tilefinch-focus-feedback-incomplete: action=%d reason=superseded\n",
               psp_focus_feedback.action);
        if (psp_focus_feedback_page_action(psp_focus_feedback.action)
            && psp_focus_feedback_page_action(action)
            && psp_focus_feedback.superseded_count
                   < sizeof(psp_focus_feedback.superseded)
                         / sizeof(psp_focus_feedback.superseded[0])) {
            psp_focus_feedback.superseded[
                psp_focus_feedback.superseded_count].started_us =
                    psp_focus_feedback.started_us;
            psp_focus_feedback.superseded[
                psp_focus_feedback.superseded_count++].first_visible_us =
                    psp_focus_feedback.first_visible_us;
        }
    }
    if (!psp_focus_feedback_page_action(action))
        psp_focus_feedback.superseded_count = 0;
    psp_focus_feedback.engine = engine;
    psp_focus_feedback.generation = counters.generation;
    psp_focus_feedback.started_us = started_us;
    psp_focus_feedback.first_visible_us = 0;
    psp_focus_feedback.rendered_frames = render->frames_rendered;
    psp_focus_feedback.shell_serial = browser_engine_render_shell_serial(engine);
    psp_focus_feedback.action = action;
}

static void psp_focus_feedback_published(const uint16_t *frame, const PspUiState *ui)
{
    BrowserEngine *engine = psp_focus_feedback.engine;
    if (engine == NULL) return;
    BrowserValidationCounters counters;
    if (!browser_engine_validation_counters(engine, &counters)
        || !counters.loaded
        || counters.generation != psp_focus_feedback.generation) {
        printf("tilefinch-focus-feedback-incomplete: action=%d reason=navigation\n",
               psp_focus_feedback.action);
        psp_focus_feedback.engine = NULL;
        psp_focus_feedback.superseded_count = 0;
        return;
    }
    const TileCache *render = browser_engine_render_metrics_view(engine);
    if (psp_focus_feedback.first_visible_us == 0 && ui != NULL
        && ui->screen == PSP_UI_SCREEN_PAGE && render != NULL
        && render->frames_rendered > psp_focus_feedback.rendered_frames
        && frame == browser_engine_framebuffer(engine, NULL)) {
        psp_focus_feedback.first_visible_us = sceKernelGetSystemTimeWide();
    }
    if (ui == NULL || ui->screen != PSP_UI_SCREEN_PAGE || render == NULL
        || render->frames_rendered == 0
        || (browser_engine_render_shell_serial(engine) == psp_focus_feedback.shell_serial
            && render->frames_rendered <= psp_focus_feedback.rendered_frames)
        || browser_engine_render_frame_pending(engine)
        || frame != browser_engine_framebuffer(engine, NULL)) return;
    if (psp_focus_feedback_page_action(psp_focus_feedback.action)) {
        /* Time from the press to a complete frame at the new position. */
        /* Cumulative render-cache counters; consecutive lines give each
           press's hits (paint-ahead served) and raster misses. */
        uint64_t complete_us = sceKernelGetSystemTimeWide();
        uint64_t visible_us = psp_focus_feedback.first_visible_us != 0
            ? psp_focus_feedback.first_visible_us : complete_us;
        printf("tilefinch-scroll-feedback: action=%d elapsed=%lluus "
               "first-visible=%lluus y=%d/%d "
               "tile-us=%lluus tiles=%zu/%zu hits=%zu misses=%zu "
               "rasterized=%zu idle=%zu/%zu frames=%zu frame-us=%llu "
               "phases=%llu/%llu/%llu/%llu/%llu/%llu "
               "overflow=%zu/%zu\n",
               psp_focus_feedback.action,
               (unsigned long long) (complete_us
                                     - psp_focus_feedback.started_us),
               (unsigned long long) (visible_us
                                     - psp_focus_feedback.started_us),
               ui->scroll_y, ui->maximum_scroll_y,
               (unsigned long long) render->frame_tile_us,
               render->tiles_allocated, render->tile_capacity,
               render->hits, render->misses, render->rasterized,
               render->idle_jobs_completed, render->idle_units,
               render->frames_rendered,
               (unsigned long long) render->frame_us,
               (unsigned long long) render->frame_setup_us,
               (unsigned long long) render->frame_tile_us,
               (unsigned long long) render->frame_overflow_us,
               (unsigned long long) render->frame_sticky_us,
               (unsigned long long) render->frame_fixed_us,
               (unsigned long long) render->frame_indicator_us,
               render->overflow_tile_commands,
               render->overflow_direct_commands);
        for (uint8_t at = 0; at < psp_focus_feedback.superseded_count; at++) {
            uint64_t started_us = psp_focus_feedback.superseded[at].started_us;
            uint64_t shown_us =
                psp_focus_feedback.superseded[at].first_visible_us != 0
                    ? psp_focus_feedback.superseded[at].first_visible_us
                    : visible_us;
            printf("tilefinch-scroll-feedback: action=%d elapsed=%lluus "
                   "first-visible=%lluus superseded=1\n",
                   psp_focus_feedback.action,
                   (unsigned long long) (complete_us - started_us),
                   (unsigned long long) (shown_us - started_us));
        }
        psp_focus_feedback.superseded_count = 0;
        psp_focus_feedback.engine = NULL;
        return;
    }
    printf("tilefinch-focus-feedback: action=%d shown=1 visible=%u elapsed=%lluus "
           "rect=%d,%d,%d,%d relayouts=%zu layout-total=%lluus "
           "fixed=%zu/%zu/%zu fixed-us=%lluus tile-us=%lluus\n",
           psp_focus_feedback.action, ui->has_focus ? 1u : 0u,
           (unsigned long long) (sceKernelGetSystemTimeWide() - psp_focus_feedback.started_us),
           ui->focus_x, ui->focus_y, ui->focus_width, ui->focus_height,
           counters.incremental_relayouts,
           (unsigned long long) counters.layout_us,
           counters.layout_fixed_count, render->fixed_cache_builds,
           render->fixed_cache_blits,
           (unsigned long long) render->frame_fixed_us,
           (unsigned long long) render->frame_tile_us);
    psp_focus_feedback.engine = NULL;
    /* After the elapsed time above was taken: hashing reads VRAM. */
    psp_focus_frame_report(engine, "complete");
}

/* psp_media_track_menu_blit's GE blit: its cost, and on a changed source a
   deterministic 8x8 channel-map oracle. */
void psp_vprobe_track_menu_blit(
    bool blitted, const PspMediaPresentGeCost *cost_in,
    const uint16_t *source, int source_width, int source_height,
    int source_stride, bool source_changed, const uint32_t *destination,
    int destination_stride, int left, int top)
{
    PspMediaPresentGeCost cost = *cost_in;
    PspPresentationCadence *metrics = &psp_presentation_cadence;
    if (blitted) {
        uint64_t elapsed_us = cost.submit_us + cost.sync_us;
        metrics->track_menu_blits++;
        metrics->track_menu_blit_total_us += elapsed_us;
        if (elapsed_us > metrics->track_menu_blit_max_us)
            metrics->track_menu_blit_max_us = elapsed_us;
        if (source_changed) {
            unsigned mismatches = 0u;
            unsigned samples = 0u;
            /* The exhaustive 61,952-pixel device proof is a release-time
               probe, not interactive work. Keep a deterministic 8x8 oracle
               in ordinary validation runs so a bad channel map or geometry
               still fails visibly without stalling the menu-opening frame. */
            for (unsigned sample_y = 0; sample_y < 8u; sample_y++) {
                int y = (int) (sample_y
                    * (unsigned) (source_height - 1) / 7u);
                for (unsigned sample_x = 0; sample_x < 8u; sample_x++) {
                    int x = (int) (sample_x
                        * (unsigned) (source_width - 1) / 7u);
                    uint16_t pixel = source[(size_t) y * source_stride + x];
                    unsigned red = tilefinch_rgb565_red_code(pixel);
                    unsigned green = tilefinch_rgb565_green_code(pixel);
                    unsigned blue = tilefinch_rgb565_blue_code(pixel);
                    red = (red << 3) | (red >> 2);
                    green = (green << 2) | (green >> 4);
                    blue = (blue << 3) | (blue >> 2);
                    uint32_t expected = (uint32_t) red
                        | ((uint32_t) green << 8)
                        | ((uint32_t) blue << 16)
                        | UINT32_C(0xff000000);
                    uint32_t got = destination[
                        (size_t) (top + y) * destination_stride
                            + (size_t) (left + x)];
                    /* GU_TCC_RGB leaves the otherwise-unused scanout alpha
                       byte unspecified; the panel and later RGB565 backdrop
                       conversion consume only the three colour channels. */
                    if ((got & UINT32_C(0x00ffffff))
                        != (expected & UINT32_C(0x00ffffff))) {
                        mismatches++;
                    }
                    samples++;
                }
            }
            printf(
                "tilefinch-track-menu-blit: samples=%lu mismatches=%lu "
                "submit=%lluus sync=%lluus\n",
                (unsigned long) samples,
                (unsigned long) mismatches,
                (unsigned long long) cost.submit_us,
                (unsigned long long) cost.sync_us);
        }
    } else {
        metrics->track_menu_blit_fallbacks++;
    }
}

/* The video-surface present's probes: first-picture (player start) and
   track-menu feedback phase clocks, presentation cadence, and the scripted
   capture of each accepted scanout. */
void psp_vprobe_video_begin(PspVprobeVideo *probe)
{
    if (psp_player_start_clocks != NULL)
        psp_player_start_clocks[1] = psp_menu_phase_clock();
    uint64_t presentation_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    unsigned menu_key = psp_active_media->ui_presentation.track_menu_open
        ? psp_active_media->ui_presentation.track_menu_tab + 1u : 0u;
    unsigned menu_input_us = atomic_load_explicit(
        &psp_media_menu_feedback_time, memory_order_acquire);
    bool measure_menu = menu_input_us != 0u
        && atomic_load_explicit(&psp_media_menu_feedback_key, memory_order_relaxed) == menu_key
        && (uint32_t) presentation_started_us - menu_input_us < 1000000u;
    *probe = (PspVprobeVideo) {
        .presentation_started_us = presentation_started_us,
        .menu_key = menu_key,
        .menu_input_us = menu_input_us,
        .measure_menu = measure_menu
    };
    if (measure_menu) probe->menu_clocks[0] = psp_menu_phase_clock();
}

/* The scanout about to be accepted reuses the front picture (or, with
   false, is freshly drawn). */
void psp_vprobe_video_skipped(bool skipped)
{
    psp_media_present_last_skipped = false;
    if (skipped) psp_media_present_last_skipped = true;
}

void psp_vprobe_video_drawn(PspVprobeVideo *probe)
{
    if (psp_player_start_clocks != NULL)
        psp_player_start_clocks[2] = psp_menu_phase_clock();
    if (probe->measure_menu) probe->menu_clocks[1] = psp_menu_phase_clock();
}

void psp_vprobe_video_composed(PspVprobeVideo *probe)
{
    if (psp_player_start_clocks != NULL)
        psp_player_start_clocks[3] = psp_menu_phase_clock();
    if (probe->measure_menu) probe->menu_clocks[2] = psp_menu_phase_clock();
    psp_cadence_composed(probe->presentation_started_us, false);
    probe->published_index = psp_display.back_buffer;
}

void psp_vprobe_video_publish_returned(bool published)
{
    if (psp_player_start_clocks != NULL) {
        psp_player_start_clocks[4] = psp_menu_phase_clock();
        psp_player_start.published[psp_player_start.count++] = published;
        psp_player_start_clocks = NULL;
    }
}

void psp_vprobe_video_published(
    PspVprobeVideo *probe, bool published, bool menu_feedback,
    const uint32_t *vram)
{
    unsigned menu_input_us = probe->menu_input_us;
    if (probe->measure_menu && published) {
        probe->menu_clocks[3] = psp_menu_phase_clock();
        psp_menu_phase_report(probe->menu_key, menu_input_us, probe->menu_clocks);
        atomic_compare_exchange_strong_explicit(&psp_media_menu_feedback_time,
            &menu_input_us, 0u, memory_order_relaxed, memory_order_relaxed);
    }
    psp_cadence_published(published);
    psp_cadence_video_published(published,
        menu_feedback ? NULL : &psp_active_media->frame);
    if (published) {
        psp_input_script_capture_media_present(
            &psp_active_media->ui, NULL, vram, PSP_VRAM_STRIDE,
            true, probe->published_index, psp_media_present_last_skipped, false,
            menu_feedback ? NULL : &psp_active_media->frame, psp_media_present_records,
            PSP_DISPLAY_PAGE_BUFFER_COUNT);
    }
}

/* psp_present_internal's first-picture sample: at most four loading and four
   playing presents per media generation. */
void psp_vprobe_player_start(bool media_replaces_page)
{
    psp_player_start_clocks = NULL;
    if (media_replaces_page) {
        if (psp_player_start.generation != psp_active_media->generation) {
            memset(&psp_player_start, 0, sizeof(psp_player_start));
            psp_player_start.generation = psp_active_media->generation;
        }
        bool resolving = psp_active_media->ui.resolving;
        unsigned *samples = resolving ? &psp_player_start.loading_count
                                      : &psp_player_start.playing_count;
        if (*samples < 4u && psp_player_start.count < 8u) {
            psp_player_start_clocks = psp_player_start.clocks[psp_player_start.count];
            psp_player_start.resolving[psp_player_start.count] = resolving;
            (*samples)++;
            psp_player_start_clocks[0] = psp_menu_phase_clock();
        }
    }
}

void psp_vprobe_player_start_abandon(void)
{
    psp_player_start_clocks = NULL;
}

/* psp_present_internal's phase record: the base copy (wall, owner CPU and
   bytes), the chrome composite and the publication, with the display
   backend's flush/framebuffer/vblank split. */
void psp_vprobe_phase_begin(PspVprobePhase *phase)
{
    uint64_t presentation_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    *phase = (PspVprobePhase) {
        .presentation_started_us = presentation_started_us,
        .base_finished_us = presentation_started_us,
        .composite_finished_us = presentation_started_us
    };
    phase->base_cpu_before_valid = psp_present_cpu_measurement
        && psp_present_thread_cpu(&phase->base_cpu_before);
}

void psp_vprobe_phase_base_bytes(PspVprobePhase *phase, uint32_t base_bytes)
{
    phase->base_bytes = base_bytes;
}

void psp_vprobe_phase_copied_rows(
    PspVprobePhase *phase, unsigned opaque_top, unsigned copy_end)
{
    phase->base_bytes = (copy_end > opaque_top ? copy_end - opaque_top : 0u)
        * PSP_SCREEN_WIDTH * sizeof(uint16_t);
}

void psp_vprobe_phase_base_done(PspVprobePhase *phase)
{
    phase->base_finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    phase->base_cpu_after_valid = psp_present_cpu_measurement
        && psp_present_thread_cpu(&phase->base_cpu_after);
}

void psp_vprobe_phase_ui_composited(PspVprobePhase *phase)
{
    (void) psp_ui_validation_last_timing(&phase->ui_timing);
}

void psp_vprobe_phase_composed(PspVprobePhase *phase, bool native_surface)
{
    phase->composite_finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    psp_cadence_composed(phase->presentation_started_us, native_surface);
    phase->published_index = psp_display.back_buffer;
    (void) psp_display_validation_timing_snapshot(&phase->display_timing_before);
    if (psp_present_pre_publish_hook != NULL) {
        uint32_t next = psp_present_last_timing.sequence + 1u;
        psp_present_pre_publish_hook(next == 0u ? 1u : next);
    }
}

void psp_vprobe_phase_published(
    const PspVprobePhase *phase, bool published, bool media_visible,
    bool media_replaces_page, const uint16_t *frame, const PspUiState *ui,
    const uint16_t *vram)
{
    uint64_t presentation_started_us = phase->presentation_started_us;
    uint64_t base_finished_us = phase->base_finished_us;
    uint64_t composite_finished_us = phase->composite_finished_us;
    uint64_t base_cpu_before = phase->base_cpu_before;
    uint64_t base_cpu_after = phase->base_cpu_after;
    bool base_cpu_before_valid = phase->base_cpu_before_valid;
    bool base_cpu_after_valid = phase->base_cpu_after_valid;
    uint32_t base_bytes = phase->base_bytes;
    PspUiCompositeTiming ui_timing = phase->ui_timing;
    unsigned published_index = phase->published_index;
    PspDisplayBackendTiming display_timing_before = phase->display_timing_before;
    uint64_t published_us = (uint64_t) sceKernelGetSystemTimeWide();
    PspDisplayBackendTiming display_timing_after = {0};
    (void) psp_display_validation_timing_snapshot(&display_timing_after);
    uint32_t sequence = psp_present_last_timing.sequence + 1u;
    if (sequence == 0) sequence = 1u;
    psp_present_last_timing = (PspPresentPhaseTiming) {
        .base_us = base_finished_us - presentation_started_us,
        .base_cpu_valid = base_cpu_before_valid && base_cpu_after_valid
            && base_cpu_after >= base_cpu_before,
        .base_cpu_us = base_cpu_after >= base_cpu_before
            ? base_cpu_after - base_cpu_before : 0,
        .base_bytes = base_bytes,
        .composite_us = composite_finished_us - base_finished_us,
        .publish_us = published_us - composite_finished_us,
        .publish_flush_us = display_timing_after.flush_us
            - display_timing_before.flush_us,
        .publish_set_framebuffer_us =
            display_timing_after.set_framebuffer_us
                - display_timing_before.set_framebuffer_us,
        .publish_vblank_us = display_timing_after.vblank_us
            - display_timing_before.vblank_us,
        .ui_focus_scroll_us = ui_timing.focus_scroll_us,
        .ui_chrome_us = ui_timing.chrome_us,
        .ui_cursor_us = ui_timing.cursor_us,
        .ui_screen_us = ui_timing.screen_us,
        .ui_toast_us = ui_timing.toast_us,
        .ui_loading_us = ui_timing.loading_us,
        .sequence = sequence
    };
    if (published && psp_presentation_cadence.cursor_sample_pending_us != 0) {
        PspPresentationCadence *metrics = &psp_presentation_cadence;
        uint64_t compose = composite_finished_us - presentation_started_us;
        uint64_t publish = published_us - composite_finished_us;
        metrics->cursor_work_samples++;
        metrics->cursor_compose_total_us += compose;
        metrics->cursor_publish_total_us += publish;
        if (compose > metrics->cursor_compose_max_us)
            metrics->cursor_compose_max_us = compose;
        if (publish > metrics->cursor_publish_max_us)
            metrics->cursor_publish_max_us = publish;
    }
    psp_cadence_published(published);
    if (published && !media_visible) psp_input_script_measure_presented(ui);
    if (published && !media_visible)
        psp_focus_frame_note_present(frame, ui, vram);
    if (published && !media_visible) psp_focus_feedback_published(frame, ui);
    if (published && media_visible) {
        psp_input_script_capture_media_present(
            &psp_active_media->ui, vram, NULL, PSP_VRAM_STRIDE,
            false, published_index,
            media_replaces_page && psp_media_present_last_skipped,
            false, media_replaces_page ? &psp_active_media->frame : NULL,
            psp_media_present_records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    }
}

void psp_vprobe_fonts_begin(PspVprobeFonts *probe)
{
    probe->before = psp_menu_phase_clock();
}

void psp_vprobe_fonts_end(const PspVprobeFonts *probe)
{
    PspMenuPhaseClock fonts_before = probe->before;
    PspMenuPhaseClock fonts_after = psp_menu_phase_clock();
    PspUiChromeGlyphPreload preload = psp_ui_chrome_glyph_preload();
    if (preload.glyphs != 0u) {
        bool valid = fonts_before.valid && fonts_after.valid
            && fonts_after.cpu >= fonts_before.cpu;
        printf("tilefinch-player-fonts: regular-large-glyphs=%u wall-low=%lluus wall-high=%lluus cpu=%lluus valid=%u\n",
            preload.glyphs,
            (unsigned long long) (fonts_after.before - fonts_before.after),
            (unsigned long long) (fonts_after.after - fonts_before.before),
            (unsigned long long) (valid ? fonts_after.cpu - fonts_before.cpu : 0), valid);
    }
}

void psp_vprobe_focus_first_visible(
    const BrowserEngine *engine, const NavigationSession *nav, uint64_t now_us)
{
    /* This accepted frame already contains the new
       native focus ring, even though author styling
       has not rendered. Keep that latency distinct
       from the later complete-frame receipt. */
    if (psp_focus_feedback.engine == engine
        && psp_focus_feedback.generation == nav->generation
        && !psp_focus_feedback_page_action(psp_focus_feedback.action)
        && psp_focus_feedback.first_visible_us == 0) {
        psp_focus_feedback.first_visible_us = now_us;
        printf("tilefinch-focus-first-visible: action=%d "
               "elapsed=%lluus native=1\n",
               psp_focus_feedback.action,
               (unsigned long long) (now_us
                   - psp_focus_feedback.started_us));
    }
}

/* Only so the mode line is printed once rather than once a frame. */
static int psp_media_present_reported_mode = -1;
static uint64_t psp_media_present_reported_generation;
static int psp_media_present_reported_width;
static int psp_media_present_reported_height;

/*
 * One line per opened stream, or per change of mind within one. The device
 * truth cycle reads this to learn which presenter actually ran: a session
 * that silently fell back to software would otherwise look exactly like one
 * that chose it, and the frame budget of the two is not the same. The line
 * is validation-log output, so other builds compile the report away.
 */
__attribute__((noinline))
void psp_media_present_report(
    PspMediaPresentMode mode, const char *reason,
    const MediaVideoFrame *frame, const PspMediaPresentPlan *plan,
    const PspMediaPresentTexture *texture)
{
    uint64_t generation =
        psp_active_media == NULL ? 0 : psp_active_media->generation;
    if (psp_media_present_reported_mode == (int) mode
        && psp_media_present_reported_generation == generation
        && psp_media_present_reported_width == frame->width
        && psp_media_present_reported_height == frame->height) return;
    psp_media_present_reported_mode = (int) mode;
    psp_media_present_reported_generation = generation;
    psp_media_present_reported_width = frame->width;
    psp_media_present_reported_height = frame->height;
    uint32_t passthrough_drawn = 0;
    uint32_t passthrough_source = 0;
    psp_media_present_ge_passthrough(
        &passthrough_drawn, &passthrough_source);
    printf("tilefinch-media-present: mode=%s reason=%s generation=%llu "
           "source=%dx%d stride=%d format=%d output=%dx%d left=%d top=%d "
           "bands=%u quads=%u texture=%dx%d seam=%d surface=%s "
           "sampled=%s@%s "
           "passthrough-drawn=0x%08x passthrough-source=0x%08x\n",
           psp_media_present_mode_name(mode),
           reason == NULL ? "unknown" : reason,
           (unsigned long long) generation,
           frame->width, frame->height, frame->stride_pixels,
           (int) frame->format,
           plan->video.width, plan->video.height,
           plan->video.x, plan->video.y,
           (unsigned) plan->band_count, (unsigned) plan->quad_count,
           plan->quad_count == 0 ? 0 : plan->quads[0].texture_width,
           plan->quad_count == 0 ? 0 : plan->quads[0].texture_height,
           plan->quad_count < 2 ? 0 : plan->quads[1].texture_column,
           psp_display_video_active(&psp_display) ? "8888" : "565",
           /* The single largest term in a video frame is where the engine
              reads its texture from; say it rather than assume the staging
              handed in was the staging used. */
           texture == NULL ? "none"
               : (texture->staged ? "staged" : "linear"),
           texture == NULL ? "none"
               : (psp_display_in_edram(&psp_display, texture->pixels)
                      ? "edram" : "main"),
           (unsigned) passthrough_drawn, (unsigned) passthrough_source);
}

/* psp_media_present_account's cost totals for the media job report and
   the stability summary. */
void psp_vprobe_present_account(
    uint64_t started_us, const PspMediaPresentGeCost *cost, bool drawn)
{
    uint64_t elapsed_us =
        (uint64_t) sceKernelGetSystemTimeWide() - started_us;
    if (psp_active_media != NULL) {
        psp_active_media->present_scale_frames++;
        psp_active_media->present_scale_total_us += elapsed_us;
        if (elapsed_us > psp_active_media->present_scale_max_us)
            psp_active_media->present_scale_max_us = elapsed_us;
        if (drawn) {
            psp_active_media->present_ge_frames++;
            psp_active_media->present_ge_submit_total_us += cost->submit_us;
            psp_active_media->present_ge_sync_total_us += cost->sync_us;
            if (cost->sync_us > psp_active_media->present_ge_sync_max_us)
                psp_active_media->present_ge_sync_max_us = cost->sync_us;
            psp_active_media->present_ge_wait_total_us += cost->wait_us;
            if (cost->wait_us > psp_active_media->present_ge_wait_max_us)
                psp_active_media->present_ge_wait_max_us = cost->wait_us;
        }
    }
}

/* Load-time page-scroll responsiveness; see psp_load_experience.h. Written
   by the input supervisor (presses) and the preview scroller (movement). */
static PspLoadScroll psp_load_scroll;

void psp_load_scroll_note_press(void)
{
    psp_load_scroll_press(&psp_load_scroll, sceKernelGetSystemTimeLow());
}

void psp_load_scroll_note_moved(void)
{
    uint32_t now = sceKernelGetSystemTimeLow();
    uint32_t pending = psp_load_scroll.pending_press_us;
    if (pending != 0 && now - pending >= 300000u)
        printf("tilefinch-load-scroll-slow: wait=%lums\n",
               (unsigned long) ((now - pending) / 1000u));
    psp_load_scroll_moved(&psp_load_scroll, now);
}

void psp_load_experience_report(const char *scope, BrowserEngine *engine,
                                const BrowserNavigationJobMetrics *metrics)
{
    if (metrics == NULL) return;
    BrowserValidationCounters counters;
    bool loaded = metrics->status == BROWSER_NAVIGATION_JOB_SUCCEEDED
        && browser_engine_validation_counters(engine, &counters)
        && counters.loaded;
    PspLoadReach reach = {
        .first_present_us = metrics->provisional_first_present_us,
        .preview_reach_us = metrics->provisional_reach_us,
        .preview_reach_px = metrics->provisional_reach_px,
        .loaded_us = loaded ? metrics->elapsed_us : 0,
        .page_height_px = loaded ? counters.layout_height : 0,
        .screen_height_px = PSP_SCREEN_HEIGHT
    };
    bool unresolved = psp_load_scroll_finish(
        &psp_load_scroll, sceKernelGetSystemTimeLow());
    static const unsigned screens[] = { 1u, 2u, 5u, 10u };
    char reach_text[96];
    size_t used = 0;
    for (size_t at = 0; at < sizeof(screens) / sizeof(screens[0]); at++) {
        uint64_t when = psp_load_reach_time(&reach, screens[at]);
        int written = when == UINT64_MAX
            ? snprintf(reach_text + used, sizeof(reach_text) - used,
                       "%s%u:never", at == 0 ? "" : ",", screens[at])
            : snprintf(reach_text + used, sizeof(reach_text) - used,
                       "%s%u:%llums", at == 0 ? "" : ",", screens[at],
                       (unsigned long long) (when / 1000u));
        if (written < 0 || (size_t) written >= sizeof(reach_text) - used)
            break;
        used += (size_t) written;
    }
    printf("tilefinch-load-experience: scope=%s status=%d "
           "first-present=%llums preview-reach=%dpx@%llums "
           "loaded=%llums height=%d screens-reached=%s "
           "screens-at-5s=%u screens-at-10s=%u "
           "load-scroll=%u/%u max-move=%lums longest-wait=%lums%s\n",
           scope, (int) metrics->status,
           (unsigned long long) (reach.first_present_us / 1000u),
           reach.preview_reach_px,
           (unsigned long long) (reach.preview_reach_us / 1000u),
           (unsigned long long) (reach.loaded_us / 1000u),
           reach.page_height_px, reach_text,
           psp_load_reach_screens_at(&reach, 5000000u),
           psp_load_reach_screens_at(&reach, 10000000u),
           psp_load_scroll.moved, psp_load_scroll.presses,
           (unsigned long) (psp_load_scroll.max_move_latency_us / 1000u),
           (unsigned long) (psp_load_scroll.longest_wait_us / 1000u),
           unresolved ? " unresolved=1" : "");
    psp_load_scroll_reset(&psp_load_scroll);
}
