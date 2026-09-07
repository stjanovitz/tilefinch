/* Device runtime for the PSP browser EBOOT: display presentation and cadence,
 * exit/power callbacks, the clock policy worker, the cooperative-work
 * supervisor that keeps the panel alive during long engine operations, the
 * validation-only power test, and the boot entrance.
 *
 * The callback-visible process globals live here because this TU owns their
 * writers. Ordinary browser resources remain in the canonical owner records.
 */
#include "psp_app_internal.h"
#include <psputility_sysparam.h>
#include "tilefinch/pixel_math.h"
#include "tilefinch/psp_time.h"
#include "tilefinch/psp_threads.h"

PspMediaSession *psp_active_media;
atomic_uint psp_background_ui_available;
atomic_bool psp_home_exit_requested;
PspLifecycle psp_lifecycle;

#define PSP_CERTIFICATE_YOUTUBE_HEAD "YouTube page fetch failed"
#define PSP_CERTIFICATE_GENERIC_HEAD "Secure page fetch failed"
_Static_assert(
    sizeof(PSP_CERTIFICATE_YOUTUBE_HEAD) - 1u
        <= PSP_UI_LARGE_TOAST_CHARACTER_LIMIT,
    "YouTube certificate headline must fit the large PSP toast");
_Static_assert(
    sizeof(PSP_CERTIFICATE_GENERIC_HEAD) - 1u
        <= PSP_UI_LARGE_TOAST_CHARACTER_LIMIT,
    "generic certificate headline must fit the large PSP toast");
const char *psp_user_visible_error(
    const char *detail, long tls_verify_result,
    bool tls_verify_result_available, bool tls_verification_failed,
    TilefinchTlsGuidance *tls_guidance,
    char *summary, size_t summary_size)
{
    if (detail == NULL) detail = "";
    if (tls_guidance != NULL) *tls_guidance = TILEFINCH_TLS_GUIDANCE_NONE;
    if (summary == NULL || summary_size == 0
        || !tls_verification_failed) {
        return detail;
    }
    PspTimeStatus clock_status = psp_time_read_utc(NULL);
    /* Keep the useful route-level context, but move the recovery action onto
       its own full-width line. The unabridged backend sentence remains in the
       failure report rather than being clipped into an unreadable toast. */
    const char *headline = strstr(
            detail, "YouTube page fetch failed") != NULL
        ? PSP_CERTIFICATE_YOUTUBE_HEAD
        : PSP_CERTIFICATE_GENERIC_HEAD;
    if (tls_guidance != NULL) {
        *tls_guidance = tilefinch_tls_verification_guidance(
            (uint32_t) tls_verify_result,
            tls_verify_result_available, clock_status);
    }
    snprintf(summary, summary_size, "%s", headline);
    return summary;
}

bool psp_exit_plan_requested(const PspExitPlan *plan)
{
    return plan != NULL && plan->cause != PSP_EXIT_NONE;
}

bool psp_exit_plan_restarts_launcher(const PspExitPlan *plan)
{
    return plan != NULL
        && (plan->cause == PSP_EXIT_UPDATE_RESTART
            || plan->cause == PSP_EXIT_CONFIG_RESTART);
}

void psp_exit_plan_request(PspExitPlan *plan, PspExitCause cause)
{
    if (plan == NULL || cause == PSP_EXIT_NONE) return;
    /* A restart is a stronger handoff than an ordinary exit. Once selected,
       a later HOME callback or validation completion cannot downgrade it. */
    if (psp_exit_plan_restarts_launcher(plan)) return;
    if (cause == PSP_EXIT_UPDATE_RESTART
        || cause == PSP_EXIT_CONFIG_RESTART
        || plan->cause == PSP_EXIT_NONE) {
        plan->cause = cause;
    }
}

void psp_presentation_init(PspPresentationResources *presentation)
{
    if (presentation == NULL) return;
    memset(presentation, 0, sizeof(*presentation));
    psp_ui_init(&presentation->ui);
    psp_ui_set_tabs(&presentation->ui, &presentation->tab_view);
}

void psp_presentation_bind_chrome_fonts(
    PspPresentationResources *presentation, BrowserEngine *engine)
{
    if (presentation == NULL || engine == NULL) return;
    const FontFace *regular = browser_engine_font_face(engine, FONT_SANS);
    if (regular == NULL)
        regular = browser_engine_native_home_font(engine);
    psp_ui_set_chrome_fonts(
        regular,
        browser_engine_font_face_variant(
            engine, FONT_SANS, false, true));
    presentation->chrome_fonts_bound = true;
}

void psp_presentation_unbind_chrome_fonts(
    PspPresentationResources *presentation)
{
    if (presentation == NULL || !presentation->chrome_fonts_bound) return;
    psp_ui_clear_chrome_font();
    presentation->chrome_fonts_bound = false;
}

static atomic_int psp_callback_setup_ready;
static atomic_int psp_callback_setup_result;
static atomic_int psp_power_callback_setup_result;
static atomic_int psp_home_exit_watchdog_started;

/* For PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT, read once for the status clock. */
#include <psputility.h>

static int psp_home_exit_watchdog(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    for (unsigned waited_ms = 0;
         waited_ms < PSP_HOME_EXIT_GRACE_MS; waited_ms += 10u) {
        (void) sceKernelDelayThread(10000);
    }
    psp_log_emergency("home-exit-grace-expired");
    sceKernelExitGame();
    sceKernelExitDeleteThread(0);
    return 0;
}

static bool psp_start_home_exit_watchdog(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &psp_home_exit_watchdog_started, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) return true;
    int thread_id = sceKernelCreateThread(
        "home-exit-watchdog", psp_home_exit_watchdog,
        TILEFINCH_PSP_THREAD_PRIORITY_WATCHDOG, 4u * 1024u, 0, NULL);
    int started = thread_id < 0
        ? thread_id : sceKernelStartThread(thread_id, 0, NULL);
    if (started >= 0) return true;
    if (thread_id >= 0) (void) sceKernelDeleteThread(thread_id);
    atomic_store_explicit(
        &psp_home_exit_watchdog_started, 0, memory_order_release);
    return false;
}

static int psp_exit_callback(int arg1, int arg2, void *common)
{
    (void) arg1; (void) arg2; (void) common;
    atomic_store_explicit(
        &psp_home_exit_requested, true, memory_order_release);
    psp_log_emergency("home-exit-requested");
    /* HOME is delivered on the callback thread. Engine/session state belongs
       to the main thread, so never serialize it here. Return immediately so
       this same thread can resume the busy-screen supervisor: it turns the
       request into transport cancellation and visible CLOSING feedback on
       its next tick. A separate one-shot thread retains the finite escape
       hatch for a genuinely non-cooperative firmware call. */
    if (psp_start_home_exit_watchdog()) return 0;
    /* Low-memory fallback: if the tiny failsafe thread cannot be admitted,
       preserve the old terminal guarantee even though presentation cannot
       advance during this exceptional wait. */
    for (unsigned waited_ms = 0;
         waited_ms < PSP_HOME_EXIT_GRACE_MS; waited_ms += 10u) {
        (void) sceKernelDelayThread(10000);
    }
    psp_log_emergency("home-exit-grace-expired");
    sceKernelExitGame();
    return 0;
}

bool psp_home_exit_pending(void)
{
    return atomic_load_explicit(
        &psp_home_exit_requested, memory_order_acquire);
}

static int psp_power_callback(int unknown, int power_info, void *common)
{
    (void) unknown;
    (void) common;
    /*
     * PSPSDK delivers this on the callback thread, sometimes before the main
     * thread can execute any suspend-side work. Keep it allocation-free and
     * non-blocking: the browser thread owns every resource transition.
     */
    if ((power_info
            & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) != 0) {
        psp_lifecycle_notify_suspend(&psp_lifecycle);
    }
    if ((power_info & PSP_POWER_CB_RESUMING) != 0) {
        psp_lifecycle_notify_resume(&psp_lifecycle);
    } else if ((power_info & PSP_POWER_CB_RESUME_COMPLETE) != 0) {
        /* Some firmware reports COMPLETE without an observable RESUMING.
           Epoch matching also coalesces the ordinary two-callback case. */
        psp_lifecycle_notify_resume(&psp_lifecycle);
    }
    return 0;
}

static int psp_callback_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    int callback_id = sceKernelCreateCallback("exit", psp_exit_callback,
                                              NULL);
    int registered = callback_id < 0
        ? callback_id : sceKernelRegisterExitCallback(callback_id);
    printf("tilefinch-callback: callback=%d registered=0x%08x\n",
           callback_id, (unsigned) registered);
    int power_callback_id = sceKernelCreateCallback(
        "power", psp_power_callback, NULL);
    int power_slot = power_callback_id < 0
        ? power_callback_id
        : scePowerRegisterCallback(-1, power_callback_id);
    printf("tilefinch-power-callback: callback=%d slot=0x%08x\n",
           power_callback_id, (unsigned) power_slot);
    atomic_store_explicit(
        &psp_background_ui_available, registered >= 0 ? 1u : 0u,
        memory_order_release);
    atomic_store_explicit(
        &psp_callback_setup_result, registered, memory_order_relaxed);
    atomic_store_explicit(
        &psp_power_callback_setup_result, power_slot,
        memory_order_relaxed);
    atomic_store_explicit(
        &psp_callback_setup_ready, 1, memory_order_release);
    while (registered >= 0 || power_slot >= 0) {
        /* Reuse the mandatory callback thread as the loading-UI supervisor.
           It owns no engine state and consumes no additional thread stack. */
        (void) sceKernelDelayThreadCB(16000);
        if (psp_lifecycle_presentation_allowed(&psp_lifecycle))
            psp_background_ui_tick();
    }
    return 0;
}

int psp_setup_callbacks(void)
{
    atomic_store_explicit(
        &psp_callback_setup_ready, 0, memory_order_relaxed);
    atomic_store_explicit(
        &psp_callback_setup_result, -1, memory_order_relaxed);
    atomic_store_explicit(
        &psp_power_callback_setup_result, -1, memory_order_relaxed);
    atomic_store_explicit(
        &psp_background_ui_available, 0u, memory_order_relaxed);
    atomic_store_explicit(
        &psp_home_exit_watchdog_started, 0, memory_order_relaxed);
    int thread_id = sceKernelCreateThread(
        "callbacks", psp_callback_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK, 8u * 1024u, 0, NULL);
    int started = thread_id < 0
        ? thread_id : sceKernelStartThread(thread_id, 0, NULL);
    printf("tilefinch-callback: thread=%d started=0x%08x\n",
           thread_id, (unsigned) started);
    if (started < 0) return started;

    /* StartThread only proves that the callback worker was admitted. The
       registration calls execute asynchronously inside it and can fail for
       PSP-only reasons (callback slots, stack admission, CFW state). The
       worker has higher priority than the browser thread, so this normally
       observes READY immediately; retain a small finite bound so boot never
       depends on the scheduler doing so. */
    for (unsigned waited_ms = 0; waited_ms < 20u; waited_ms++) {
        if (atomic_load_explicit(
                &psp_callback_setup_ready, memory_order_acquire) != 0) {
            int registered = atomic_load_explicit(
                &psp_callback_setup_result, memory_order_relaxed);
            int power_slot = atomic_load_explicit(
                &psp_power_callback_setup_result,
                memory_order_relaxed);
            printf(
                "tilefinch-callback: ready exit=0x%08x power=0x%08x "
                "waited-ms=%u\n",
                (unsigned) registered, (unsigned) power_slot,
                waited_ms);
            return registered;
        }
        (void) sceKernelDelayThread(1000);
    }
    printf("tilefinch-callback: setup timed out after 20ms\n");
    return -1;
}

uint64_t psp_time_ns(void *context)
{
    (void) context;
    return (uint64_t) sceKernelGetSystemTimeWide() * 1000u;
}

uint64_t psp_time_us(void *context)
{
    (void) context;
    return (uint64_t) sceKernelGetSystemTimeWide();
}

uint64_t psp_wall_time_ns(void *context)
{
    (void) context;
    /* psp_time.c supplies the RTC-backed time() implementation used by TLS.
       Do not substitute sceKernelGetSystemTimeWide here: that is monotonic
       time since boot, and made expiring media URLs appear valid for decades
       on hardware while host tests continued to see a real Unix epoch. */
    time_t seconds = time(NULL);
    return seconds <= 0 ? 0
        : (uint64_t) seconds * UINT64_C(1000000000);
}

const char *psp_preferred_language(void *context)
{
    (void) context;
    /* HOME is presented before platform services are installed, and this
       callback is not invoked until the YouTube provider starts its first
       request. Cache that one firmware query for the process lifetime. */
    static const char *cached;
    if (cached != NULL) return cached;
    int language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    if (sceUtilityGetSystemParamInt(
            PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &language) < 0) {
        cached = "en";
        return cached;
    }
    switch (language) {
    case PSP_SYSTEMPARAM_LANGUAGE_JAPANESE: cached = "ja"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_FRENCH: cached = "fr"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_SPANISH: cached = "es"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_GERMAN: cached = "de"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_ITALIAN: cached = "it"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_DUTCH: cached = "nl"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE: cached = "pt"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN: cached = "ru"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_KOREAN: cached = "ko"; break;
    case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL:
        cached = "zh-TW";
        break;
    case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED:
        cached = "zh-CN";
        break;
    case PSP_SYSTEMPARAM_LANGUAGE_ENGLISH:
    default:
        cached = "en";
        break;
    }
    return cached;
}

TilefinchDateFormat psp_preferred_date_format(void *context)
{
    (void) context;
    /* Like the language query, this is first reached while a provider builds
       an already-fetched watch page, never during native HOME startup. */
    static bool cached;
    static TilefinchDateFormat value;
    if (cached) return value;
    int format = PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD;
    if (sceUtilityGetSystemParamInt(
            PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT, &format) >= 0) {
        if (format == PSP_SYSTEMPARAM_DATE_FORMAT_MMDDYYYY)
            value = TILEFINCH_DATE_FORMAT_MONTH_DAY_YEAR;
        else if (format == PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY)
            value = TILEFINCH_DATE_FORMAT_DAY_MONTH_YEAR;
    }
    cached = true;
    return value;
}

void psp_log_message(void *context, const char *message)
{
    (void) context;
    printf("engine: %s\n", message == NULL ? "" : message);
}

/* Rebuilt only when the stream or the display rectangle changes; see
   include/tilefinch/psp_media_scale.h for what it costs and why. Only the
   software presenter -- the Sharp option and the fallback -- uses it. */
static PspMediaScaleMap psp_media_scale_map;

/* What each scanout buffer already holds, so a picture the decoder has not
   replaced is not re-presented into a buffer that already shows it. The
   software path rotates three page buffers while the GE video surface uses
   two; size for the larger set and retain the physical index in each record. */
_Static_assert(
    PSP_DISPLAY_PAGE_BUFFER_COUNT >= PSP_DISPLAY_VIDEO_BUFFER_COUNT,
    "present records must cover every physical scanout buffer");
static PspMediaPresentRecord
    psp_media_present_records[PSP_DISPLAY_PAGE_BUFFER_COUNT];
static PspUiMediaTrackMenuCache psp_media_track_menu_cache;
_Static_assert(
    PSP_DISPLAY_VIDEO_AUX_BYTES
        >= (size_t) PSP_UI_MEDIA_TRACK_MENU_WIDTH
             * (size_t) PSP_UI_MEDIA_TRACK_MENU_HEIGHT * sizeof(uint16_t),
    "the bounded video EDRAM tail must hold the retained track menu");
/* Only so the mode line is printed once rather than once a frame. */
static int psp_media_present_reported_mode = -1;
static uint64_t psp_media_present_reported_generation;
static int psp_media_present_reported_width;
static int psp_media_present_reported_height;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Set by the presenter admission seam and sampled only after a successful
   publish. Validation needs to distinguish a freshly drawn picture from a
   scanout accepted through the same-picture fast path. */
static bool psp_media_present_last_skipped;
#endif

void psp_media_present_forget_buffers(void)
{
    psp_media_present_records_reset(
        psp_media_present_records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
}

/*
 * Smooth (the graphics engine, bilinear) is the default; Sharp is the
 * software scaler's nearest neighbour. The profile is read per present rather
 * than latched at open, which costs one load and makes the setting apply the
 * moment it can: Options is unreachable while the player owns the display, so
 * in practice a change still lands at the next media open.
 */
static PspMediaPresentMode psp_media_present_requested_mode(void)
{
    if (psp_active_media == NULL)
        return PSP_MEDIA_PRESENT_MODE_GE_SMOOTH;
    return browser_profile_video_scaling(psp_active_media->profile)
            == BROWSER_VIDEO_SCALING_SHARP
        ? PSP_MEDIA_PRESENT_MODE_SOFTWARE_SHARP
        : PSP_MEDIA_PRESENT_MODE_GE_SMOOTH;
}

/* Scanout is owned by the shared front end (include/tilefinch/psp_display.h)
   so this browser and the fixture EBOOT cannot drift apart on display mode,
   buffer rotation, sync flag, or result checking. */
PspDisplay psp_display;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
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
    uint64_t cursor_sample_previous_us;
    uint64_t cursor_sample_interval_total_us;
    uint64_t cursor_sample_interval_max_us;
    uint64_t cursor_latency_total_us;
    uint64_t cursor_latency_max_us;
    uint32_t cursor_samples;
    uint32_t cursor_sample_intervals;
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
static PspPresentPhaseTiming psp_present_last_timing;

bool psp_present_validation_last_timing(PspPresentPhaseTiming *timing)
{
    if (timing == NULL || psp_present_last_timing.sequence == 0) return false;
    *timing = psp_present_last_timing;
    return true;
}

void psp_report_presentation_cadence(const char *phase)
{
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
        "average=%lluus max=%lluus\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long) metrics->cursor_sample_intervals,
        (unsigned long long) (metrics->cursor_sample_intervals == 0 ? 0
            : metrics->cursor_sample_interval_total_us
                / metrics->cursor_sample_intervals),
        (unsigned long long) metrics->cursor_sample_interval_max_us);
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
    if (metrics->cursor_sample_previous_us != 0) {
        uint64_t interval_us = sampled_us - metrics->cursor_sample_previous_us;
        metrics->cursor_sample_intervals++;
        metrics->cursor_sample_interval_total_us += interval_us;
        if (interval_us > metrics->cursor_sample_interval_max_us)
            metrics->cursor_sample_interval_max_us = interval_us;
    }
    metrics->cursor_sample_previous_us = sampled_us;
    metrics->cursor_samples++;
    if (metrics->cursor_sample_pending_us == 0) {
        metrics->cursor_sample_pending_us = sampled_us;
    } else {
        /* Retain the oldest unpresented sample: that is the input age the
           user actually experiences when work coalesces multiple moves. */
        metrics->cursor_coalesced_samples++;
    }
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
    if (!published || frame == NULL) return;
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
#endif

static bool psp_media_frame_presentable(const MediaVideoFrame *frame)
{
    return frame != NULL && frame->pixels != NULL
        && frame->width > 0 && frame->height > 0
        && frame->stride_pixels >= frame->width;
}

/* The video frame replaces the page, so only the pixels it does not cover are
   cleared -- not all 261 KiB of scanout. Filled after the presenter has
   finished, which is what lets the GE draw without a scissor guarantee: an
   edge the rasterizer rounds outward is repaired here. */
static void psp_media_fill_bands(
    uint16_t *vram, const PspMediaPresentPlan *plan)
{
    for (size_t at = 0; at < plan->band_count; at++) {
        const PspMediaPresentRect *band = &plan->bands[at];
        for (int y = band->y; y < band->y + band->height; y++) {
            memset(vram + (size_t) y * PSP_VRAM_STRIDE + band->x, 0,
                   (size_t) band->width * sizeof(*vram));
        }
    }
}

/* The same bands, four bytes at a time, for the video surface. Black is zero
   in both formats, so this is the identical statement in a different width. */
static void psp_media_fill_bands_video(
    uint32_t *vram, const PspMediaPresentPlan *plan)
{
    for (size_t at = 0; at < plan->band_count; at++) {
        const PspMediaPresentRect *band = &plan->bands[at];
        for (int y = band->y; y < band->y + band->height; y++) {
            memset(vram + (size_t) y * PSP_VRAM_STRIDE + band->x, 0,
                   (size_t) band->width * sizeof(*vram));
        }
    }
}

/* The Sharp option, and the answer whenever the graphics engine cannot take
   the frame. Out of line because the shipping default no longer walks it. */
__attribute__((noinline))
static bool psp_media_present_software(
    const PspMediaPresentPlan *plan, const MediaVideoFrame *frame,
    uint16_t *vram)
{
    PspMediaScaleFormat format = frame->format == MEDIA_PIXEL_RGB565
        ? PSP_MEDIA_SCALE_RGB565 : PSP_MEDIA_SCALE_RGBA8888;
    if (!psp_media_scale_map_matches(
            &psp_media_scale_map, format, frame->width, frame->height,
            frame->stride_pixels, plan->video.width, plan->video.height)) {
        if (!psp_media_scale_map_build(
                &psp_media_scale_map, format, frame->width, frame->height,
                frame->stride_pixels,
                plan->video.width, plan->video.height)) {
            return false;
        }
        /* The pixel conversion this loop runs is named in assembly on
           Allegrex. Say once per geometry, on the device, that the toolchain
           assembled what was meant: a miscompiled ext or ins would otherwise
           reach a user as discoloured video that no host test can reproduce. */
        printf("tilefinch-media-scale: source=%dx%d stride=%d "
               "output=%dx%d left=%d top=%d format=%d identity-columns=%d "
               "conversion-verified=%d\n",
               frame->width, frame->height, frame->stride_pixels,
               plan->video.width, plan->video.height,
               plan->video.x, plan->video.y, (int) format,
               psp_media_scale_map.identity_columns ? 1 : 0,
               psp_media_scale_self_check() ? 1 : 0);
    }
    psp_media_scale_blit(
        &psp_media_scale_map, frame->pixels,
        vram + (size_t) plan->video.y * PSP_VRAM_STRIDE + plan->video.x,
        PSP_VRAM_STRIDE);
    return true;
}

/*
 * One line per opened stream, or per change of mind within one. The device
 * truth cycle reads this to learn which presenter actually ran: a session
 * that silently fell back to software would otherwise look exactly like one
 * that chose it, and the frame budget of the two is not the same.
 */
__attribute__((noinline))
static void psp_media_present_report(
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

/* Common bookkeeping for both presenters: cost accounting and the identity
   record, which are the same statements whatever the surface's width is. */
static void psp_media_present_account(
    PspMediaPresentRecord *record, const MediaVideoFrame *frame,
    const PspMediaPresentPlan *plan, uint64_t generation,
    uint64_t started_us, const PspMediaPresentGeCost *cost, bool drawn,
    bool chrome_paints)
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
    /* Only an overlay-free present may be remembered: some chrome surfaces
       blend with the picture and the time-dependent controls move, so a
       buffer they touched no longer holds the picture on its own. */
    if (chrome_paints) record->valid = false;
    else psp_media_present_record(
        record, frame->identity, generation, &plan->video,
        psp_display.back_buffer);
}

/* True when the plan and the identity record say this present may be skipped
   entirely, and the skip has been counted. */
static bool psp_media_present_prepare(
    const MediaVideoFrame *frame, bool chrome_paints,
    PspMediaPresentPlan *plan, PspMediaPresentRecord **record,
    uint64_t *generation)
{
    *generation =
        psp_active_media == NULL ? 0 : psp_active_media->generation;
    unsigned buffer_index =
        psp_display.back_buffer % PSP_DISPLAY_PAGE_BUFFER_COUNT;
    *record = &psp_media_present_records[buffer_index];
    if (!psp_media_present_skip_allowed(
            *record, frame->identity, *generation, &plan->video,
            buffer_index, chrome_paints)) {
        return false;
    }
    if (psp_active_media != NULL)
        psp_active_media->present_skipped_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_media_present_last_skipped = true;
#endif
    return true;
}

/* The Sharp option, and every present that reaches the 16-bit surface. The
   graphics engine is not consulted here at all: it draws into the 32-bit
   video surface and nowhere else. */
static bool psp_present_media_frame(
    uint16_t *vram, const MediaVideoFrame *frame, bool chrome_paints)
{
    if (vram == NULL || !psp_media_frame_presentable(frame)) return false;
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(
            &plan, frame->width, frame->height, frame->stride_pixels,
            PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT)) return false;
    uint64_t generation = 0;
    PspMediaPresentRecord *record = NULL;
    if (psp_media_present_prepare(
            frame, chrome_paints, &plan, &record, &generation)) return true;

    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    PspMediaPresentMode mode = psp_media_present_requested_mode();
    const char *reason = "setting";
    if (mode != PSP_MEDIA_PRESENT_MODE_SOFTWARE_SHARP) {
        /* Smooth was asked for and the video surface is not active, so the
           engine was refused or the surface could not be entered. */
        mode = PSP_MEDIA_PRESENT_MODE_SOFTWARE_FALLBACK;
        reason = psp_media_present_ge_reason();
        if (reason == NULL) reason = "no-video-surface";
    }
    /*
     * The scaler reads the decoder's surface directly for the whole of its
     * blit, so it is a reader like the staging copy and the direct draw, and
     * it claims its slot the same way. It had no claim at all before two slots
     * existed, which was survivable only because there was one surface and one
     * writer to hold off; now the claim also says WHICH picture is being read,
     * and a refusal means the pipeline moved on and this frame has nothing
     * left to draw.
     */
    if (frame->slot >= 0
        && (psp_active_media == NULL
            || !media_playback_borrow_video_slot(
                psp_active_media->playback, (unsigned) frame->slot,
                frame->generation))) return false;
    bool scaled = psp_media_present_software(&plan, frame, vram);
    if (frame->slot >= 0)
        media_playback_release_video_read(
            psp_active_media->playback, (unsigned) frame->slot);
    if (!scaled) return false;
    psp_media_fill_bands(vram, &plan);
    psp_media_present_report(mode, reason, frame, &plan, NULL);
    PspMediaPresentGeCost cost = {0, 0, 0};
    psp_media_present_account(
        record, frame, &plan, generation, started_us, &cost, false,
        chrome_paints);
    media_playback_note_frame_displayed(
        psp_active_media->playback, frame,
        MEDIA_PSP_PRESENT_PATH_SOFTWARE);
    return true;
}

static bool psp_media_complete_wide_pass(PspMediaPresentGeCost *cost);

/*
 * The experimental 360p Smooth presenter.
 *
 * The full 768x360 padded picture cannot fit beside both 8888 scanout buffers
 * in EDRAM. Two 181-row copies can, with one guard row shared across their
 * seam. Each copy is contiguous at the firmware's 768-pixel stride, so the
 * same proven DMA call as 240p moves it without a CPU row-pack. The source
 * slot stays borrowed until both copies have completed; the stage is then a
 * private EDRAM texture while each GE list runs.
 *
 * This helper is reached only when psp_media_present_wide_strip_plan accepts
 * the 638..640x360/768 geometry. The 240p presenter above and the retained
 * whole-picture stage below remain untouched.
 */
static bool psp_present_media_frame_video_strips(
    uint32_t *vram, const MediaVideoFrame *frame,
    const PspMediaPresentPlan *full,
    const PspMediaPresentStripPlan *strips,
    PspMediaPresentRecord *record, uint64_t generation,
    uint64_t started_us, bool chrome_paints,
    PspMediaPresentGeCost *cost)
{
    if (vram == NULL || frame == NULL || frame->pixels == NULL
        || full == NULL || strips == NULL || cost == NULL
        || strips->strip_count != PSP_MEDIA_PRESENT_WIDE_STRIPS
        || psp_active_media == NULL || frame->slot < 0)
        return false;
    uint32_t *staging = psp_display_video_texture(&psp_display);
    if (staging == NULL
        || psp_media_present_ge_stage_dma_quarantine_holds_staging(staging)
        || !media_playback_borrow_video_slot(
               psp_active_media->playback, (unsigned) frame->slot,
               frame->generation))
        return false;

    bool succeeded = false;
    uint64_t stage_total_us = 0;
    PspMediaPresentGeCost total = {0, 0, 0};
    psp_active_media->present_texture_staged = true;
    for (size_t at = 0; at < strips->strip_count; at++) {
        const PspMediaPresentStrip *strip = &strips->strips[at];
        const uint32_t *source = (const uint32_t *) frame->pixels
            + (size_t) strip->source_row * (size_t) frame->stride_pixels;
        size_t bytes = (size_t) frame->stride_pixels
            * (size_t) strip->copy_rows * sizeof(*source);
        if (bytes > PSP_DISPLAY_VIDEO_TEXTURE_BYTES) goto finished;

        uint64_t copy_started_us = (uint64_t) sceKernelGetSystemTimeWide();
        if (!psp_media_present_ge_stage_dma(staging, source, bytes)) {
            psp_media_present_stage(
                staging, source, frame->stride_pixels,
                frame->stride_pixels, strip->copy_rows);
            psp_media_present_ge_stage_flush(staging, bytes);
        }
        stage_total_us +=
            (uint64_t) sceKernelGetSystemTimeWide() - copy_started_us;

        PspMediaPresentTexture texture = {
            .pixels = staging,
            .stride_pixels = frame->stride_pixels,
            .staged = true
        };
        PspMediaPresentGeCost pass = {0, 0, 0};
        if (!psp_media_present_ge_submit(
                &strip->draw, &texture, vram, &pass))
            goto finished;
        (void) psp_media_pump_while_drawing(psp_active_media);
        if (!psp_media_complete_wide_pass(&pass)) goto finished;
        total.submit_us += pass.submit_us;
        total.sync_us += pass.sync_us;
        total.wait_us += pass.wait_us;
    }
    succeeded = true;

finished:
    psp_active_media->present_texture_staged = false;
    media_playback_release_video_read(
        psp_active_media->playback, (unsigned) frame->slot);
    if (!succeeded) return false;

    psp_active_media->present_stage_frames++;
    psp_active_media->present_stage_total_us += stage_total_us;
    if (stage_total_us > psp_active_media->present_stage_max_us)
        psp_active_media->present_stage_max_us = stage_total_us;
    psp_media_fill_bands_video(vram, full);
    PspMediaPresentTexture report_texture = {
        .pixels = staging,
        .stride_pixels = frame->stride_pixels,
        .staged = true
    };
    psp_media_present_report(
        PSP_MEDIA_PRESENT_MODE_GE_SMOOTH, "wide-edram-strips",
        frame, full, &report_texture);
    *cost = total;
    psp_media_present_account(
        record, frame, full, generation, started_us, cost, true,
        chrome_paints);
    media_playback_note_frame_displayed(
        psp_active_media->playback, frame,
        MEDIA_PSP_PRESENT_PATH_GE_STRIPS);
    return true;
}

/*
 * The Smooth default: the graphics engine draws the quad into the 32-bit
 * video buffer and the chrome is expanded over the rows it covers.
 *
 * Out of line, and reached only while the panel is already on the video
 * surface. A false return means the engine refused after the surface was
 * entered -- the caller leaves the surface and the next present is the
 * software one above, which costs one dropped frame at the moment a piece of
 * hardware failed for the first time.
 */
__attribute__((noinline))
static bool psp_present_media_frame_video(
    uint32_t *vram, const MediaVideoFrame *frame, bool chrome_paints,
    PspMediaPresentGeCost *cost)
{
    if (vram == NULL || !psp_media_frame_presentable(frame)) return false;
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(
            &plan, frame->width, frame->height, frame->stride_pixels,
            PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT)) return false;
    if (frame->format == MEDIA_PIXEL_RGB565 || plan.quad_count == 0) {
        /* The engine would take a 16-bit texture and this surface would not
           pass its bytes through, and a geometry it cannot express has no
           quads. Both are answers the 16-bit path must give. */
        return false;
    }
    uint64_t generation = 0;
    PspMediaPresentRecord *record = NULL;
    if (psp_media_present_prepare(
            frame, chrome_paints, &plan, &record, &generation)) return true;

    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    PspMediaPresentStripPlan strips;
    if (psp_media_present_wide_strip_plan(
            &strips, &plan, frame->width, frame->height,
            frame->stride_pixels, PSP_DISPLAY_VIDEO_TEXTURE_BYTES)) {
        return psp_present_media_frame_video_strips(
            vram, frame, &plan, &strips, record, generation,
            started_us, chrome_paints, cost);
    }
    /*
     * Submit, feed, then wait.
     *
     * The device measured this draw at 46ms a frame against the software
     * scaler's 6-10ms, and almost all of it is the wait: the list is
     * microseconds. Blocking straight through it spent 6.45 seconds of a
     * thirty-second session giving the decoder nothing, while its own feed
     * report said horizon=0 and unit-cap=54 -- ready to submit, out of frames
     * to do it in. The pump touches packets, the codec worker and the
     * transport, never these rows, so it is exactly what belongs here.
     */
    PspMediaPresentTexture texture;
    psp_media_present_texture_for(
        psp_active_media, &plan, frame,
        psp_display_video_texture(&psp_display),
        PSP_DISPLAY_VIDEO_TEXTURE_BYTES, &texture);
    /* If that posted the stage copy to the DMA worker, feed the decoder while
       the controller runs it and collect it here -- before the list is
       submitted, so the engine never samples an unfinished texture. */
    if (psp_active_media != NULL)
        psp_media_present_texture_finish(psp_active_media);
    if (!psp_media_present_ge_submit(&plan, &texture, vram, cost))
        return false;
    if (psp_active_media != NULL)
        (void) psp_media_pump_while_drawing(psp_active_media);
    if (!psp_media_present_ge_complete(cost)) return false;
    /* The engine is done with the rows, so it is done with whatever it read
       them from -- which, for a picture that could not be staged, is the
       decoder's own surface. Both failure returns above deliberately keep the
       claim: the caller answers them with the software scaler, which reads
       that same surface, and the next frame's advance releases it. */
    if (psp_active_media != NULL)
        psp_media_present_texture_release(psp_active_media);
    psp_media_fill_bands_video(vram, &plan);
    psp_media_present_report(
        PSP_MEDIA_PRESENT_MODE_GE_SMOOTH, "setting", frame, &plan, &texture);
    psp_media_present_account(
        record, frame, &plan, generation, started_us, cost, true,
        chrome_paints);
    /* The picture is on the screen. This is the only milestone that says so:
       everything upstream reports ownership changing hands. */
    if (psp_active_media != NULL)
        media_playback_note_frame_displayed(
        psp_active_media->playback, frame,
        texture.staged ? MEDIA_PSP_PRESENT_PATH_GE_STAGED
                       : MEDIA_PSP_PRESENT_PATH_GE_DIRECT);
    return true;
}

/* Kept after the ordinary presenter so the structural gate can continue to
   prove that its async 240p stage is joined before that present's first GE
   submission. The wide helper above reaches the same completion fence through
   this tiny wrapper. */
static bool psp_media_complete_wide_pass(PspMediaPresentGeCost *cost)
{
    return psp_media_present_ge_complete(cost);
}

/*
 * Whether the panel should be on the 32-bit video surface, and the transition
 * when the answer changes.
 *
 * The ownership window is exactly "a decoded video frame is about to fill the
 * screen, and Smooth is what will draw it". Everything else -- the black
 * loading stage before the first frame, the page, the boot surfaces, Sharp,
 * and a graphics engine that has been latched off -- is the 16-bit surface.
 * Deriving it per present rather than latching it at open is what makes every
 * exit path an exit path: close, the failed panel's dismissal, a quarantine, a
 * navigation away and a system suspend all stop producing decoded frames, and
 * the very next present puts the panel back.
 *
 * Entering publishes one visually equivalent 8888 copy of the loading frame
 * before decoded pixels are admitted. A session that never opens video never
 * pays for that format bridge and the boot counters cannot move.
 */
static bool psp_media_video_surface_follow(bool video_owns_screen)
{
    bool want = video_owns_screen
        && psp_media_present_requested_mode()
             == PSP_MEDIA_PRESENT_MODE_GE_SMOOTH
        /* NULL while the engine has not failed. A latched-off engine must not
           be given the panel and then hand it straight back. */
        && psp_media_present_ge_reason() == NULL;
    if (want == psp_display_video_active(&psp_display)) return want;
    if (!want) {
        (void) psp_display_video_end(&psp_display);
        psp_media_present_forget_buffers();
        return false;
    }
    if (!psp_display_video_begin(&psp_display)) return false;
    /*
     * The format bridge has already latched the expanded loading UI. Prove the
     * decoded-pixel passthrough in the other, still-hidden video buffer before
     * the first actual video publish.
     */
    if (!psp_media_present_ge_passthrough_check(
            psp_display_video_back_buffer(&psp_display))) {
        (void) psp_display_video_end(&psp_display);
        return false;
    }
    psp_media_present_forget_buffers();
    return true;
}

static bool psp_media_track_menu_blit(
    void *context, const uint16_t *source, int source_width,
    int source_height, int source_stride, bool source_changed,
    uint32_t *destination, int destination_stride, int left, int top)
{
    (void) context;
    PspMediaPresentGeCost cost = {0, 0, 0};
    bool blitted = psp_media_present_ge_blit_565(
        source, source_width, source_height, source_stride, source_changed,
        destination, destination_stride, left, top, &cost);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
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
#else
    (void) cost;
#endif
    return blitted;
}

/* One video frame plus its chrome, published. Out of line: the 16-bit
   compositor is what every non-video frame walks. */
__attribute__((noinline))
static bool psp_present_video_surface(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t presentation_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
#endif
    uint32_t *vram = psp_display_video_back_buffer(&psp_display);
    PspMediaPresentGeCost cost = {0, 0, 0};
    bool overlay_paints =
        psp_ui_media_overlay_paints(&psp_active_media->ui);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_media_present_last_skipped = false;
#endif
    if (vram == NULL
        || !psp_present_media_frame_video(
               vram, &psp_active_media->frame,
               overlay_paints, &cost)) {
        /* The engine refused after the surface was entered. Hand the panel
           back and let the next present run the software scaler; publishing a
           buffer nothing drew into would be a frame of noise. */
        (void) psp_display_video_end(&psp_display);
        psp_media_present_forget_buffers();
        return false;
    }
    PspUiMediaPreview preview = {
        .pixels = psp_active_media->seek_preview_pixels,
        .width = PSP_MEDIA_PREVIEW_WIDTH,
        .height = PSP_MEDIA_PREVIEW_HEIGHT,
        .stride = PSP_MEDIA_PREVIEW_WIDTH
    };
    psp_ui_media_composite_8888_cached(
        &psp_active_media->ui,
        psp_active_media->ui.seek_preview_active ? &preview : NULL,
        vram, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, PSP_VRAM_STRIDE,
        psp_display_video_overlay_scratch(&psp_display),
        psp_display_video_aux(&psp_display),
        psp_display_video_aux_pixels(&psp_display),
        psp_display_edram_content_epoch(), &psp_media_track_menu_cache,
        psp_media_track_menu_blit, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_cadence_composed(presentation_started_us, false);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    unsigned published_index = psp_display.back_buffer;
#endif
    bool published = psp_display_publish(&psp_display);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_cadence_published(published);
    psp_cadence_video_published(published, &psp_active_media->frame);
    if (published) {
        psp_input_script_capture_media_present(
            &psp_active_media->ui, NULL, vram, PSP_VRAM_STRIDE,
            true, published_index, psp_media_present_last_skipped, false,
            &psp_active_media->frame, psp_media_present_records,
            PSP_DISPLAY_PAGE_BUFFER_COUNT);
    }
#endif
    return published;
}

static bool psp_media_seek_holds_scanout(const PspMediaSession *media)
{
    if (media == NULL || media->job_resume_open) return false;
    switch (media->job_phase) {
    case PSP_MEDIA_JOB_SEEK_PREPARE:
    case PSP_MEDIA_JOB_SEEK_PRIME:
    case PSP_MEDIA_JOB_SEEK_DECODE:
    case PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE:
    case PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE:
        return true;
    default:
        return false;
    }
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* One outstanding input measurement, no DOM retention. The engine is the
   process-owned facade; a navigation generation change abandons the sample.
   In particular a supervisor publishing the old framebuffer is not success. */
static struct {
    BrowserEngine *engine;
    uint64_t generation, started_us, shell_serial;
    size_t rendered_frames;
    int action;
} psp_focus_feedback;

void psp_focus_feedback_begin(BrowserEngine *engine, int action, uint64_t started_us)
{
    if (engine == NULL) {
        psp_focus_feedback.engine = NULL;
        return;
    }
    const NavigationSession *navigation = browser_engine_navigation(engine);
    const TileCache *render = browser_engine_render_metrics_view(engine);
    if (navigation == NULL || render == NULL) return;
    if (psp_focus_feedback.engine != NULL)
        printf("tilefinch-focus-feedback-incomplete: action=%d reason=superseded\n",
               psp_focus_feedback.action);
    psp_focus_feedback.engine = engine;
    psp_focus_feedback.generation = navigation->generation;
    psp_focus_feedback.started_us = started_us;
    psp_focus_feedback.rendered_frames = render->frames_rendered;
    psp_focus_feedback.shell_serial = browser_engine_render_shell_serial(engine);
    psp_focus_feedback.action = action;
}

static void psp_focus_feedback_published(const uint16_t *frame, const PspUiState *ui)
{
    BrowserEngine *engine = psp_focus_feedback.engine;
    if (engine == NULL) return;
    const NavigationSession *navigation = browser_engine_navigation(engine);
    if (navigation == NULL || !navigation->page.loaded
        || navigation->generation != psp_focus_feedback.generation) {
        printf("tilefinch-focus-feedback-incomplete: action=%d reason=navigation\n",
               psp_focus_feedback.action);
        psp_focus_feedback.engine = NULL;
        return;
    }
    const TileCache *render = browser_engine_render_metrics_view(engine);
    if (ui == NULL || ui->screen != PSP_UI_SCREEN_PAGE || render == NULL
        || render->frames_rendered == 0
        || (browser_engine_render_shell_serial(engine) == psp_focus_feedback.shell_serial
            && render->frames_rendered <= psp_focus_feedback.rendered_frames)
        || browser_engine_render_frame_pending(engine)
        || frame != browser_engine_framebuffer(engine, NULL)) return;
    printf("tilefinch-focus-feedback: action=%d shown=1 visible=%u elapsed=%lluus "
           "rect=%d,%d,%d,%d relayouts=%zu layout-total=%lluus "
           "fixed=%zu/%zu/%zu fixed-us=%lluus tile-us=%lluus\n",
           psp_focus_feedback.action, ui->has_focus ? 1u : 0u,
           (unsigned long long) (sceKernelGetSystemTimeWide() - psp_focus_feedback.started_us),
           ui->focus_x, ui->focus_y, ui->focus_width, ui->focus_height,
           navigation->incremental_relayouts,
           (unsigned long long) navigation->performance.layout_us,
           navigation->page.layout.fixed_count, render->fixed_cache_builds,
           render->fixed_cache_blits,
           (unsigned long long) render->frame_fixed_us,
           (unsigned long long) render->frame_tile_us);
    psp_focus_feedback.engine = NULL;
}
#endif

bool psp_present_internal(
    const uint16_t *frame, const PspUiState *ui, bool include_media)
{
    if (frame == NULL
        || !psp_lifecycle_presentation_allowed(&psp_lifecycle))
        return false;
    bool media_visible = include_media && psp_active_media != NULL
        && psp_active_media->ui.visible;
    /* A seek has to release the decoded surface before firmware may write its
       replacement.  That is not permission to alternate the LCD between the
       32-bit video buffers and a freshly-cleared 16-bit buffer while the job
       advances.  Leave the last complete front buffer latched until the seek
       (including preview restore) reaches one stable frame.  No decoder-owned
       memory is retained or sampled here; the display controller simply keeps
       scanning the frame that was already published.  Resume-open uses the
       same job phases before any video frame exists and therefore remains on
       the ordinary loading surface. */
    if (media_visible && psp_media_seek_holds_scanout(psp_active_media))
        return false;
    bool media_replaces_page = media_visible && psp_active_media->have_frame
        && psp_media_frame_presentable(&psp_active_media->frame);
    if (psp_media_video_surface_follow(media_replaces_page)) {
        bool published = psp_present_video_surface();
        /* Still on the video surface means it owned this frame, published or
           refused by the display service. Handed back means the engine
           refused after entry: fall through and let the software presenter
           draw the same frame into the 16-bit surface. */
        if (psp_display_video_active(&psp_display)) return published;
    }
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (vram == NULL) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t presentation_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t base_finished_us = presentation_started_us;
    uint64_t composite_finished_us = presentation_started_us;
#endif
    bool native_surface = !media_visible && ui != NULL
        && psp_ui_screen_is_native_surface(ui->screen);
    if (!media_replaces_page) {
        /* Something other than the video presenter is about to own these
           pixels, so nothing may be skipped against what either buffer used
           to hold. Native surfaces write the whole panel through their own
           path and count for the same reason. */
        psp_media_present_forget_buffers();
    }
    if (media_visible && !media_replaces_page) {
        /* A native player owns the whole display as soon as it opens. Until a
           decoded frame exists, present a stable black stage behind its
           loading controls rather than exposing the unrelated web page. */
        for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
            memset(vram + (size_t) y * PSP_VRAM_STRIDE, 0,
                   PSP_SCREEN_WIDTH * sizeof(*vram));
        }
    } else if (!media_replaces_page && !native_surface) {
        unsigned opaque_top = 0u, opaque_bottom = 0u;
        psp_ui_opaque_chrome_rows(ui, &opaque_top, &opaque_bottom);
        unsigned copy_end = PSP_SCREEN_HEIGHT - opaque_bottom;
        for (unsigned y = opaque_top; y < copy_end; y++) {
            memcpy(vram + (size_t) y * PSP_VRAM_STRIDE,
                   frame + (size_t) y * PSP_SCREEN_WIDTH,
                   PSP_SCREEN_WIDTH * sizeof(*frame));
        }
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    base_finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    PspUiCompositeTiming ui_timing = {0};
#endif
    if (media_visible) {
        if (media_replaces_page) {
            bool overlay_paints =
                psp_ui_media_overlay_paints(&psp_active_media->ui);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_media_present_last_skipped = false;
#endif
            (void) psp_present_media_frame(
                vram, &psp_active_media->frame,
                overlay_paints);
        }
        PspUiMediaPreview preview = {
            .pixels = psp_active_media->seek_preview_pixels,
            .width = PSP_MEDIA_PREVIEW_WIDTH,
            .height = PSP_MEDIA_PREVIEW_HEIGHT,
            .stride = PSP_MEDIA_PREVIEW_WIDTH
        };
        psp_ui_media_composite_with_preview(
            &psp_active_media->ui,
            psp_active_media->ui.seek_preview_active ? &preview : NULL,
            vram,
            PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, PSP_VRAM_STRIDE);
    } else if (ui != NULL) {
        psp_ui_composite(ui, vram, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
                         PSP_VRAM_STRIDE);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        (void) psp_ui_validation_last_timing(&ui_timing);
#endif
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    composite_finished_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    /* Publish only a completely composed back buffer at vblank. Writing the
       scanout buffer directly caused visibly half-updated title bars and
       loading flicker on physical hardware. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_cadence_composed(presentation_started_us, native_surface);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    unsigned published_index = psp_display.back_buffer;
    PspDisplayBackendTiming display_timing_before = {0};
    (void) psp_display_validation_timing_snapshot(&display_timing_before);
#endif
    bool published = psp_display_publish(&psp_display);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t published_us = (uint64_t) sceKernelGetSystemTimeWide();
    PspDisplayBackendTiming display_timing_after = {0};
    (void) psp_display_validation_timing_snapshot(&display_timing_after);
    uint32_t sequence = psp_present_last_timing.sequence + 1u;
    if (sequence == 0) sequence = 1u;
    psp_present_last_timing = (PspPresentPhaseTiming) {
        .base_us = base_finished_us - presentation_started_us,
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
    psp_cadence_published(published);
    if (published && !media_visible) psp_focus_feedback_published(frame, ui);
    if (published && media_visible) {
        psp_input_script_capture_media_present(
            &psp_active_media->ui, vram, NULL, PSP_VRAM_STRIDE,
            false, published_index,
            media_replaces_page && psp_media_present_last_skipped,
            false, media_replaces_page ? &psp_active_media->frame : NULL,
            psp_media_present_records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    }
#endif
    return published;
}

static unsigned psp_local_hour(void)
{
    ScePspDateTime clock = {0};
    int result = sceRtcGetCurrentClockLocalTime(&clock);
    if (result < 0 || clock.hour >= 24u) {
        printf("tilefinch-appearance: local-clock-error=%d\n", result);
        return 12u;
    }
    return clock.hour;
}

/*
 * The user's 12/24-hour preference. It is a system setting that cannot
 * change while the application owns the screen, so read it once and cache
 * the answer rather than paying a utility call every status refresh. A
 * failed read falls back to 24-hour, which is also the SDK default.
 */
static bool psp_clock_is_twelve_hour(void)
{
    static int cached = -1;
    if (cached < 0) {
        int format = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
        int result = sceUtilityGetSystemParamInt(
            PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT, &format);
        if (result < 0) {
            printf("tilefinch-appearance: time-format-error=%d\n", result);
            format = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
        }
        cached = format == PSP_SYSTEMPARAM_TIME_FORMAT_12HR ? 1 : 0;
    }
    return cached == 1;
}

/*
 * Wifi strength for the status line. Unlike the battery -- a bare
 * scePowerGetBatteryLifePercent() the refresh can call from anywhere --
 * the strength read runs ~a dozen sceNetApctlGetInfo calls and only
 * answers while the link is READY. It is far too costly to pay on every
 * status refresh, so the main loop samples it on its own slow cadence and
 * caches the bar count here; the refresh below just forwards the cache.
 * A negative cache means "no signal to show" (not READY, or, outside a
 * live-network build, always), which the status line renders as absent.
 */
#define PSP_WIFI_SAMPLE_INTERVAL_US UINT64_C(4000000)
static int psp_wifi_bars_cache = -1;

static int psp_wifi_current_bars(void)
{
    return psp_wifi_bars_cache;
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
/* Map the 0..100 signal percent onto 0..4 filled bars. */
static int psp_wifi_bars_from_strength(unsigned strength)
{
    if (strength >= 76u) return 4;
    if (strength >= 51u) return 3;
    if (strength >= 26u) return 2;
    if (strength >= 1u) return 1;
    return 0;
}

void psp_sample_wifi_strength(
    const PspNetwork *network, uint64_t now_us)
{
    static uint64_t next_sample_us = 0;
    if (network == NULL || network->status != PSP_NETWORK_READY) {
        psp_wifi_bars_cache = -1;
        next_sample_us = 0;
        return;
    }
    if (psp_wifi_bars_cache >= 0 && now_us < next_sample_us) return;
    next_sample_us = now_us + PSP_WIFI_SAMPLE_INTERVAL_US;
    PspNetworkInterfaceReport report;
    if (psp_network_interface_report(network, &report))
        psp_wifi_bars_cache =
            psp_wifi_bars_from_strength(report.signal_strength);
    else
        psp_wifi_bars_cache = -1;
}
#endif

void psp_refresh_page_color_mode(PspUiState *ui)
{
    if (ui == NULL) return;
    ScePspDateTime clock = {0};
    int clock_result = sceRtcGetCurrentClockLocalTime(&clock);
    if (clock_result >= 0)
        psp_ui_set_device_status(
            clock.hour, clock.minute,
            scePowerGetBatteryLifePercent(),
            scePowerIsBatteryCharging() > 0,
            psp_clock_is_twelve_hour(),
            psp_wifi_current_bars());
    ui->page_dark =
        psp_ui_color_mode_is_dark(ui->color_mode, psp_local_hour());
}

bool psp_request_policy_clock(
    void *context, unsigned cpu_mhz, unsigned bus_mhz,
    uint64_t *request_us)
{
    PspClockWorker *worker = context;
    bool idle =
        cpu_mhz == PSP_POWER_POLICY_IDLE_CPU_MHZ
        && bus_mhz == PSP_POWER_POLICY_IDLE_BUS_MHZ;
    bool high =
        cpu_mhz == PSP_POWER_POLICY_HIGH_CPU_MHZ
        && bus_mhz == PSP_POWER_POLICY_HIGH_BUS_MHZ;
    return (idle || high)
        && psp_clock_worker_request(worker, idle, request_us);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
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
#endif

bool psp_present(const uint16_t *frame, const PspUiState *ui)
{
    return psp_present_internal(frame, ui, true);
}

bool psp_present_cursor_feedback(
    const uint16_t *frame, const PspUiState *ui)
{
    return psp_present_internal(frame, ui, true);
}

bool psp_present_supervisor_ui(const uint16_t *frame,
                                      const PspUiState *ui)
{
    /*
     * The cooperative scope publishes an immutable authoritative page frame.
     * Recompose chrome from that clean base every time: copying the previous
     * scanout would copy already-composited translucent glyph/indicator
     * pixels and then blend the next status bar over them, which accumulates
     * lines and flicker during rapid input.
     */
    if (frame == NULL || ui == NULL) return false;
    return psp_present_internal(frame, ui, false);
}

static void psp_present_supervisor_media(const PspUiMediaState *media)
{
    if (media == NULL
        || !psp_lifecycle_presentation_allowed(&psp_lifecycle)) return;
    /* The supervisor may write either scanout format without presenting a
       decoded picture. Forget both buffer identities before touching either
       path, including attempts whose later publication fails, so an ordinary
       presenter can never skip against supervisor chrome or black pixels.
       This callback runs inside the same cooperative presentation scope as
       the ordinary presenter, preserving record-table serialization. */
    psp_media_present_forget_buffers();
    if (psp_display_video_active(&psp_display)) {
        /* A cooperative preview seek must not switch the panel from the
           32-bit video surface to a separately-cleared 16-bit surface merely
           to acknowledge input. That format/latch round trip was visible on
           hardware as a flash across the lower player chrome. Freeze the
           last complete front frame into the free buffer and overwrite only
           the scrubber's opaque rows; the decoder surface is neither sampled
           nor retained here. Copying the already-composited title/badge is
           safe because this path deliberately does not redraw them. */
        uint32_t *front = psp_display_video_front_buffer(&psp_display);
        uint32_t *back = psp_display_video_back_buffer(&psp_display);
        uint16_t *scratch =
            psp_display_video_overlay_scratch(&psp_display);
        if (front != NULL && back != NULL && scratch != NULL) {
            memcpy(back, front, PSP_DISPLAY_VIDEO_BUFFER_BYTES);
            psp_ui_media_composite_controls_8888(
                media, back, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
                PSP_VRAM_STRIDE, scratch);
            unsigned published_index = psp_display.back_buffer;
            bool published = psp_display_publish(&psp_display);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (published) {
                psp_input_script_capture_media_present(
                    media, NULL, back, PSP_VRAM_STRIDE, true,
                    published_index, false, true,
                    psp_active_media != NULL && psp_active_media->have_frame
                        ? &psp_active_media->frame : NULL,
                    psp_media_present_records,
                    PSP_DISPLAY_PAGE_BUFFER_COUNT);
            }
#else
            (void) published_index;
            (void) published;
#endif
            return;
        }
    }
    /* This composes 16-bit rows. Every such site asserts the surface it is
       about to write rather than assuming the last present left it there --
       the supervisor runs on the callback thread and cannot know. */
    (void) psp_display_video_end(&psp_display);
    uint16_t *front = psp_display_front_buffer(&psp_display);
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (front == NULL || vram == NULL) return;
    /* The 16-bit software-decoder surface needs the same immutable-picture
       rule as the 32-bit surface above. Clearing this buffer and drawing all
       snapshot chrome briefly published black video; copying the last
       complete scanout preserves the picture and any current caption while
       the cooperative unit updates only the input-acknowledgement controls.
       Before a first picture, however, the 565 surface is a loading stage:
       its central progress panel must advance under supervisor ownership. */
    memcpy(vram, front,
           PSP_DISPLAY_BUFFER_PIXELS * sizeof(*vram));
    psp_ui_media_composite_supervisor_565(
        media, vram, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
        PSP_VRAM_STRIDE);
    unsigned published_index = psp_display.back_buffer;
    bool published = psp_display_publish(&psp_display);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (published) {
        psp_input_script_capture_media_present(
            media, vram, NULL, PSP_VRAM_STRIDE, false,
            published_index, false, true,
            psp_active_media != NULL && psp_active_media->have_frame
                ? &psp_active_media->frame : NULL,
            psp_media_present_records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    }
#else
    (void) published_index;
    (void) published;
#endif
}

void psp_present_boot_surface(
    PspUiStartupView view, const char *status, int progress_per_mille)
{
    /* Keep pre-engine presentation entirely in VRAM.  A second 480x272
       framebuffer would consume scarce main memory before the page budget is
       even admitted. Two scanout surfaces fit in VRAM and eliminate tearing
       without consuming the page budget. */
    (void) psp_display_video_end(&psp_display);
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (vram == NULL) return;
    /*
     * Only the completed back buffer can become visible. The old startup
     * path cleared the second scanout surface too, doubling the work before
     * the first publish for pixels that the next ordinary present overwrites
     * in full.
     */
    for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
        uint16_t *row = vram + (size_t) y * PSP_VRAM_STRIDE;
        for (int x = 0; x < PSP_VRAM_STRIDE; x++) {
            row[x] = PSP_STARTUP_BACKGROUND;
        }
    }
    psp_ui_startup_composite(
        view, status, progress_per_mille, vram,
        PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, PSP_VRAM_STRIDE);
    (void) psp_display_publish(&psp_display);
}


PspNavigationCooperate psp_navigation_cooperate;
static PspUiMediaIntent psp_completed_supervisor_media_intent;
enum { PSP_SUPERVISOR_PAGE_INPUT_LIMIT = 4 };
static uint32_t psp_completed_supervisor_page_input[
    PSP_SUPERVISOR_PAGE_INPUT_LIMIT];
static uint8_t psp_completed_supervisor_page_input_count;
volatile unsigned psp_validation_cancel_after_ms;
volatile unsigned psp_validation_preview_scroll;

bool psp_platform_present(
    void *context, const uint16_t *pixels, size_t width,
    size_t height, size_t stride_pixels)
{
    PspNavigationCooperate *cooperate = context;
    if (cooperate == NULL || cooperate->active == 0
        || cooperate->ui == NULL || pixels == NULL
        || width != PSP_SCREEN_WIDTH || height != PSP_SCREEN_HEIGHT
        || stride_pixels != PSP_SCREEN_WIDTH) {
        return false;
    }
    if (!__sync_bool_compare_and_swap(
            &cooperate->presenting, 0u, 1u)) {
        if (cooperate->engine != NULL)
            cooperate->provisional_present_requested = 1;
        return false;
    }
    __sync_synchronize();
    if (cooperate->active == 0) {
        cooperate->presenting = 0;
        return false;
    }
    /* Report the scanout outcome rather than the attempt: a present the
       display service refused is not a frame the user saw. */
    bool shown = psp_present_internal(pixels, cooperate->ui, false);
    cooperate->last_present_us = sceKernelGetSystemTimeWide();
    if (shown) cooperate->presentations++;
    cooperate->provisional_present_requested = 0;
    __sync_synchronize();
    cooperate->presenting = 0;
    return shown;
}

static void psp_work_cooperate_begin_internal(
    PspUiState *ui, const uint16_t *frame,
    bool periodic_present, bool acknowledge_non_cancel_busy,
    bool log_session, const char *cancel_status, const char *phase,
    BrowserEngine *engine, const PspUiMediaState *media_ui,
    bool owner_thread_only)
{
    /* Publish an immutable page-frame pointer and a private UI copy before
       making the job visible to the callback-thread supervisor. The engine
       does not paint this framebuffer while candidate navigation is active. */
    psp_navigation_cooperate.ui = ui;
    if (ui != NULL) {
        psp_navigation_cooperate.supervisor_ui = *ui;
        psp_navigation_cooperate.original_screen = ui->screen;
        psp_navigation_cooperate.original_chrome_visible = ui->chrome_visible;
    }
    psp_navigation_cooperate.input_yield = false;
    psp_navigation_cooperate.media_surface = media_ui != NULL;
    psp_navigation_cooperate.media_detached = false;
    if (media_ui != NULL)
        psp_navigation_cooperate.supervisor_media_ui = *media_ui;
    psp_navigation_cooperate.engine = engine;
    psp_navigation_cooperate.frame = frame;
    psp_navigation_cooperate.last_present_us = 0;
    psp_navigation_cooperate.started_us =
        sceKernelGetSystemTimeWide();
    psp_navigation_cooperate.last_checkpoint_us =
        sceKernelGetSystemTimeWide();
    psp_navigation_cooperate.maximum_checkpoint_gap_us = 0;
    psp_navigation_cooperate.maximum_checkpoint_gap_phase =
        phase == NULL ? "work-begin" : phase;
    psp_navigation_cooperate.last_phase =
        phase == NULL ? "work-begin" : phase;
    psp_navigation_cooperate.last_completed_work_units = 0;
    psp_navigation_cooperate.checkpoint_calls = 0;
    psp_navigation_cooperate.presentations = 0;
    psp_navigation_cooperate.input_acknowledgements = 0;
    SceCtrlData initial_pad = {0};
    uint32_t initial_buttons =
        sceCtrlPeekBufferPositive(&initial_pad, 1) > 0
            ? initial_pad.Buttons : 0;
    psp_navigation_cooperate.supervisor_previous_buttons =
        initial_buttons;
    psp_navigation_cooperate.fallback_previous_buttons =
        initial_buttons;
    psp_navigation_cooperate.cancel_status =
        cancel_status == NULL
            ? "STOPPING..." : cancel_status;
    psp_navigation_cooperate.periodic_present = periodic_present;
    psp_navigation_cooperate.owner_thread_only = owner_thread_only;
    psp_navigation_cooperate.acknowledge_non_cancel_busy =
        acknowledge_non_cancel_busy;
    psp_navigation_cooperate.log_session = log_session;
    psp_navigation_cooperate.supervised = atomic_load_explicit(
        &psp_background_ui_available, memory_order_acquire) != 0
            ? 1u : 0u;
    psp_navigation_cooperate.presenting = 0;
    psp_navigation_cooperate.provisional_present_requested = 0;
    psp_navigation_cooperate.provisional_scroll_requests = 0;
    psp_navigation_cooperate.pending_page_input_count = 0;
    psp_navigation_cooperate.pending_page_input_dropped = 0;
    psp_navigation_cooperate.pending_media_intent =
        (PspUiMediaIntent) {0};
    tilefinch_cancellation_init(&psp_navigation_cooperate.cancellation);
    __sync_synchronize();
    psp_navigation_cooperate.active =
        ui != NULL && frame != NULL ? 1u : 0u;
}

void psp_work_cooperate_begin(
    PspUiState *ui, const uint16_t *frame,
    bool periodic_present, bool acknowledge_non_cancel_busy,
    bool log_session, const char *cancel_status, const char *phase,
    BrowserEngine *engine, const PspUiMediaState *media_ui)
{
    psp_work_cooperate_begin_internal(
        ui, frame, periodic_present, acknowledge_non_cancel_busy,
        log_session, cancel_status, phase, engine, media_ui, false);
}

/* Arm cheaply: ordinary short tasks must not copy the large UI snapshot or
   sample the pad. Activation occurs only at a same-thread cooperative safe
   point, never inside an outstanding GE list or from the callback thread. */
static struct {
    _Atomic unsigned armed;
    _Atomic int owner_thread;
    PspUiState *ui;
    const uint16_t *frame;
    uint64_t started_us;
    uint64_t last_poll_us;
    bool optional_font_publication;
    PspUiToolbarInputState *toolbar;
} psp_runtime_cooperate;

void psp_runtime_cooperate_begin(PspUiState *ui, const uint16_t *frame,
                                 PspUiToolbarInputState *toolbar)
{
    if (psp_navigation_cooperate.active || ui == NULL || frame == NULL
        || ui->page_gamepad_capture) return;
    psp_runtime_cooperate.ui = ui;
    psp_runtime_cooperate.frame = frame;
    psp_runtime_cooperate.toolbar = toolbar;
    psp_runtime_cooperate.started_us = sceKernelGetSystemTimeWide();
    psp_runtime_cooperate.last_poll_us = 0;
    psp_runtime_cooperate.optional_font_publication = false;
    atomic_store_explicit(&psp_runtime_cooperate.owner_thread,
                          sceKernelGetThreadId(), memory_order_relaxed);
    atomic_store_explicit(&psp_runtime_cooperate.armed, 1u, memory_order_release);
}

bool psp_runtime_cooperate_end(uint32_t *observed_buttons)
{
    if (!atomic_load_explicit(&psp_runtime_cooperate.armed,
                              memory_order_acquire)) return false;
    if (!psp_navigation_cooperate.active
        || !psp_navigation_cooperate.owner_thread_only) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t elapsed = sceKernelGetSystemTimeWide()
            - psp_runtime_cooperate.started_us;
        if (elapsed >= UINT64_C(100000))
            printf("tilefinch-runtime-cooperation: unserviced=%lluus\n",
                   (unsigned long long) elapsed);
#endif
        atomic_store_explicit(&psp_runtime_cooperate.armed, 0u,
                              memory_order_release);
        return false;
    }
    if (observed_buttons != NULL)
        *observed_buttons = psp_navigation_observed_buttons();
    psp_navigation_cooperate_end("page-runtime");
    atomic_store_explicit(&psp_runtime_cooperate.armed, 0u, memory_order_release);
    return true;
}

void psp_navigation_cooperate_begin(
    PspUiState *ui, const uint16_t *frame, BrowserEngine *engine)
{
    psp_work_cooperate_begin(
        ui, frame, true, true, true,
        "STOPPING PAGE LOAD...", "navigation-begin", engine, NULL);
}

void psp_work_cooperate_refresh_media(const PspUiMediaState *media_ui)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    if (media_ui == NULL || cooperate->active == 0
        || !cooperate->media_surface
        || tilefinch_cancellation_requested(&cooperate->cancellation)
        || !__sync_bool_compare_and_swap(
               &cooperate->presenting, 0u, 1u)) return;
    __sync_synchronize();
    if (cooperate->active != 0 && cooperate->media_surface
        && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        cooperate->supervisor_media_ui = *media_ui;
    }
    __sync_synchronize();
    cooperate->presenting = 0;
}

void psp_work_cooperate_begin_media_open(
    PspUiState *ui, const uint16_t *engine_frame,
    const PspUiMediaState *media_ui)
{
    if (ui == NULL || media_ui == NULL) return;
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, "OPENING VIDEO  O CANCEL", 600);
    psp_work_cooperate_begin(
        ui, engine_frame, true, true, true,
        "STOPPING VIDEO...", "media-open", NULL, media_ui);
}

bool psp_navigation_cancel_requested(void)
{
    return !psp_lifecycle_presentation_allowed(&psp_lifecycle)
        || tilefinch_cancellation_requested(
               &psp_navigation_cooperate.cancellation);
}

const TilefinchCancellation *psp_navigation_cancellation(void)
{
    return &psp_navigation_cooperate.cancellation;
}

uint32_t psp_navigation_observed_buttons(void)
{
    return psp_navigation_cooperate.supervisor_previous_buttons;
}

static void psp_supervisor_show_status(
    PspNavigationCooperate *cooperate, const char *status)
{
    /*
     * Every caller of this function is on a cancellation path, so reaching it
     * with a media surface installed means the user has asked for the video to
     * stop. Detach the player here instead of repainting it with a "STOPPING"
     * caption: the main thread is still inside one blocking network primitive
     * and cannot retire the open job for as long as that primitive runs, but
     * the page frame this scope already retained is complete and needs no main
     * thread at all to be shown. Dropping the overlay and letting the periodic
     * tick paint that frame puts the user back on the page within one
     * supervisor interval rather than one network timeout.
     *
     * media_surface is what selects the player composite in the tick, so
     * clearing it is the detach; media_detached is what remembers that the
     * background work is a video so later presses still say so. Nothing in the
     * unwind changes: the latch, the primitive's own abort, the pump-end
     * cancellation check, and the scope-end psp_media_close all still run, and
     * psp_media_close hides the player in the main thread's own UI before the
     * next main-thread present can composite it.
     */
    if (cooperate->media_surface) {
        cooperate->supervisor_media_ui.visible = false;
        cooperate->media_surface = false;
        cooperate->media_detached = true;
    }
    psp_ui_show_status(
        &cooperate->supervisor_ui,
        status == NULL ? "STOPPING VIDEO..." : status, 600);
}

bool psp_navigation_cooperate_active(void)
{
    return psp_navigation_cooperate.active;
}

bool psp_navigation_cooperate_supervised(void)
{
    return psp_navigation_cooperate.active != 0
        && psp_navigation_cooperate.supervised != 0;
}

bool psp_request_provisional_scroll(
    BrowserEngine *engine, PspUiState *ui, int direction)
{
    if (engine == NULL || direction == 0
        || !browser_engine_navigation_pending(engine)) {
        return false;
    }
    direction = direction > 0 ? 1 : -1;
    bool moved =
        browser_engine_scroll_provisional_page(engine, direction);
    if (!moved && psp_navigation_cooperate.active != 0
        && psp_navigation_cooperate.engine == engine) {
        /* A button may arrive before the second snapshot is ready. Retain
           one direction, not an unbounded count of analog-repeat events. */
        (void) __sync_lock_test_and_set(
            &psp_navigation_cooperate.provisional_scroll_requests,
            direction);
    }
    if (ui != NULL) {
        psp_ui_show_status(
            ui,
            moved ? "PAGE PREVIEW - STILL LOADING"
                  : "PAGE MOVE QUEUED - STILL LOADING",
            120);
    }
    return true;
}

void psp_navigation_cooperate_end(const char *scope)
{
    psp_navigation_cooperate.active = 0;
    __sync_synchronize();
    /* A supervisor presentation is normally one framebuffer copy plus one
       vblank. It still owns this state and the alternating VRAM buffer until
       it clears presenting, so a time limit cannot safely transfer ownership:
       clearing the structure after 100 ms used to create a cross-thread
       use-after-reset on an unusually delayed display operation. */
    unsigned drain_wait_ms = 0;
    while (psp_navigation_cooperate.presenting != 0) {
        (void) sceKernelDelayThread(1000);
        drain_wait_ms++;
        if ((drain_wait_ms & 63u) == 0) psp_log_heartbeat();
        if (drain_wait_ms == 100u
            && psp_navigation_cooperate.log_session) {
            printf("tilefinch-ui-supervisor: presentation drain exceeded "
                   "100ms; retaining shared state until release\n");
        }
    }
    if (psp_navigation_cooperate.owner_thread_only
        && psp_navigation_cooperate.cursor_feedback
        && psp_navigation_cooperate.ui != NULL) {
        psp_ui_adopt_priority(psp_navigation_cooperate.ui,
                             &psp_navigation_cooperate.supervisor_ui,
                             psp_navigation_cooperate.original_screen,
                             psp_navigation_cooperate.original_chrome_visible);
    }
    if (psp_navigation_cooperate.pending_media_intent.action
            != PSP_UI_MEDIA_ACTION_NONE) {
        /* The presentation fence above also fences the callback's 64-bit
           seek target on 32-bit Allegrex. Do not read it while the callback
           can still be updating the tuple. */
        psp_completed_supervisor_media_intent =
            psp_navigation_cooperate.pending_media_intent;
    }
    if ((!psp_navigation_cancel_requested()
         || (psp_navigation_cooperate.owner_thread_only
             && psp_navigation_cooperate.input_yield))
        && !psp_navigation_cooperate.media_surface
        && !psp_navigation_cooperate.media_detached) {
        for (uint8_t at = 0;
             at < psp_navigation_cooperate.pending_page_input_count;
             at++) {
            if (psp_completed_supervisor_page_input_count
                    == PSP_SUPERVISOR_PAGE_INPUT_LIMIT) {
                memmove(
                    &psp_completed_supervisor_page_input[0],
                    &psp_completed_supervisor_page_input[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(psp_completed_supervisor_page_input[0]));
                psp_completed_supervisor_page_input_count--;
            }
            psp_completed_supervisor_page_input[
                psp_completed_supervisor_page_input_count++] =
                    psp_navigation_cooperate.pending_page_input[at];
        }
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (psp_navigation_cooperate.pending_page_input_count != 0
        || psp_navigation_cooperate.pending_page_input_dropped != 0) {
        printf("tilefinch-ui-supervisor-input: scope=%s queued=%u "
               "dropped=%u\n",
               scope == NULL ? "unknown" : scope,
               (unsigned) psp_navigation_cooperate.pending_page_input_count,
               (unsigned) psp_navigation_cooperate.pending_page_input_dropped);
    }
#endif
    bool cancelled = psp_navigation_cancel_requested();
    bool report =
        psp_navigation_cooperate.log_session || cancelled
        || psp_navigation_cooperate.presentations != 0
        || psp_navigation_cooperate.input_acknowledgements != 0
        || psp_navigation_cooperate.maximum_checkpoint_gap_us
               >= UINT64_C(100000);
    if (psp_navigation_cooperate.ui != NULL && report) {
        printf("tilefinch-navigation-cooperate: scope=%s checkpoints=%zu "
               "presentations=%zu max-gap=%lluus/%s last-phase=%s "
               "last-work=%zu cancelled=%d\n",
               scope == NULL ? "unknown" : scope,
               psp_navigation_cooperate.checkpoint_calls,
               psp_navigation_cooperate.presentations,
               (unsigned long long)
                   psp_navigation_cooperate.maximum_checkpoint_gap_us,
               psp_navigation_cooperate.maximum_checkpoint_gap_phase == NULL
                   ? "unknown"
                   : psp_navigation_cooperate.maximum_checkpoint_gap_phase,
               psp_navigation_cooperate.last_phase == NULL
                   ? "unknown" : psp_navigation_cooperate.last_phase,
               psp_navigation_cooperate.last_completed_work_units,
               cancelled ? 1 : 0);
    }
    /* active was cleared above, so use the retained checkpoint count rather
       than the active flag to decide whether a session was installed. */
    if (psp_navigation_cooperate.ui != NULL && report) {
        uint64_t ended_us = sceKernelGetSystemTimeWide();
        uint64_t cancel_drain_us =
            psp_navigation_cooperate.cancellation_requested_us != 0
            && ended_us
                 >= psp_navigation_cooperate.cancellation_requested_us
                ? ended_us
                    - psp_navigation_cooperate.cancellation_requested_us
                : 0;
        printf("tilefinch-ui-supervisor: scope=%s available=%u "
               "checkpoints=%zu presentations=%zu max-gap=%lluus/%s "
               "cancelled=%u input-acks=%zu max-ack=%lluus "
               "cancel-drain=%lluus drained=%u injected=%u\n",
               scope == NULL ? "unknown" : scope,
               atomic_load_explicit(
                   &psp_background_ui_available,
                   memory_order_acquire),
               psp_navigation_cooperate.checkpoint_calls,
               psp_navigation_cooperate.presentations,
               (unsigned long long)
                   psp_navigation_cooperate.maximum_checkpoint_gap_us,
               psp_navigation_cooperate.maximum_checkpoint_gap_phase == NULL
                   ? "unknown"
                   : psp_navigation_cooperate.maximum_checkpoint_gap_phase,
               cancelled ? 1u : 0u,
               psp_navigation_cooperate.input_acknowledgements,
               (unsigned long long)
                   psp_navigation_cooperate.maximum_input_ack_us,
               (unsigned long long) cancel_drain_us,
               psp_navigation_cooperate.presenting == 0 ? 1u : 0u,
               psp_navigation_cooperate.validation_cancel_injected
                   ? 1u : 0u);
    }
    memset(&psp_navigation_cooperate, 0,
           sizeof(psp_navigation_cooperate));
}

bool psp_navigation_cooperate_take_media_intent(
    PspUiMediaIntent *intent)
{
    if (intent == NULL
        || psp_completed_supervisor_media_intent.action
               == PSP_UI_MEDIA_ACTION_NONE) return false;
    *intent = psp_completed_supervisor_media_intent;
    psp_completed_supervisor_media_intent = (PspUiMediaIntent) {0};
    return true;
}

bool psp_navigation_cooperate_take_page_input(uint32_t *pressed)
{
    if (pressed == NULL
        || psp_completed_supervisor_page_input_count == 0) return false;
    *pressed = psp_completed_supervisor_page_input[0];
    psp_completed_supervisor_page_input_count--;
    if (psp_completed_supervisor_page_input_count != 0) {
        memmove(
            &psp_completed_supervisor_page_input[0],
            &psp_completed_supervisor_page_input[1],
            psp_completed_supervisor_page_input_count
                * sizeof(psp_completed_supervisor_page_input[0]));
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-ui-supervisor-input: replay=0x%04x remaining=%u\n",
           (unsigned) *pressed,
           (unsigned) psp_completed_supervisor_page_input_count);
#endif
    return true;
}

static void psp_work_ui_tick(bool owner_thread);

void psp_background_ui_tick(void)
{
    psp_work_ui_tick(false);
}

static void psp_work_ui_tick(bool owner_thread)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    if (cooperate->active == 0
        || cooperate->owner_thread_only != owner_thread
        || (!owner_thread && cooperate->supervised == 0)) return;
    if (!__sync_bool_compare_and_swap(
            &cooperate->presenting, 0u, 1u)) return;
    __sync_synchronize();
    if (cooperate->active == 0) {
        cooperate->presenting = 0;
        return;
    }
    SceCtrlData pad = { .Lx = 128, .Ly = 128 };
    bool urgent_present = false;
    uint64_t acknowledgement_started_us = 0;
    uint32_t ui_pressed = 0;
    PspUiMediaIntent supervisor_media_intent = {0};
    PspUiInput scripted_input = {
        .analog_x = 128,
        .analog_y = 128
    };
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool scripted = psp_input_script_running()
        && psp_input_script_busy_frame(&scripted_input);
    if (scripted) ui_pressed = scripted_input.pressed;
#else
    const bool scripted = false;
#endif
    uint64_t now_us = sceKernelGetSystemTimeWide();
    if (psp_home_exit_pending()
        && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us = now_us;
        acknowledgement_started_us = now_us;
        cooperate->input_acknowledgements++;
        psp_ui_show_status(&cooperate->supervisor_ui, "CLOSING...", 600);
        urgent_present = true;
    }
    unsigned validation_cancel_after_ms =
        psp_validation_cancel_after_ms;
    if (validation_cancel_after_ms != 0
        && !tilefinch_cancellation_requested(&cooperate->cancellation)
        && now_us >= cooperate->started_us
        && now_us - cooperate->started_us
               >= (uint64_t) validation_cancel_after_ms * 1000u) {
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us = now_us;
        cooperate->validation_cancel_injected = true;
        acknowledgement_started_us = now_us;
        cooperate->input_acknowledgements++;
        psp_supervisor_show_status(cooperate, cooperate->cancel_status);
        urgent_present = true;
    }
    uint32_t physical_pressed = 0;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
        physical_pressed =
            pad.Buttons & ~cooperate->supervisor_previous_buttons;
        cooperate->supervisor_previous_buttons = pad.Buttons;
    }
    if (!scripted) {
        ui_pressed = psp_ui_buttons(physical_pressed)
            | psp_controller_take_latched_pressed();
    }
    /* Only the rollback-safe optional face transaction is preemptible here.
       Never interrupt author mutations this way or dispatch DOM input while
       layout is borrowing its state. Button edges use the existing mailbox;
       held nub movement resumes through the ordinary next-frame receiver. */
    int analog_x = scripted ? scripted_input.analog_x : pad.Lx;
    int analog_y = scripted ? scripted_input.analog_y : pad.Ly;
    bool analog_active = analog_x < 104 || analog_x > 152
        || analog_y < 104 || analog_y > 152;
    bool priority_handled = false;
    if (owner_thread && cooperate->engine == NULL
        && !cooperate->media_surface && !cooperate->media_detached
        && cooperate->pending_page_input_count == 0) {
        /* Native menus and cursor movement do not call the document. Keep
           their visual state on the completed frame while page work runs. */
        PspUiInput native_input = { .pressed = ui_pressed,
            .held = scripted ? scripted_input.held : psp_ui_buttons(pad.Buttons),
            .analog_x = analog_x, .analog_y = analog_y, .elapsed_ms = 16 };
        PspUiToolbarInputState *toolbar = psp_runtime_cooperate.toolbar;
        bool toolbar_tracking = toolbar != NULL && (toolbar->gesture.held
            || ((native_input.held & PSP_UI_BUTTON_TOOLBAR) != 0
                && cooperate->supervisor_ui.screen == PSP_UI_SCREEN_PAGE));
        if (psp_ui_filter_toolbar_input(toolbar, &native_input,
                cooperate->supervisor_ui.screen == PSP_UI_SCREEN_PAGE,
                scripted && toolbar != NULL ? toolbar->sample_ms + 16u : now_us / 1000u))
            toolbar->reader_pending = true;
        priority_handled = psp_ui_update_priority(
            &cooperate->supervisor_ui, &native_input);
        /* A held Triangle is consumed by the gesture, not replayed as a
           fresh press on return to the main loop. Reader dispatch waits for
           the page's borrowed state to be released. */
        priority_handled |= toolbar_tracking;
        if (priority_handled) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            /* Include owner-checkpoint cursor samples, not just the main
               loop: omitting these makes a responsive supervised interval
               look like one long gap in the cursor cadence report. */
            if (ui_pressed == 0 && analog_active)
                psp_cursor_latency_sample(now_us);
#endif
            cooperate->cursor_feedback = true;
            cooperate->input_acknowledgements++;
            acknowledgement_started_us = sceKernelGetSystemTimeWide();
            urgent_present = true;
        }
    }
    bool yield_for_input = owner_thread
        && psp_runtime_cooperate.optional_font_publication
        && (ui_pressed != 0 || analog_active);
    if (yield_for_input) {
        cooperate->input_yield = true;
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us = sceKernelGetSystemTimeWide();
        if (ui_pressed != 0 && !priority_handled
            && cooperate->pending_page_input_count < PSP_SUPERVISOR_PAGE_INPUT_LIMIT)
            cooperate->pending_page_input[cooperate->pending_page_input_count++] = ui_pressed;
    } else if (priority_handled) {
        /* Already applied; replaying Select would immediately close the menu. */
    } else if ((ui_pressed & PSP_UI_BUTTON_CANCEL) != 0
        && !tilefinch_cancellation_requested(
            &cooperate->cancellation)) {
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us =
            sceKernelGetSystemTimeWide();
        acknowledgement_started_us =
            cooperate->cancellation_requested_us;
        cooperate->input_acknowledgements++;
        psp_supervisor_show_status(cooperate, cooperate->cancel_status);
        urgent_present = true;
    } else if (cooperate->media_surface
               && (ui_pressed
                   & (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT
                      | PSP_UI_BUTTON_PAGE_UP | PSP_UI_BUTTON_PAGE_DOWN
                      | PSP_UI_BUTTON_CONFIRM)) != 0
               && !tilefinch_cancellation_requested(
                   &cooperate->cancellation)) {
        /*
         * A preview seek can spend several callback ticks in a firmware or
         * range unit. Dropping direction edges here made the player appear
         * unresponsive until that picture returned. Apply each edge to the
         * supervisor's private UI snapshot immediately and retain one latest
         * command for the browser thread. The one-entry mailbox is deliberate:
         * repeated directions are a target, not N decoder transactions.
         */
        PspUiInput media_input = {
            .pressed = ui_pressed,
            .held = ui_pressed,
            .analog_x = 128,
            .analog_y = 128,
            .elapsed_ms = 16
        };
        supervisor_media_intent = psp_ui_media_update(
            &cooperate->supervisor_media_ui, &media_input);
        if (supervisor_media_intent.action
                != PSP_UI_MEDIA_ACTION_NONE) {
            cooperate->pending_media_intent = supervisor_media_intent;
        }
        if (supervisor_media_intent.visual_changed) {
            cooperate->input_acknowledgements++;
            acknowledgement_started_us =
                sceKernelGetSystemTimeWide();
            urgent_present = true;
        }
    } else if ((ui_pressed & (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
                              | PSP_UI_BUTTON_PAGE_UP
                              | PSP_UI_BUTTON_PAGE_DOWN)) != 0
               && cooperate->engine != NULL
               && !tilefinch_cancellation_requested(
                   &cooperate->cancellation)) {
        int direction =
            (ui_pressed & (PSP_UI_BUTTON_DOWN
                           | PSP_UI_BUTTON_PAGE_DOWN)) != 0
                ? 1 : -1;
        (void) __sync_lock_test_and_set(
            &cooperate->provisional_scroll_requests, direction);
        cooperate->input_acknowledgements++;
        acknowledgement_started_us =
            sceKernelGetSystemTimeWide();
        psp_ui_show_status(
            &cooperate->supervisor_ui,
            "MOVING PAGE PREVIEW...", 120);
        urgent_present = true;
    } else if (ui_pressed != 0
               && tilefinch_cancellation_requested(
                   &cooperate->cancellation)) {
        cooperate->input_acknowledgements++;
        acknowledgement_started_us =
            sceKernelGetSystemTimeWide();
        psp_supervisor_show_status(
            cooperate, cooperate->media_surface || cooperate->media_detached
                ? "VIDEO IS STILL STOPPING" : "STILL STOPPING - PLEASE WAIT");
        urgent_present = true;
    } else if (ui_pressed != 0
               && cooperate->engine == NULL
               && !cooperate->media_surface) {
        /* The stable page is still authoritative while a raster or other
           bounded page service runs. Retain input for the browser-thread
           controller instead of acknowledging and discarding it. The queue
           is deliberately tiny; if a pathological service outlives a burst,
           prefer the newest four commands so the eventual state follows the
           user's latest intent. */
        if (cooperate->pending_page_input_count
                == PSP_SUPERVISOR_PAGE_INPUT_LIMIT) {
            memmove(
                &cooperate->pending_page_input[0],
                &cooperate->pending_page_input[1],
                (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                    * sizeof(cooperate->pending_page_input[0]));
            cooperate->pending_page_input_count--;
            cooperate->pending_page_input_dropped++;
        }
        cooperate->pending_page_input[
            cooperate->pending_page_input_count++] = ui_pressed;
        cooperate->input_acknowledgements++;
        acknowledgement_started_us = sceKernelGetSystemTimeWide();
        if (owner_thread) {
            psp_ui_show_status(&cooperate->supervisor_ui,
                              "PAGE UPDATE - INPUT QUEUED", 120);
            urgent_present = true;
        }
    } else if (ui_pressed != 0
               && cooperate->acknowledge_non_cancel_busy) {
        cooperate->input_acknowledgements++;
        acknowledgement_started_us =
            sceKernelGetSystemTimeWide();
        psp_ui_show_status(
            &cooperate->supervisor_ui,
            "BUSY - CIRCLE CANCELS", 120);
        urgent_present = true;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (scripted) {
        if (cooperate->media_surface)
            psp_input_script_observe_media(
                &supervisor_media_intent,
                &cooperate->supervisor_media_ui);
        psp_input_script_observe(NULL, &cooperate->supervisor_ui);
        psp_input_script_capture_live_mark(
            psp_display_front_buffer(&psp_display),
            PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
    }
#endif
    now_us = sceKernelGetSystemTimeWide();
    if (!urgent_present && !cooperate->periodic_present) {
        cooperate->presenting = 0;
        return;
    }
    if (!urgent_present && cooperate->last_present_us != 0
        && now_us - cooperate->last_present_us
               < PSP_NAVIGATION_PRESENT_INTERVAL_US) {
        cooperate->presenting = 0;
        return;
    }
    if (cooperate->active != 0) {
        PspUiInput idle = {
            .analog_x = 128,
            .analog_y = 128
        };
        (void) psp_ui_update(&cooperate->supervisor_ui, &idle);
        /* Both variants are immutable snapshots. The supervisor never reads
           the main thread's mutable media or browser state while a native
           decoder, resolver, or raster unit is running. */
        if (cooperate->media_surface)
            psp_present_supervisor_media(&cooperate->supervisor_media_ui);
        else if (!psp_present_supervisor_ui(
                     cooperate->frame, &cooperate->supervisor_ui)) {
            /* Input remains queued, but a refused latch is not visible
               feedback and must not produce a successful latency sample. */
            cooperate->presenting = 0;
            return;
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (scripted
            && !cooperate->supervisor_ui.page_gamepad_capture
            && (ui_pressed & PSP_UI_BUTTON_CANCEL) != 0) {
            psp_input_script_capture_named(
                "cancel-ack", psp_display_front_buffer(&psp_display),
                PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
        }
#endif
        cooperate->last_present_us = now_us;
        cooperate->presentations++;
        if (acknowledgement_started_us != 0) {
            uint64_t finished_us = sceKernelGetSystemTimeWide();
            uint64_t elapsed_us =
                finished_us >= acknowledgement_started_us
                    ? finished_us - acknowledgement_started_us : 0;
            if (elapsed_us > cooperate->maximum_input_ack_us)
                cooperate->maximum_input_ack_us = elapsed_us;
        }
    }
    __sync_synchronize();
    cooperate->presenting = 0;
}

bool psp_platform_cooperate(
    void *context, const char *phase, size_t completed_work_units)
{
    PspNavigationCooperate *cooperate = context;
    if (cooperate == &psp_navigation_cooperate
        && atomic_load_explicit(&psp_runtime_cooperate.armed,
                                memory_order_acquire)) {
        /* Reject worker hooks before reading any owner-only borrowed state. */
        if (sceKernelGetThreadId() != atomic_load_explicit(
                &psp_runtime_cooperate.owner_thread, memory_order_relaxed))
            return true;
        if (phase != NULL && strcmp(phase, "optional-font-publication") == 0) {
            psp_runtime_cooperate.optional_font_publication = completed_work_units != 0;
            if (completed_work_units == 0) return true;
        }
        uint64_t now = sceKernelGetSystemTimeWide();
        if (!cooperate->active
            && now - psp_runtime_cooperate.started_us >= UINT64_C(8000)) {
            psp_work_cooperate_begin_internal(
                psp_runtime_cooperate.ui, psp_runtime_cooperate.frame,
                psp_runtime_cooperate.ui->page_activation_busy,
                true, false, "STOPPING PAGE UPDATE...",
                "page-runtime", NULL, NULL, true);
            /* Include work before the first checkpoint in gap accounting. */
            cooperate->started_us = psp_runtime_cooperate.started_us;
            cooperate->last_checkpoint_us = psp_runtime_cooperate.started_us;
        }
        if (cooperate->active && cooperate->owner_thread_only
            && (psp_runtime_cooperate.last_poll_us == 0
                || now - psp_runtime_cooperate.last_poll_us >= UINT64_C(16000))) {
            psp_runtime_cooperate.last_poll_us = now;
            psp_work_ui_tick(true);
        }
    }
    if (cooperate == NULL || !cooperate->active) return true;
    if (psp_home_exit_pending()
        && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us =
            sceKernelGetSystemTimeWide();
        psp_ui_show_status(cooperate->ui, "CLOSING...", 600);
    }
    psp_log_heartbeat();
    uint64_t now_us = sceKernelGetSystemTimeWide();
    uint64_t checkpoint_gap_us =
        now_us >= cooperate->last_checkpoint_us
            ? now_us - cooperate->last_checkpoint_us : 0;
    if (checkpoint_gap_us > cooperate->maximum_checkpoint_gap_us) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (checkpoint_gap_us > UINT64_C(33000))
            printf("tilefinch-cooperate-slow-gap: elapsed=%lluus from=%s to=%s work=%zu\n",
                   (unsigned long long) checkpoint_gap_us,
                   cooperate->last_phase == NULL ? "unknown" : cooperate->last_phase,
                   phase == NULL ? "unknown" : phase, completed_work_units);
#endif
        cooperate->maximum_checkpoint_gap_us = checkpoint_gap_us;
        cooperate->maximum_checkpoint_gap_phase = phase;
    }
    cooperate->last_checkpoint_us = now_us;
    cooperate->last_phase = phase;
    cooperate->last_completed_work_units = completed_work_units;
    cooperate->checkpoint_calls++;
    if (cooperate->provisional_present_requested != 0
        && cooperate->engine != NULL) {
        BrowserProvisionalViewport viewport = {0};
        if (browser_engine_provisional_viewport(
                cooperate->engine, &viewport)
            && psp_platform_present(
                cooperate, viewport.pixels,
                PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
                PSP_SCREEN_WIDTH)) {
            cooperate->provisional_present_requested = 0;
        }
    }
    if (psp_validation_preview_scroll != 0
        && !cooperate->validation_preview_scroll_injected
        && cooperate->engine != NULL
        && !tilefinch_cancellation_requested(
            &cooperate->cancellation)) {
        BrowserProvisionalViewport viewport = {0};
        if (browser_engine_provisional_viewport(
                cooperate->engine, &viewport)
            && viewport.frame_count > 1
            && viewport.current_frame == 0
            && browser_engine_scroll_provisional_page(
                cooperate->engine, 1)) {
            cooperate->validation_preview_scroll_injected = true;
            cooperate->input_acknowledgements++;
            psp_ui_show_status(
                cooperate->ui,
                "PAGE PREVIEW - STILL LOADING", 120);
            printf("tilefinch-preview-validation: scroll=page-down "
                   "frame=%zu/%zu y=%d\n",
                   viewport.current_frame + 2u,
                   viewport.frame_count,
                   viewport.maximum_scroll_y);
        }
    }
    int provisional_scroll_requests = __sync_lock_test_and_set(
        &cooperate->provisional_scroll_requests, 0);
    if (cooperate->engine != NULL
        && provisional_scroll_requests != 0
        && !tilefinch_cancellation_requested(
            &cooperate->cancellation)) {
        int direction = provisional_scroll_requests > 0 ? 1 : -1;
        psp_ui_show_status(
            cooperate->ui, "PAGE PREVIEW - STILL LOADING", 120);
        bool moved = browser_engine_scroll_provisional_page(
            cooperate->engine, direction);
        if (!moved) {
            /* The first snapshot may be visible while the page-down
               snapshot is still rasterizing. Keep one bounded request for
               the next same-thread engine checkpoint. */
            (void) __sync_lock_test_and_set(
                &cooperate->provisional_scroll_requests, direction);
        }
    }
    bool urgent_present = false;
    uint64_t acknowledgement_started_us = 0;
    if (cooperate->supervised == 0 && !cooperate->owner_thread_only) {
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
            uint32_t pressed =
                pad.Buttons & ~cooperate->fallback_previous_buttons;
            cooperate->fallback_previous_buttons = pad.Buttons;
            if ((pressed & PSP_CTRL_CIRCLE) != 0
                && !tilefinch_cancellation_requested(
                    &cooperate->cancellation)) {
                tilefinch_cancellation_request(&cooperate->cancellation);
                cooperate->cancellation_requested_us =
                    sceKernelGetSystemTimeWide();
                acknowledgement_started_us =
                    cooperate->cancellation_requested_us;
                cooperate->input_acknowledgements++;
                urgent_present = true;
            } else if ((pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN
                                   | PSP_CTRL_LTRIGGER
                                   | PSP_CTRL_RTRIGGER)) != 0
                       && cooperate->engine != NULL
                       && !tilefinch_cancellation_requested(
                           &cooperate->cancellation)) {
                int direction =
                    (pressed & (PSP_CTRL_DOWN | PSP_CTRL_RTRIGGER)) != 0
                        ? 1 : -1;
                (void) __sync_lock_test_and_set(
                    &cooperate->provisional_scroll_requests,
                    direction);
                cooperate->input_acknowledgements++;
                acknowledgement_started_us =
                    sceKernelGetSystemTimeWide();
                psp_ui_show_status(
                    cooperate->ui,
                    "PAGE MOVE QUEUED - STILL LOADING", 120);
                urgent_present = true;
            }
        }
        if (urgent_present) {
            psp_ui_show_status(
                cooperate->ui, "STOPPING PAGE LOAD...", 600);
        }
    }
    if (cooperate->supervised == 0 && !cooperate->owner_thread_only
        && (urgent_present || cooperate->last_present_us == 0
        || now_us - cooperate->last_present_us
               >= PSP_NAVIGATION_PRESENT_INTERVAL_US)) {
        PspUiInput idle = {
            .analog_x = 128,
            .analog_y = 128
        };
        (void) psp_ui_update(cooperate->ui, &idle);
        psp_present_supervisor_ui(cooperate->frame, cooperate->ui);
        cooperate->last_present_us = now_us;
        cooperate->presentations++;
        if (acknowledgement_started_us != 0) {
            uint64_t finished_us = sceKernelGetSystemTimeWide();
            uint64_t elapsed_us =
                finished_us >= acknowledgement_started_us
                    ? finished_us - acknowledgement_started_us : 0;
            if (elapsed_us > cooperate->maximum_input_ack_us)
                cooperate->maximum_input_ack_us = elapsed_us;
        }
    }
    return !tilefinch_cancellation_requested(&cooperate->cancellation);
}

/*
 * The boot entrance.
 *
 * Init presents whichever frame of the choreography it has reached; the
 * frame is a pure function of that index, so a fast boot skips ahead and a
 * slow one simply holds on the frame it got to. Nothing here waits, polls,
 * or samples input: safe start belongs to the update launcher, which samples
 * a pre-held L trigger over its short startup window and finishes before this
 * process starts. The line this surface draws restates that launch contract;
 * it is not a reader of the button.
 *
 * When a stage overruns, the entrance holds the mark centred and shows the
 * stage's own name -- the single muted line the slow branch allows. The
 * next stage that arrives promptly resumes the choreography.
 */
#define PSP_BOOT_ENTRANCE_SLOW_US UINT64_C(700000)

static uint64_t psp_boot_entrance_previous_us;

void psp_present_boot_entrance(
    unsigned frame, const char *stage, bool wave, const PspUiState *home)
{
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    bool slow = psp_boot_entrance_previous_us != 0
        && now_us - psp_boot_entrance_previous_us
               >= PSP_BOOT_ENTRANCE_SLOW_US;
    psp_boot_entrance_previous_us = now_us;
    (void) psp_display_video_end(&psp_display);
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (vram == NULL) return;
    PspUiBootEntranceView view = {
        .frame = frame,
        .wave = wave,
        .branch_status = slow ? stage : NULL
    };
    psp_ui_boot_entrance_composite(
        &view, home, vram, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
        PSP_VRAM_STRIDE);
    (void) psp_display_publish(&psp_display);
}
