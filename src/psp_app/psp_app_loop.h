/* The interactive loop's carried state, shared by psp_script_main.c, which
 * runs the loop, and the validation loop drivers in
 * psp_app_validation_loop.c.
 */
#ifndef TILEFINCH_PSP_APP_LOOP_H
#define TILEFINCH_PSP_APP_LOOP_H

#include "psp_app_validation.h"
#include "tilefinch/psp_update_check.h"
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)
#include "psp_update_e2e.h"
#endif

/* What one frame of the interactive loop leaves for the next, plus the
   borrowed owners and boot facts its steps need. It lives for one
   psp_app_run_interactive call; per-frame facts stay in PspAppFrameState. */
struct PspLoop {
    PspApp app;
    PspBootQueue *boot_queue;
    FetchPreconnectDwell *home_preconnect_dwell;
    PspPowerPolicy *power_policy;
    bool loaded;
    bool restoring_last_page;
    bool initial_error_page;
    /* Launcher trial of a freshly installed slot, confirmed on first use. */
    TilefinchUpdateState trial_health_state;
    TilefinchUpdateSlot running_slot;
    bool trial_health_pending;
    uint64_t trial_health_started_us;
    uint64_t trial_health_next_attempt_us;
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
&& defined(TILEFINCH_PSP_LIVE_NETWORK)
    PspUpdateE2E update_e2e;
#endif
    PspUpdateCheck update_check;
    unsigned voice_pressure_ticks;
    unsigned scroll_log_counter;
    bool render_job_pending;
    uint64_t render_job_last_progress_us;
    /* A scrolled frame is on screen with checkerboard tiles still filling. */
    bool render_placeholder_active;
    size_t navigation_pump_boundaries;
    size_t blocker_navigation_baseline;
    bool blocker_navigation_active;
    long interactive_validation_ticks;
    /* The media-open scope is the only cooperate scope this loop holds
       across frames, so the loop -- not a per-frame predicate -- has to
       remember that it owns one. */
    bool media_open_scope;
    uint64_t media_stability_target_us;
    bool media_stability_lifecycle_injected;
    uint64_t loading_present_us;
    uint64_t previous_ui_sample_us;
    /* Sample the next input without another vblank wait (see
       psp_wait_before_interactive_input). */
    bool fast_page_followup;
    uint64_t render_progress_present_us;
    bool canvas_page_followup;
    /* The previous iteration presented a focus move over a live canvas
       and skipped the page's animation callbacks. */
    bool focus_runtime_deferred;
    /* The frame's wait found a page task already runnable and yielded
       instead of waiting for vblank. */
    bool page_task_yield;
    uint64_t next_color_mode_check_us;
    unsigned power_worker_completions;
    unsigned power_worker_failures;
    uint64_t power_high_ms;
    uint64_t power_idle_ms;
    uint64_t power_transition_ms;
    size_t lifecycle_network_requests_cancelled;
    bool lifecycle_network_was_started;
    bool lifecycle_network_was_ready;
};

#endif
