/* Device runtime for the PSP browser EBOOT: display presentation and cadence,
 * exit/power callbacks, the clock policy worker, the cooperative-work
 * supervisor that keeps the panel alive during long engine operations, the
 * validation-only power test, and the boot entrance.
 *
 * The callback-visible process globals live here because this TU owns their
 * writers. Ordinary browser resources remain in the canonical owner records.
 */
#include "psp_app_internal.h"
#include "psp_app_validation.h"
#include "tilefinch/psp_input_route.h"
#include <psputility_sysparam.h>
#include "tilefinch/pixel_math.h"
#include "tilefinch/psp_load_experience.h"
#include "tilefinch/psp_time.h"
#include "tilefinch/psp_threads.h"
#include "../psp_thread_contract.h"
#include "../psp_canvas_ge.h"
#include "tilefinch/canvas_ge_presenter.h"

PspMediaSession *psp_active_media;
atomic_uint psp_background_ui_available;
atomic_bool psp_home_exit_requested;
PspLifecycle psp_lifecycle;

/* Only the browser's verified VFPU owner may use the accelerated CPU copy.
   The supervisor also presents pages but has no VFPU thread context. */
static atomic_int psp_present_vfpu_owner;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* On, as in shipping builds; copy-vfpu-off selects the scalar copy for an
   A/B run, and copy-vfpu-check compares each copy (a mismatch turns the
   VFPU copy off). */
static bool psp_present_vfpu_copy_enabled = true;
static bool psp_present_vfpu_copy_verify;
static uint32_t psp_present_vfpu_copies, psp_present_vfpu_checks;
static uint32_t psp_present_vfpu_errors;

void psp_present_validation_vfpu_copy(bool enabled, bool verify)
{
    psp_present_vfpu_copy_enabled = enabled;
    psp_present_vfpu_copy_verify = verify;
}

void psp_present_validation_vfpu_copy_counts(
    uint32_t *copies, uint32_t *checks, uint32_t *errors)
{
    *copies = psp_present_vfpu_copies;
    *checks = psp_present_vfpu_checks;
    *errors = psp_present_vfpu_errors;
}

#endif

/* GE-direct canvas publication (canvas_ge_presenter.h, psp_canvas_ge.h).
   The engine pointer only identifies whose RAM frame a present is handed. */
static BrowserEngine *psp_present_canvas_engine;
static CanvasGePresenter psp_present_canvas_ge;

void psp_present_canvas_engine_bind(BrowserEngine *engine)
{
    psp_present_canvas_engine = engine;
}

void psp_present_canvas_ge_enable(bool enabled, bool verify)
{
    canvas_ge_presenter_enable(&psp_present_canvas_ge, enabled, verify);
}

const CanvasGePresenterStats *psp_present_canvas_ge_stats(void)
{
    return &psp_present_canvas_ge.stats;
}

const char *psp_user_visible_error(
    const char *detail, long tls_verify_result,
    bool tls_verify_result_available, bool tls_verification_failed,
    TilefinchTlsGuidance *tls_guidance,
    char *summary, size_t summary_size)
{
    if (detail == NULL) detail = "";
    if (tls_guidance != NULL) *tls_guidance = TILEFINCH_TLS_GUIDANCE_NONE;
    if (summary == NULL || summary_size == 0
        || (!tls_verification_failed
            && !tilefinch_error_is_certificate_verification_failure(detail))) {
        return detail;
    }
    PspTimeStatus clock_status = psp_time_read_utc(NULL);
    /* Keep the useful route-level context, but move the recovery action onto
       its own full-width line. The unabridged backend sentence remains in the
       failure report rather than being clipped into an unreadable toast. */
    const char *headline = tilefinch_tls_verification_summary(
        detail, (uint32_t) tls_verify_result,
        tls_verify_result_available, clock_status, tls_guidance);
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
    SceKernelThreadInfo thread;
    SceUID owner = sceKernelGetThreadId();
    atomic_store_explicit(&psp_present_vfpu_owner,
        owner > 0 && psp_thread_snapshot(owner, &thread) >= 0
            && (thread.attr & PSP_THREAD_ATTR_VFPU) != 0 ? owner : 0,
        memory_order_relaxed);
    memset(presentation, 0, sizeof(*presentation));
    /* GE-direct publication is the default for eligible full-screen WebGL
       canvases; everything else keeps the CPU conversion and copy. */
    static const CanvasGeBackend canvas_ge_backend = {
        .scale = psp_canvas_ge_scale, .busy = psp_canvas_ge_busy,
        .fail = psp_canvas_ge_fail, .context = NULL
    };
    canvas_ge_presenter_init(&psp_present_canvas_ge, &canvas_ge_backend);
    canvas_ge_presenter_enable(&psp_present_canvas_ge, true, false);
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
    psp_presentation_rebind_chrome_fonts(
        regular,
        browser_engine_font_face_variant(
            engine, FONT_SANS, false, true),
        presentation->ui.browser_ui_scale);
    presentation->chrome_fonts_bound = true;
    psp_report_chrome_glyph_preload();
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
PSP_VALIDATION_SHARED PspMediaPresentRecord
    psp_media_present_records[PSP_DISPLAY_PAGE_BUFFER_COUNT];
static PspUiMediaTrackMenuCache psp_media_track_menu_cache;
static uint32_t psp_media_front_menu_key, psp_media_front_menu_epoch;
static uint64_t psp_media_front_frame_epoch;
static uint64_t psp_media_front_frame_identity;
static uint64_t psp_media_front_menu_generation;
static uint64_t psp_media_menu_deferred_us;
_Static_assert(
    PSP_DISPLAY_VIDEO_AUX_BYTES
        >= (size_t) PSP_UI_MEDIA_TRACK_MENU_WIDTH
             * (size_t) PSP_UI_MEDIA_TRACK_MENU_MAX_HEIGHT * sizeof(uint16_t),
    "the bounded video EDRAM tail must hold the retained track menu");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Set by the presenter admission seam and sampled only after a successful
   publish. Validation needs to distinguish a freshly drawn picture from a
   scanout accepted through the same-picture fast path. */
bool psp_media_present_last_skipped;
#endif

void psp_media_present_forget_buffers(void)
{
    psp_media_front_menu_key = 0;
    psp_media_menu_deferred_us = 0;
    psp_media_present_records_reset(
        psp_media_present_records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
}

bool psp_media_menu_feedback_pending(void)
{
    if (psp_active_media == NULL || !psp_active_media->ui.visible
        || !psp_active_media->ui.playing
        || psp_active_media->generation != psp_media_front_menu_generation)
        psp_media_menu_deferred_us = 0;
    return psp_media_menu_deferred_us != 0;
}

static bool psp_media_menu_feedback_wait(bool same_picture_feedback)
{
    if (!psp_active_media->ui.playing) {
        psp_media_menu_deferred_us = 0;
        return false;
    }
    /* A fresh picture ends the wait, not the publication obligation. Keep a
       pending request until a successful swap, including after refusal. */
    if (!same_picture_feedback) return false;
    /* Do not spend a display interval on an old picture just for UI feedback.
       The ordinary owner pump keeps decoding while this request remains due.
       Bound the wait to one 30-Hz picture so stalled streams still get UI. */
    uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
    if (psp_media_menu_deferred_us == 0) psp_media_menu_deferred_us = now;
    return now - psp_media_menu_deferred_us < UINT64_C(33334);
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
    if (psp_active_media == NULL || psp_active_media->ui_presentation.clockwise)
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


/* Common bookkeeping for both presenters: cost accounting and the identity
   record, which are the same statements whatever the surface's width is.
   The cost totals feed only the validation build's media job report and
   stability summary. */
static void psp_media_present_account(
    PspMediaPresentRecord *record, const MediaVideoFrame *frame,
    const PspMediaPresentPlan *plan, uint64_t generation,
    uint64_t started_us, const PspMediaPresentGeCost *cost, bool drawn,
    bool chrome_paints)
{
    psp_vprobe_present_account(started_us, cost, drawn);
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
    static bool previous_clockwise;
    bool clockwise = plan->quad_count != 0 && plan->quads[0].clockwise;
    if (clockwise != previous_clockwise) {
        psp_media_present_forget_buffers();
        previous_clockwise = clockwise;
    }
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
    /* A refused GE never leads to a CPU rotation pass. Return to the proven
       landscape Sharp path, including its matching control orientation. */
    if (psp_active_media != NULL) {
        psp_active_media->ui_presentation.clockwise = false;
        psp_active_media->ui_presentation.portrait_crop = false;
    }
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
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t stage_total_us = 0;
#endif
    PspMediaPresentGeCost total = {0, 0, 0};
    /* The strip copies overwrite the same EDRAM buffer used by a retained
       crop. A paused picture can change modes without changing identity. */
    psp_active_media->present_stage_identity = 0;
    psp_active_media->present_texture_staged = true;
    for (size_t at = 0; at < strips->strip_count; at++) {
        const PspMediaPresentStrip *strip = &strips->strips[at];
        const uint32_t *source = (const uint32_t *) frame->pixels
            + (size_t) strip->source_row * (size_t) frame->stride_pixels;
        size_t bytes = (size_t) frame->stride_pixels
            * (size_t) strip->copy_rows * sizeof(*source);
        if (bytes > PSP_DISPLAY_VIDEO_TEXTURE_BYTES) goto finished;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t copy_started_us = (uint64_t) sceKernelGetSystemTimeWide();
#endif
        if (!psp_media_present_ge_stage_dma(staging, source, bytes)) {
            psp_media_present_stage(
                staging, source, frame->stride_pixels,
                frame->stride_pixels, strip->copy_rows);
            psp_media_present_ge_stage_flush(staging, bytes);
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        stage_total_us +=
            (uint64_t) sceKernelGetSystemTimeWide() - copy_started_us;
#endif

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

#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Read only by the validation media-job report. */
    psp_active_media->present_stage_frames++;
    psp_active_media->present_stage_total_us += stage_total_us;
    if (stage_total_us > psp_active_media->present_stage_max_us)
        psp_active_media->present_stage_max_us = stage_total_us;
#endif
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
    bool clockwise = psp_active_media != NULL
        && psp_active_media->ui_presentation.clockwise;
    bool crop = clockwise && psp_active_media->ui_presentation.portrait_crop;
    bool planned = crop
        ? psp_media_present_plan_portrait_crop(
              &plan, frame->width, frame->height, frame->stride_pixels,
              PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT)
        : clockwise
        ? psp_media_present_plan_clockwise(
              &plan, frame->width, frame->height, frame->stride_pixels,
              PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT)
        : psp_media_present_plan(
              &plan, frame->width, frame->height, frame->stride_pixels,
              PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    if (!planned) return false;
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
    psp_vprobe_track_menu_blit(
        blitted, &cost, source, source_width, source_height, source_stride,
        source_changed, destination, destination_stride, left, top);
    return blitted;
}

/* One video frame plus its chrome, published. Out of line: the 16-bit
   compositor is what every non-video frame walks. */
static void psp_media_service_scanout_wait(void *context)
{
    (void) psp_media_feed_before_blocking(context, true);
}

static bool psp_media_wait_video_due(PspMediaSession *media)
{
    uint64_t started_us = sceKernelGetSystemTimeWide();
    for (unsigned poll = 0; poll < 18u; poll++) {
        uint64_t delay_us = psp_media_present_delay_us(media);
        if (delay_us == 0) return true;
        media->presentation_deferred = true;
        uint64_t now_us = sceKernelGetSystemTimeWide();
        if (delay_us > PSP_MEDIA_PREPARATION_WINDOW_US || now_us < started_us
            || now_us - started_us >= PSP_MEDIA_PREPARATION_WINDOW_US) return false;
        uint64_t remaining_us = PSP_MEDIA_PREPARATION_WINDOW_US - (now_us - started_us);
        if (delay_us > remaining_us) delay_us = remaining_us;
        if (delay_us > 1000u) delay_us = 1000u;
        psp_media_service_scanout_wait(media);
        if (sceKernelDelayThread((unsigned) delay_us) < 0) return false;
    }
    return psp_media_present_delay_us(media) == 0;
}

__attribute__((noinline))
static bool psp_present_video_surface(void)
{
    PspVprobeVideo probe;
    psp_vprobe_video_begin(&probe);
    uint32_t *vram = psp_display_video_back_buffer(&psp_display);
    PspMediaPresentGeCost cost = {0, 0, 0};
    uint32_t feedback_key = psp_ui_media_menu_feedback_key(&psp_active_media->ui);
    uint32_t feedback_epoch = psp_display_edram_content_epoch();
    /* The front is read-only; the hidden back is exclusively ours. Copy the
       completed scanout and replace only the opaque chooser. Never redraw a
       blended control on these already-composited pixels. A smaller chooser,
       dismissal, seek, rotation or stream/EDRAM transition takes fresh video.
       A newer decoded picture also takes priority: epoch equality alone says
       nothing about which picture the front buffer actually contains. */
    bool current_picture = psp_media_front_menu_generation == psp_active_media->generation
        && psp_media_front_menu_epoch == feedback_epoch
        && psp_media_front_frame_epoch == psp_active_media->frame.epoch
        && psp_media_front_frame_identity == psp_active_media->frame.identity;
    bool menu_feedback = current_picture
        && psp_ui_media_menu_feedback_reuse(psp_media_front_menu_key, feedback_key);
    /* Decoder/status changes can request a refresh between source pictures
       too. Coalesce those without spending a vblank on a duplicate; actual
       buffering/end state and seek/rotation previews must remain immediate. */
    if (psp_media_menu_feedback_wait(current_picture && feedback_key != 0
            && !psp_active_media->ui.buffering && !psp_active_media->ui.ended))
        return false;
    menu_feedback = menu_feedback && psp_media_present_ge_stage_rows(vram,
            psp_display_video_front_buffer(&psp_display),
            PSP_VRAM_STRIDE, PSP_VRAM_STRIDE, PSP_SCREEN_HEIGHT);
    if (menu_feedback)
        psp_media_present_records[psp_display.back_buffer].valid = false;
    bool overlay_paints =
        psp_ui_media_overlay_paints(&psp_active_media->ui);
    psp_vprobe_video_skipped(menu_feedback);
    /* Only this owner changes priority. Audio/codec keep their higher band;
       transport keeps running during DMA/GE/vblank waits, but cannot delay
       the owner once a draw finishes. Restore on both exit paths below. */
    int previous_priority = psp_thread_priority_raise(
        TILEFINCH_PSP_THREAD_PRIORITY_PRESENT);
    if (vram == NULL
        || (!menu_feedback && !psp_present_media_frame_video(
               vram, &psp_active_media->frame,
               overlay_paints, &cost))) {
        int restored = psp_thread_priority_restore(previous_priority);
        if (restored < 0)
            printf("tilefinch-media-present: priority-restore=0x%08X\n",
                   (unsigned) restored);
        /* The engine refused after the surface was entered. Hand the panel
           back and let the next present run the software scaler; publishing a
           buffer nothing drew into would be a frame of noise. */
        (void) psp_display_video_end(&psp_display);
        psp_media_present_forget_buffers();
        return false;
    }
    psp_vprobe_video_drawn(&probe);
    /* A firmware batch can end with several access units which produce no
       picture. Collect/feed after drawing, not only at the next loop head:
       otherwise overlay work and scanout leave the decoder idle at each GOP. */
    (void) psp_media_feed_before_blocking(psp_active_media, false);
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
    psp_vprobe_video_composed(&probe);
    /* The preceding feed may finish during overlay composition. Give its
       successor the impending vblank wait without claiming another picture
       or changing either display buffer's ownership. */
    (void) psp_media_feed_before_blocking(psp_active_media, false);
    bool published = psp_media_wait_video_due(psp_active_media)
        && psp_display_publish_serviced(
        &psp_display, psp_media_service_scanout_wait, psp_active_media);
    int restored = psp_thread_priority_restore(previous_priority);
    if (restored < 0)
        printf("tilefinch-media-present: priority-restore=0x%08X\n",
               (unsigned) restored);
    psp_vprobe_video_publish_returned(published);
    if (published) {
        psp_media_presented(psp_active_media);
        psp_media_menu_deferred_us = 0;
        psp_media_front_menu_key = feedback_key;
        psp_media_front_menu_epoch = feedback_epoch;
        psp_media_front_menu_generation = psp_active_media->generation;
        if (!menu_feedback) {
            psp_media_front_frame_epoch = psp_active_media->frame.epoch;
            psp_media_front_frame_identity = psp_active_media->frame.identity;
        }
    }
    psp_vprobe_video_published(&probe, published, menu_feedback, vram);
    return published;
}

static bool psp_media_seek_holds_scanout(const PspMediaSession *media)
{
    if (media == NULL
        || psp_media_continuation_resuming(&media->continuation))
        return false;
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
    psp_vprobe_player_start(media_replaces_page);
    if (psp_media_video_surface_follow(media_replaces_page)) {
        bool published = psp_present_video_surface();
        /* Still on the video surface means it owned this frame, published or
           refused by the display service. Handed back means the engine
           refused after entry: fall through and let the software presenter
           draw the same frame into the 16-bit surface. */
        if (psp_display_video_active(&psp_display)) return published;
    }
    /* Surface refusal takes the ordinary fallback; do not label its absent
       video/overlay boundaries as valid first-picture samples. */
    psp_vprobe_player_start_abandon();
    uint16_t *vram = psp_display_back_buffer(&psp_display);
    if (vram == NULL) return false;
    PspVprobePhase phase;
    psp_vprobe_phase_begin(&phase);
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
        psp_vprobe_phase_base_bytes(
            &phase, PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT * sizeof(*vram));
    } else if (!media_replaces_page && !native_surface) {
        unsigned opaque_top = 0u, opaque_bottom = 0u;
        psp_ui_opaque_chrome_rows(ui, &opaque_top, &opaque_bottom);
        unsigned copy_end = PSP_SCREEN_HEIGHT - opaque_bottom;
        int ge_copied = (int) canvas_ge_presenter_publish(
            &psp_present_canvas_ge,
            psp_present_canvas_engine == NULL ? NULL
                : browser_engine_render_shell(psp_present_canvas_engine),
            frame, vram, PSP_VRAM_STRIDE, PSP_SCREEN_WIDTH,
            opaque_top, copy_end,
            sceKernelGetThreadId() == atomic_load_explicit(
                &psp_present_vfpu_owner, memory_order_relaxed));
        if (ge_copied < 0) return false;
        if (ge_copied == 0) {
            bool vfpu_eligible = sceKernelGetThreadId() == atomic_load_explicit(
                &psp_present_vfpu_owner, memory_order_relaxed);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            vfpu_eligible = vfpu_eligible && psp_present_vfpu_copy_enabled;
            if (vfpu_eligible && psp_present_vfpu_copy_verify
                && opaque_top < copy_end) {
                psp_present_vfpu_checks++;
                unsigned errors = psp_display_validation_probe_row_copy(
                    &psp_display, frame, opaque_top, copy_end);
                psp_present_vfpu_errors |= errors;
                if (errors != 0u) {
                    psp_present_vfpu_copy_enabled = false;
                    return false;
                }
                psp_present_vfpu_copies++;
            } else
#endif
            {
                PspDisplayRowCopyResult copied = psp_display_copy_rgb565_rows(
                    &psp_display, frame, opaque_top, copy_end,
                    vfpu_eligible);
                if (copied < 0) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                if (copied == PSP_DISPLAY_ROW_COPY_VFPU)
                    psp_present_vfpu_copies++;
#endif
            }
        }
        psp_vprobe_phase_copied_rows(&phase, opaque_top, copy_end);
    }
    psp_vprobe_phase_base_done(&phase);
    if (media_visible) {
        if (media_replaces_page) {
            bool overlay_paints =
                psp_ui_media_overlay_paints(&psp_active_media->ui);
            psp_vprobe_video_skipped(false);
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
        psp_vprobe_phase_ui_composited(&phase);
    }
    /* Publish only a completely composed back buffer at vblank. Writing the
       scanout buffer directly caused visibly half-updated title bars and
       loading flicker on physical hardware. */
    psp_vprobe_phase_composed(&phase, native_surface);
    bool published = (!media_replaces_page
        || psp_media_wait_video_due(psp_active_media))
        && psp_display_publish(&psp_display);
    psp_vprobe_phase_published(
        &phase, published, media_visible, media_replaces_page, frame, ui,
        vram);
    if (published && media_replaces_page)
        psp_media_presented(psp_active_media);
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
static uint64_t psp_completed_supervisor_page_input_us[
    PSP_SUPERVISOR_PAGE_INPUT_LIMIT];
static uint8_t psp_completed_supervisor_page_input_count;
volatile unsigned psp_validation_cancel_after_ms;
volatile unsigned psp_validation_preview_scroll;

void psp_platform_retire_frame(void *context, const uint16_t *pixels)
{
    PspNavigationCooperate *cooperate = context;
    if (cooperate == NULL || pixels == NULL
        || cooperate->page_frame != pixels) return;
    cooperate->page_frame = NULL;
    __sync_synchronize();
    /* A callback-thread present may have read the pointer already; it holds
       presenting for one frame copy and a vblank. Wait it out however long
       it takes (as psp_navigation_cooperate_end does): freeing after a time
       limit would let a delayed display operation read freed pixels. The
       browser thread never holds presenting here (its presents return
       before the engine frees). */
    unsigned waited_ms = 0;
    while (cooperate->presenting != 0) {
        (void) sceKernelDelayThread(1000);
        if ((++waited_ms & 63u) == 0) psp_log_heartbeat();
    }
}

/* What a supervisor present repaints under its chrome: the load preview
   while one is on screen, else the incumbent page. */
static const uint16_t *psp_supervisor_page_frame(
    const PspNavigationCooperate *cooperate)
{
    const uint16_t *preview = cooperate->page_frame;
    return preview != NULL ? preview : cooperate->frame;
}

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
    /* Completion-time raster publication shares the owner supervisor's
       latest menu/chrome state. The owner copy is adopted only on scope
       exit; presenting it here would undo an already acknowledged Select. */
    const PspUiState *shown_ui = cooperate->owner_thread_only
        ? &cooperate->supervisor_ui : cooperate->ui;
    bool shown = psp_present_internal(pixels, shown_ui, false);
    cooperate->last_present_us = sceKernelGetSystemTimeWide();
    if (shown) {
        cooperate->presentations++;
        /* Engine presents come from the browser thread: remember a load
           preview frame so periodic presents keep showing it. */
        cooperate->page_frame = pixels == cooperate->frame ? NULL : pixels;
    }
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
    psp_navigation_cooperate.page_frame = NULL;
    psp_navigation_cooperate.provisional_fill_us = 0;
    psp_navigation_cooperate.forwarded_head = 0;
    psp_navigation_cooperate.forwarded_tail = 0;
    psp_navigation_cooperate.owner_frame_us = 0;
    psp_navigation_cooperate.owner_present_us = 0;
    psp_navigation_cooperate.owner_page_screen = 1;
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
    bool optional_preemptible;
    bool forward_awaited;
    /* Inside a provisional layout completion of this engine, whose
       checkpoints serve page scrolling; serving guards reentry. */
    BrowserEngine *engine;
    bool completion;
    bool serving;
    bool painting;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The oldest press being served, and when its move was first shown,
       for tilefinch-scroll-feedback. */
    uint64_t served_press_us;
    uint64_t served_visible_us;
#endif
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
    psp_runtime_cooperate.optional_preemptible = false;
    psp_runtime_cooperate.forward_awaited = false;
    psp_runtime_cooperate.engine = NULL;
    psp_runtime_cooperate.completion = false;
    psp_runtime_cooperate.serving = false;
    psp_runtime_cooperate.painting = false;
    /* A short render may never activate the lazy supervisor. Its caller
       still needs a fresh cancellation token, not a prior scope's refusal. */
    tilefinch_cancellation_init(&psp_navigation_cooperate.cancellation);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_runtime_cooperate.served_press_us = 0;
    psp_runtime_cooperate.served_visible_us = 0;
#endif
    atomic_store_explicit(&psp_runtime_cooperate.owner_thread,
                          sceKernelGetThreadId(), memory_order_relaxed);
    atomic_store_explicit(&psp_runtime_cooperate.armed, 1u, memory_order_release);
}

void psp_runtime_cooperate_serve_page_scroll(BrowserEngine *engine)
{
    psp_runtime_cooperate.engine = engine;
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

/* Presses that only move the timeline; the session can record them as
   intent while an open is still running. */
static bool psp_media_timeline_action(PspUiMediaAction action)
{
    return action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK
        || action == PSP_UI_MEDIA_ACTION_SEEK
        || action == PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW
        || action == PSP_UI_MEDIA_ACTION_PLAY_PAUSE;
}

bool psp_work_cooperate_refresh_media(const PspUiMediaState *media_ui,
                                      PspUiMediaIntent *timeline_intent)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    bool taken = false;
    if (media_ui == NULL || cooperate->active == 0
        || !cooperate->media_surface
        || tilefinch_cancellation_requested(&cooperate->cancellation)
        || !__sync_bool_compare_and_swap(
               &cooperate->presenting, 0u, 1u)) return false;
    __sync_synchronize();
    if (cooperate->active != 0 && cooperate->media_surface
        && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        PspUiMediaIntent pending = cooperate->pending_media_intent;
        cooperate->supervisor_media_ui = *media_ui;
        /*
         * An open can run for seconds (a rewind re-resolves its URLs). Hand
         * timeline presses to the browser thread now rather than at the end
         * of the scope, so the session's recorded target and this snapshot
         * agree. The press's own visual is carried onto the fresh snapshot:
         * the browser thread shows it from the next frame on.
         */
        if (timeline_intent != NULL
            && psp_media_timeline_action(pending.action)) {
            cooperate->pending_media_intent = (PspUiMediaIntent) {0};
            PspUiMediaState *snapshot = &cooperate->supervisor_media_ui;
            if (pending.action == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK)
                psp_ui_media_set_seek_preview(snapshot, pending.seek_time_us);
            else if (pending.action == PSP_UI_MEDIA_ACTION_SEEK)
                psp_ui_media_commit_seek(snapshot, pending.seek_time_us);
            else if (pending.action
                         == PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW)
                psp_ui_media_cancel_seek_preview(snapshot);
            else
                snapshot->playing = !snapshot->playing;
            *timeline_intent = pending;
            taken = true;
        }
    }
    __sync_synchronize();
    cooperate->presenting = 0;
    return taken;
}

/* Hold the supervisor's presentation fence around a chrome glyph cache
   change. A supervisor frame draws straight from that cache, so replacing
   or filling it while a presentation is in flight would let the callback
   thread read freed or half-built glyph storage. The wait is one
   presentation at most (a framebuffer copy and a vblank). */
static bool psp_presentation_fence_acquire(void)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    if (cooperate->active == 0) return false;
    unsigned waited_ms = 0;
    while (!__sync_bool_compare_and_swap(&cooperate->presenting, 0u, 1u)) {
        (void) sceKernelDelayThread(1000);
        if ((++waited_ms & 63u) == 0) psp_log_heartbeat();
    }
    __sync_synchronize();
    return true;
}

static void psp_presentation_fence_release(bool held)
{
    if (!held) return;
    __sync_synchronize();
    psp_navigation_cooperate.presenting = 0;
}

void psp_presentation_rebind_chrome_fonts(
    const FontFace *regular, const FontFace *bold, unsigned scale)
{
    bool held = psp_presentation_fence_acquire();
    psp_ui_set_chrome_fonts(regular, bold, scale);
    psp_presentation_fence_release(held);
}

void psp_presentation_preload_chrome_scale(unsigned scale)
{
    bool held = psp_presentation_fence_acquire();
    psp_ui_preload_chrome_scale(scale);
    psp_presentation_fence_release(held);
}

void psp_work_cooperate_begin_media_open(
    PspUiState *ui, const uint16_t *engine_frame,
    const PspUiMediaState *media_ui)
{
    if (ui == NULL || media_ui == NULL) return;
    /* Populate playback's actual weights/buttons before the supervisor draws,
       without preparing an unused large bold alphabet on every first open. */
    bool held = psp_presentation_fence_acquire();
    PspVprobeFonts fonts_probe;
    psp_vprobe_fonts_begin(&fonts_probe);
    psp_ui_preload_media_chrome();
    psp_vprobe_fonts_end(&fonts_probe);
    psp_presentation_fence_release(held);
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, PSP_MEDIA_OPEN_STATUS, 600);
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

void psp_work_cooperate_show_status(void *context, const char *status)
{
    (void) context;
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    /* The supervisor owns its UI copy while presenting; a progress line
       that loses that race is replaced by the next one anyway. */
    if (status == NULL || cooperate->active == 0
        || tilefinch_cancellation_requested(&cooperate->cancellation)
        || !__sync_bool_compare_and_swap(&cooperate->presenting, 0u, 1u))
        return;
    /* Cancellation may have been shown since the check above. */
    if (!tilefinch_cancellation_requested(&cooperate->cancellation))
        psp_ui_show_status(&cooperate->supervisor_ui, status, 600);
    cooperate->last_present_us = 0;
    __sync_synchronize();
    cooperate->presenting = 0;
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


/* Page presses on the load preview, bounded: each one moves a page when
   the next checkpoint applies it, and a burst beyond the bound is the
   reader's intent to go far, which four pages already serve. */
enum { PSP_PROVISIONAL_SCROLL_QUEUE = 4 };

static void psp_queue_provisional_scroll(
    PspNavigationCooperate *cooperate, int direction)
{
    for (;;) {
        int queued = cooperate->provisional_scroll_requests;
        int next = queued + direction;
        if (next > PSP_PROVISIONAL_SCROLL_QUEUE
            || next < -PSP_PROVISIONAL_SCROLL_QUEUE) return;
        if (__sync_bool_compare_and_swap(
                &cooperate->provisional_scroll_requests, queued, next))
            return;
    }
}

/* Work that serves page presses has begun (a relayout's build, after its
   images loaded): the leading page presses queued for the page meanwhile
   are served from its checkpoints rather than replayed after it. A queued
   press of any other kind keeps its place and everything after it. */
static void psp_serve_queued_page_scrolls(PspNavigationCooperate *cooperate)
{
    if (!cooperate->active || !cooperate->owner_thread_only
        || !__sync_bool_compare_and_swap(&cooperate->presenting, 0u, 1u))
        return;
    uint8_t served = 0;
    while (served < cooperate->pending_page_input_count) {
        uint32_t pressed = cooperate->pending_page_input[served];
        if (pressed == 0
            || (pressed & ~PSP_INPUT_ROUTE_SCROLL_BUTTONS) != 0) break;
        psp_queue_provisional_scroll(
            cooperate,
            (pressed & (PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_PAGE_DOWN)) != 0
                ? 1 : -1);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t pressed_us = cooperate->pending_page_input_us[served];
        if (psp_runtime_cooperate.served_press_us == 0
            || pressed_us < psp_runtime_cooperate.served_press_us)
            psp_runtime_cooperate.served_press_us = pressed_us;
#endif
        served++;
    }
    if (served != 0) {
        uint8_t kept = cooperate->pending_page_input_count - served;
        memmove(&cooperate->pending_page_input[0],
                &cooperate->pending_page_input[served],
                kept * sizeof(cooperate->pending_page_input[0]));
        memmove(&cooperate->pending_page_input_us[0],
                &cooperate->pending_page_input_us[served],
                kept * sizeof(cooperate->pending_page_input_us[0]));
        cooperate->pending_page_input_count = kept;
    }
    __sync_synchronize();
    cooperate->presenting = 0;
}

bool psp_request_provisional_scroll(
    BrowserEngine *engine, PspUiState *ui, int delta, bool page)
{
    if (engine == NULL || delta == 0
        || !browser_engine_navigation_pending(engine)) {
        return false;
    }
    int direction = delta > 0 ? 1 : -1;
    bool moved;
    if (page) {
        psp_load_scroll_note_press();
        moved = browser_engine_scroll_provisional_page(engine, direction);
        if (moved) psp_load_scroll_note_moved();
    } else {
        moved = browser_engine_scroll_provisional_by(engine, delta);
    }
    if (!moved && page && psp_navigation_cooperate.active != 0
        && psp_navigation_cooperate.engine == engine) {
        /* No preview yet, or the reader is at its end: apply the press when
           the next checkpoint finds room (a deeper preview). Analog motion
           is continuous and simply keeps coming. */
        psp_queue_provisional_scroll(&psp_navigation_cooperate, direction);
    }
    /* No toast: a moved preview is its own feedback, and the loading
       indicator already says the page is incomplete. Toasts here were set
       on both the supervisor's UI copy and the page UI with different text,
       so alternating presents replayed the toast's entry motion. */
    (void) ui;
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
    psp_navigation_cooperate.page_frame = NULL;
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
                memmove(
                    &psp_completed_supervisor_page_input_us[0],
                    &psp_completed_supervisor_page_input_us[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(psp_completed_supervisor_page_input_us[0]));
                psp_completed_supervisor_page_input_count--;
            }
            psp_completed_supervisor_page_input_us[
                psp_completed_supervisor_page_input_count] =
                    psp_navigation_cooperate.pending_page_input_us[at];
            psp_completed_supervisor_page_input[
                psp_completed_supervisor_page_input_count++] =
                    psp_navigation_cooperate.pending_page_input[at];
        }
        /* Presses forwarded to a loading page that the browser loop had
           not taken when the load ended belong to the page it ended on. */
        const unsigned forwarded_capacity =
            sizeof(psp_navigation_cooperate.forwarded_pressed)
            / sizeof(psp_navigation_cooperate.forwarded_pressed[0]);
        while (psp_navigation_cooperate.forwarded_tail
                   != psp_navigation_cooperate.forwarded_head) {
            if (psp_completed_supervisor_page_input_count
                    == PSP_SUPERVISOR_PAGE_INPUT_LIMIT) {
                memmove(
                    &psp_completed_supervisor_page_input[0],
                    &psp_completed_supervisor_page_input[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(psp_completed_supervisor_page_input[0]));
                memmove(
                    &psp_completed_supervisor_page_input_us[0],
                    &psp_completed_supervisor_page_input_us[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(psp_completed_supervisor_page_input_us[0]));
                psp_completed_supervisor_page_input_count--;
            }
            psp_completed_supervisor_page_input_us[
                psp_completed_supervisor_page_input_count] = 0;
            psp_completed_supervisor_page_input[
                psp_completed_supervisor_page_input_count++] =
                    psp_navigation_cooperate.forwarded_pressed[
                        psp_navigation_cooperate.forwarded_tail
                        % forwarded_capacity];
            psp_navigation_cooperate.forwarded_tail++;
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

uint32_t psp_navigation_cooperate_owner_frame(bool page_screen)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    if (cooperate->active == 0 || cooperate->supervised == 0
        || cooperate->engine == NULL) return 0;
    cooperate->owner_page_screen = page_screen ? 1u : 0u;
    cooperate->owner_frame_us = sceKernelGetSystemTimeWide();
    unsigned tail = cooperate->forwarded_tail;
    if (tail == cooperate->forwarded_head) return 0;
    __sync_synchronize();
    uint32_t pressed = cooperate->forwarded_pressed[
        tail % (sizeof(cooperate->forwarded_pressed)
                / sizeof(cooperate->forwarded_pressed[0]))];
    __sync_synchronize();
    cooperate->forwarded_tail = tail + 1u;
    return pressed;
}

bool psp_navigation_cooperate_owner_present(const PspUiState *ui)
{
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    if (ui == NULL || cooperate->active == 0
        || !__sync_bool_compare_and_swap(&cooperate->presenting, 0u, 1u))
        return false;
    __sync_synchronize();
    bool shown = cooperate->active != 0
        && psp_present_internal(psp_supervisor_page_frame(cooperate),
                                ui, false);
    if (shown) {
        uint64_t now_us = sceKernelGetSystemTimeWide();
        cooperate->last_present_us = now_us;
        cooperate->owner_present_us = now_us;
        cooperate->presentations++;
    }
    __sync_synchronize();
    cooperate->presenting = 0;
    return shown;
}

bool psp_navigation_cooperate_page_input_pending(void)
{
    /* The owner reads only the completed FIFO, after the scope fence. */
    return psp_completed_supervisor_page_input_count != 0;
}

bool psp_navigation_cooperate_take_page_input(uint32_t *pressed,
                                              uint64_t *pressed_us)
{
    if (pressed == NULL || pressed_us == NULL
        || psp_completed_supervisor_page_input_count == 0) return false;
    *pressed = psp_completed_supervisor_page_input[0];
    *pressed_us = psp_completed_supervisor_page_input_us[0];
    psp_completed_supervisor_page_input_count--;
    if (psp_completed_supervisor_page_input_count != 0) {
        memmove(
            &psp_completed_supervisor_page_input[0],
            &psp_completed_supervisor_page_input[1],
            psp_completed_supervisor_page_input_count
                * sizeof(psp_completed_supervisor_page_input[0]));
        memmove(
            &psp_completed_supervisor_page_input_us[0],
            &psp_completed_supervisor_page_input_us[1],
            psp_completed_supervisor_page_input_count
                * sizeof(psp_completed_supervisor_page_input_us[0]));
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-ui-supervisor-input: replay=0x%04x remaining=%u\n",
           (unsigned) *pressed,
           (unsigned) psp_completed_supervisor_page_input_count);
#endif
    return true;
}

static void psp_work_ui_tick(bool owner_thread);

/* Supervisor side of the press queue; a full queue drops the press (the
   loop is far behind, and eight presses already describe the intent). */
static void psp_forward_owner_press(
    PspNavigationCooperate *cooperate, uint32_t pressed)
{
    const unsigned capacity = sizeof(cooperate->forwarded_pressed)
        / sizeof(cooperate->forwarded_pressed[0]);
    unsigned head = cooperate->forwarded_head;
    if (head - cooperate->forwarded_tail >= capacity) return;
    cooperate->forwarded_pressed[head % capacity] = pressed;
    __sync_synchronize();
    cooperate->forwarded_head = head + 1u;
}

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
        PSP_VALIDATION_KNOB(psp_validation_cancel_after_ms);
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
    if (!analog_active) psp_cursor_latency_idle();
    bool priority_handled = false;
    if (owner_thread && cooperate->engine == NULL
        && !cooperate->media_surface && !cooperate->media_detached
        && !cooperate->supervisor_ui.page_gamepad_capture) {
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
        uint8_t queued_before = cooperate->pending_page_input_count;
        priority_handled = psp_ui_update_priority_with_page_queue(
            &cooperate->supervisor_ui, &native_input,
            &cooperate->pending_page_input_count);
        bool native_visual_changed = priority_handled;
        cooperate->pending_page_input_dropped +=
            queued_before - cooperate->pending_page_input_count;
        /* A held Triangle is consumed by the gesture, not replayed as a
           fresh press on return to the main loop. Reader dispatch waits for
           the page's borrowed state to be released. */
        priority_handled |= toolbar_tracking;
        /* Tracking consumes Triangle without changing presentation. Do not
           spend a framebuffer copy/vblank on the still-held button: the tap
           toggles on release, and a hold dispatches Reader only afterward. */
        if (native_visual_changed) {
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
    PspInputRouteContext route_context = {
        .owner = cooperate->media_surface ? PSP_INPUT_OWNER_MEDIA
            : cooperate->engine != NULL ? PSP_INPUT_OWNER_LOADING_PAGE
            : PSP_INPUT_OWNER_PAGE_SERVICE,
        .loop = !cooperate->owner_page_screen ? PSP_INPUT_LOOP_IN_MENU
            : cooperate->forwarded_head != cooperate->forwarded_tail
                ? PSP_INPUT_LOOP_BEHIND : PSP_INPUT_LOOP_ON_PAGE,
        .pressed = ui_pressed,
        .analog_active = analog_active,
        .owner_thread = owner_thread,
        .optional_preemptible = psp_runtime_cooperate.optional_preemptible,
        .forward_awaited = psp_runtime_cooperate.forward_awaited,
        .scroll_served = owner_thread && psp_runtime_cooperate.completion
            && psp_runtime_cooperate.engine != NULL,
        .priority_handled = priority_handled,
        .cancelling = tilefinch_cancellation_requested(
            &cooperate->cancellation),
        .acknowledge_busy = cooperate->acknowledge_non_cancel_busy,
        .media_preview_active = cooperate->media_surface
            && cooperate->supervisor_media_ui.seek_preview_active,
        .media_menu_open = cooperate->media_surface
            && cooperate->supervisor_media_ui.presentation != NULL
            && cooperate->supervisor_media_ui.presentation->track_menu_open,
        .page_gamepad_capture = cooperate->supervisor_ui.page_gamepad_capture
    };
    switch (psp_input_route(&route_context)) {
        case PSP_INPUT_ROUTE_YIELD:
            cooperate->input_yield = true;
            tilefinch_cancellation_request(&cooperate->cancellation);
            cooperate->cancellation_requested_us =
                sceKernelGetSystemTimeWide();
            if (ui_pressed != 0 && !priority_handled
                && cooperate->pending_page_input_count
                       < PSP_SUPERVISOR_PAGE_INPUT_LIMIT) {
                cooperate->pending_page_input_us[
                    cooperate->pending_page_input_count] =
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                        scripted ? psp_input_script_request_time(now_us) :
#endif
                        (uint64_t) sceKernelGetSystemTimeWide();
                cooperate->pending_page_input[
                    cooperate->pending_page_input_count++] = ui_pressed;
            }
            break;
        case PSP_INPUT_ROUTE_PRIORITY_DONE:
            /* Already applied; replaying Select would immediately close
               the menu. */
            break;
        case PSP_INPUT_ROUTE_FORWARD:
            /* The loading page's browser loop acts between its pumps:
               focus, activation, menus, and everything while it shows a
               menu or has not caught up (so Select then Circle closes the
               menu it opens rather than stopping the load). */
            psp_forward_owner_press(cooperate, ui_pressed);
            cooperate->input_acknowledgements++;
            break;
        case PSP_INPUT_ROUTE_CANCEL:
            tilefinch_cancellation_request(&cooperate->cancellation);
            cooperate->cancellation_requested_us =
                sceKernelGetSystemTimeWide();
            acknowledgement_started_us =
                cooperate->cancellation_requested_us;
            cooperate->input_acknowledgements++;
            psp_supervisor_show_status(cooperate, cooperate->cancel_status);
            urgent_present = true;
            break;
        case PSP_INPUT_ROUTE_MEDIA: {
            /*
             * A preview seek can spend several callback ticks in a firmware
             * or range unit. Dropping direction edges here made the player
             * appear unresponsive until that picture returned. Apply each
             * edge to the supervisor's private UI snapshot immediately and
             * retain one latest command for the browser thread. The
             * one-entry mailbox is deliberate: repeated directions are a
             * target, not N decoder transactions.
             */
            PspUiInput media_input = {
                .pressed = ui_pressed,
                .held = ui_pressed,
                .analog_x = 128,
                .analog_y = 128,
                .elapsed_ms = 16
            };
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            bool menu_was_open = cooperate->supervisor_media_ui.presentation != NULL
                && cooperate->supervisor_media_ui.presentation->track_menu_open;
            unsigned menu_old_tab = cooperate->supervisor_media_ui.presentation == NULL
                ? 0u : cooperate->supervisor_media_ui.presentation->track_menu_tab;
            uint32_t menu_pressed_us = (uint32_t) (scripted
                ? psp_input_script_request_time(now_us) : (uint64_t) sceKernelGetSystemTimeWide());
#endif
            supervisor_media_intent = psp_ui_media_update(
                &cooperate->supervisor_media_ui, &media_input);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_media_menu_feedback_input(&cooperate->supervisor_media_ui,
                menu_was_open, menu_old_tab, menu_pressed_us);
#endif
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
            break;
        }
        case PSP_INPUT_ROUTE_SCROLL: {
            int direction =
                (ui_pressed & (PSP_UI_BUTTON_DOWN
                               | PSP_UI_BUTTON_PAGE_DOWN)) != 0
                    ? 1 : -1;
            psp_queue_provisional_scroll(cooperate, direction);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            /* Load-time scroll telemetry counts the loading page only; a
               press served during a layout completion is timed instead. */
            if (cooperate->engine != NULL) psp_load_scroll_note_press();
            else if (psp_runtime_cooperate.served_press_us == 0)
                psp_runtime_cooperate.served_press_us =
                    sceKernelGetSystemTimeWide();
#endif
            cooperate->input_acknowledgements++;
            acknowledgement_started_us =
                sceKernelGetSystemTimeWide();
            urgent_present = true;
            break;
        }
        case PSP_INPUT_ROUTE_STILL_STOPPING:
            cooperate->input_acknowledgements++;
            acknowledgement_started_us =
                sceKernelGetSystemTimeWide();
            psp_supervisor_show_status(
                cooperate,
                cooperate->media_surface || cooperate->media_detached
                    ? "VIDEO IS STILL STOPPING"
                    : "STILL STOPPING - PLEASE WAIT");
            urgent_present = true;
            break;
        case PSP_INPUT_ROUTE_QUEUE_PAGE:
            /* The stable page is still authoritative while a raster or
               other bounded page service runs. Retain input for the
               browser-thread controller instead of acknowledging and
               discarding it. The queue is deliberately tiny; if a
               pathological service outlives a burst, prefer the newest four
               commands so the eventual state follows the user's latest
               intent. */
            if (cooperate->pending_page_input_count
                    == PSP_SUPERVISOR_PAGE_INPUT_LIMIT) {
                memmove(
                    &cooperate->pending_page_input[0],
                    &cooperate->pending_page_input[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(cooperate->pending_page_input[0]));
                memmove(
                    &cooperate->pending_page_input_us[0],
                    &cooperate->pending_page_input_us[1],
                    (PSP_SUPERVISOR_PAGE_INPUT_LIMIT - 1u)
                        * sizeof(cooperate->pending_page_input_us[0]));
                cooperate->pending_page_input_count--;
                cooperate->pending_page_input_dropped++;
            }
            cooperate->pending_page_input_us[
                cooperate->pending_page_input_count] =
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                    scripted ? psp_input_script_request_time(now_us) :
#endif
                    (uint64_t) sceKernelGetSystemTimeWide();
            cooperate->pending_page_input[
                cooperate->pending_page_input_count++] = ui_pressed;
            cooperate->input_acknowledgements++;
            acknowledgement_started_us = sceKernelGetSystemTimeWide();
            if (owner_thread && !cooperate->page_input_yield_boundary) {
                psp_ui_show_status(&cooperate->supervisor_ui,
                                  "PAGE UPDATE - INPUT QUEUED", 120);
                urgent_present = true;
            }
            break;
        case PSP_INPUT_ROUTE_BUSY:
            cooperate->input_acknowledgements++;
            acknowledgement_started_us =
                sceKernelGetSystemTimeWide();
            psp_ui_show_status(
                &cooperate->supervisor_ui,
                "BUSY - CIRCLE CANCELS", 120);
            urgent_present = true;
            break;
        case PSP_INPUT_ROUTE_NONE:
        default:
            break;
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
    /* At a resumable boundary the queued action itself can provide feedback
       as soon as this scope returns. A separate status publication would
       block that receiver on vblank, even though no page work remains owed. */
    if (!urgent_present && owner_thread
        && cooperate->page_input_yield_boundary
        && cooperate->pending_page_input_count != 0) {
        cooperate->presenting = 0;
        return;
    }
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
    /* A live browser loop presents the loading page itself, with its own
       cursor and menus; step in only once it has been busy a while. */
    if (!owner_thread && cooperate->engine != NULL
        && cooperate->owner_present_us != 0
        && now_us - cooperate->owner_present_us
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
                     psp_supervisor_page_frame(cooperate),
                     &cooperate->supervisor_ui)) {
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

/* A provisional page's layout completion is running on this thread (we
   are at one of its checkpoints): apply the page presses the supervisor
   queued to the committed page, fill what they uncovered a slice at a time,
   and present. The completion keeps its progress. */
/* Presents the engine frame a completion-time scroll or fill changed. */
static bool psp_runtime_present_completion_frame(
    PspNavigationCooperate *cooperate)
{
    bool shown = psp_platform_present(
        cooperate, psp_runtime_cooperate.frame, PSP_SCREEN_WIDTH,
        PSP_SCREEN_HEIGHT, PSP_SCREEN_WIDTH);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The same line a main-loop page press reports: press to the first
       frame at the new position, and to a complete one. */
    uint64_t pressed_us = psp_runtime_cooperate.served_press_us;
    if (pressed_us != 0 && shown) {
        uint64_t shown_us = sceKernelGetSystemTimeWide();
        if (psp_runtime_cooperate.served_visible_us == 0)
            psp_runtime_cooperate.served_visible_us = shown_us;
        if (!browser_engine_completion_frame_pending(
                psp_runtime_cooperate.engine)) {
            printf("tilefinch-scroll-feedback: action=completion "
                   "elapsed=%lluus first-visible=%lluus\n",
                   (unsigned long long) (shown_us - pressed_us),
                   (unsigned long long)
                       (psp_runtime_cooperate.served_visible_us
                        - pressed_us));
            psp_runtime_cooperate.served_press_us = 0;
            psp_runtime_cooperate.served_visible_us = 0;
        }
    }
#endif
    return shown;
}

/* Applies queued page presses to the committed page; true when it moved. */
static bool psp_runtime_apply_completion_scrolls(
    PspNavigationCooperate *cooperate, BrowserEngine *engine)
{
    int requests = __sync_lock_test_and_set(
        &cooperate->provisional_scroll_requests, 0);
    int direction = requests > 0 ? 1 : -1;
    bool moved = false;
    /* A press at the end of the laid-out extent has nowhere to go until
       the completion adopts; it is dropped rather than replayed late. */
    for (int remaining = requests * direction; remaining > 0; remaining--) {
        if (!browser_engine_completion_scroll_page(engine, direction)) break;
        moved = true;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (requests != 0 && !moved
        && psp_runtime_cooperate.served_visible_us == 0)
        psp_runtime_cooperate.served_press_us = 0;
#endif
    /* The move first: resident tiles and a checkerboard, at once. */
    if (moved) (void) psp_runtime_present_completion_frame(cooperate);
    return moved;
}

static void psp_runtime_serve_completion_scroll(
    PspNavigationCooperate *cooperate, uint64_t now_us)
{
    BrowserEngine *engine = psp_runtime_cooperate.engine;
    if (!browser_engine_layout_completion_running(engine)
        || tilefinch_cancellation_requested(&cooperate->cancellation))
        return;
    psp_runtime_cooperate.serving = true;
    (void) psp_runtime_apply_completion_scrolls(cooperate, engine);
    /* Then the screen the reader paged to is filled before the completion
       continues: its checkerboard is what they are looking at, and the
       completion's next checkpoint can be hundreds of milliseconds away
       (root style, bidi). Input is still sampled every 16 ms and further
       presses applied; after 500 ms the completion gets its turn. */
    uint64_t fill_started_us = now_us;
    uint64_t progress_shown_us = now_us;
    while (browser_engine_completion_frame_pending(engine)
           && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        /* The tile boundary deliberately defers to queued focus/activation.
           Spinning here cannot make progress while that same input waits
           for the outer layout to return; resume filling after its receiver. */
        if (cooperate->pending_page_input_count != 0
            || psp_completed_supervisor_page_input_count != 0) break;
        uint64_t fill_now_us = sceKernelGetSystemTimeWide();
        if (fill_now_us - fill_started_us >= UINT64_C(500000)) break;
        /* A partial frame is a full recompose: show one every 50 ms. */
        bool show_progress =
            fill_now_us - progress_shown_us >= UINT64_C(50000);
        if (browser_engine_completion_raster_step(engine, show_progress)) {
            (void) psp_runtime_present_completion_frame(cooperate);
            progress_shown_us = fill_now_us;
        }
        if (fill_now_us - psp_runtime_cooperate.last_poll_us
                >= UINT64_C(16000)) {
            psp_runtime_cooperate.last_poll_us = fill_now_us;
            psp_work_ui_tick(true);
            (void) psp_runtime_apply_completion_scrolls(cooperate, engine);
        }
    }
    cooperate->provisional_fill_us = now_us;
    psp_runtime_cooperate.serving = false;
}

/* Parse-time page JavaScript has a bounded allowance (twenty seconds); a
   page that exhausts it keeps its server content but loses JavaScript. Once
   a load has spent a few seconds of it, say so, so a long quiet wait reads
   as work rather than a hang. */
void psp_show_page_script_clock(PspUiState *ui, const BrowserEngine *engine)
{
    uint64_t used_us = 0, limit_us = 0;
    if (ui == NULL
        || !browser_engine_parser_script_time(engine, &used_us, &limit_us)
        || limit_us == 0 || used_us < UINT64_C(3000000)) return;
    /* Microseconds >> 10 is ~1.024 ms; 977 of those make a second. Keeps
       software 64-bit division out of the frame path. */
    uint32_t used_s = (uint32_t) (used_us >> 10) / 977u;
    uint32_t limit_s = (uint32_t) (limit_us >> 10) / 977u;
    char text[40];
    snprintf(text, sizeof(text), "PAGE SCRIPTS %uS OF %uS",
             (unsigned) used_s, (unsigned) limit_s);
    psp_ui_keep_progress_status(ui, text, 12u, 45u);
}

static void psp_runtime_cooperate_activate(PspNavigationCooperate *cooperate)
{
    if (cooperate->active) return;
    psp_work_cooperate_begin_internal(
        psp_runtime_cooperate.ui, psp_runtime_cooperate.frame,
        psp_runtime_cooperate.ui->page_activation_busy,
        true, false, "STOPPING PAGE UPDATE...",
        "page-runtime", NULL, NULL, true);
    /* Include work before the first checkpoint in gap accounting. */
    cooperate->started_us = psp_runtime_cooperate.started_us;
    cooperate->last_checkpoint_us = psp_runtime_cooperate.started_us;
    cooperate->last_present_us = psp_runtime_cooperate.started_us;
}

/* Resumable owner boundaries: queued page input defers the next unit. */
#define PSP_COOPERATE_YIELD_BOUNDARY \
    (TILEFINCH_COOPERATE_PAGE_TASK | TILEFINCH_COOPERATE_RENDER_INPUT \
     | TILEFINCH_COOPERATE_IMAGE_ADMISSION)

bool psp_platform_cooperate(
    void *context, const char *phase, size_t completed_work_units,
    unsigned boundary)
{
    PspNavigationCooperate *cooperate = context;
    if (cooperate == &psp_navigation_cooperate
        && atomic_load_explicit(&psp_runtime_cooperate.armed,
                                memory_order_acquire)) {
        /* Reject worker hooks before reading any owner-only borrowed state. */
        if (sceKernelGetThreadId() != atomic_load_explicit(
                &psp_runtime_cooperate.owner_thread, memory_order_relaxed))
            return true;
        if ((boundary & TILEFINCH_COOPERATE_RENDER_COMPOSE) != 0) {
            psp_runtime_cooperate.painting = completed_work_units != 0;
            return true;
        }
        if (psp_runtime_cooperate.painting) return true;
        if ((boundary & TILEFINCH_COOPERATE_IMAGE_ADMISSION) != 0) {
            /* Request setup can include synchronous cache/replay I/O.
               Arm before it starts, not at an eight-millisecond checkpoint
               that admission may never reach. Native menu/chrome/cursor
               feedback uses the immutable incumbent; page input queues
               until the next safe admission boundary. */
            psp_runtime_cooperate_activate(cooperate);
        }
        if ((boundary & TILEFINCH_COOPERATE_FOCUS_FEEDBACK) != 0) {
            /* This is often the first safe point of a focus move. The
               ordinary eight-millisecond lazy arm has not fired yet. */
            psp_runtime_cooperate_activate(cooperate);
            BrowserEngine *engine = psp_runtime_cooperate.engine;
            PspUiState *ui = psp_runtime_cooperate.ui;
            if (engine != NULL && ui != NULL
                && __sync_bool_compare_and_swap(&cooperate->presenting, 0u, 1u)) {
                __sync_synchronize();
                /* Use the supervisor's latest menu/cursor state and its
                   presentation fence, not a second concurrent VRAM writer.
                   Updating only focus preserves any priority input already
                   acknowledged while the owner was selecting the target. */
                PspUiState *shown_ui = &cooperate->supervisor_ui;
                BrowserFrontendSnapshot snapshot = {0};
                bool sampled = cooperate->active != 0
                    && shown_ui->screen == PSP_UI_SCREEN_PAGE
                    && !shown_ui->cursor_visible
                    && browser_engine_frontend_snapshot(engine, &snapshot);
                const NavigationSession *nav = sampled ? snapshot.navigation : NULL;
                int entry_scroll = 0;
                bool has_entry = nav != NULL && browser_engine_current_entry(
                    engine, NULL, 0u, &entry_scroll);
                int x = 0, y = 0, width = 0, height = 0;
                if (has_entry && browser_engine_focus_indicator_rect(
                        engine, &x, &y, &width, &height)) {
                    y -= entry_scroll;
                    /* The incumbent pixels haven't scrolled yet. An offscreen
                       target is published by the normal reveal/render path. */
                    if (y >= 0 && (int64_t) y + height <= nav->viewport.css_height) {
                        psp_ui_set_focus(shown_ui, true,
                            viewport_css_to_device(&nav->viewport, x),
                            viewport_css_to_device(&nav->viewport, y),
                            viewport_css_to_device(&nav->viewport, width),
                            viewport_css_to_device(&nav->viewport, height));
                        psp_ui_set_focus(ui, true, shown_ui->focus_x, shown_ui->focus_y,
                            shown_ui->focus_width, shown_ui->focus_height);
                        if (psp_present_internal(psp_supervisor_page_frame(cooperate),
                                                shown_ui, false)) {
                            uint64_t now_us = sceKernelGetSystemTimeWide();
                            psp_vprobe_focus_first_visible(engine, nav, now_us);
                            cooperate->last_present_us = now_us;
                            cooperate->owner_present_us = now_us;
                            cooperate->presentations++;
                        }
                    }
                }
                __sync_synchronize();
                cooperate->presenting = 0;
            }
            return !psp_home_exit_pending();
        }
        /* Optional work that holds no author state (a font batch, the rest
           of a provisional layout) yields to page input and reruns; see
           psp_input_route() for the extension a reader waits on. */
        if ((boundary & TILEFINCH_COOPERATE_SCROLL_SERVABLE) != 0) {
            /* Not optional: the build is kept, and only page presses are
               served at its checkpoints (the completion's route). */
            psp_runtime_cooperate.completion = completed_work_units != 0;
            if (psp_runtime_cooperate.completion
                && psp_runtime_cooperate.engine != NULL)
                psp_serve_queued_page_scrolls(cooperate);
            return true;
        }
        bool completion =
            (boundary & TILEFINCH_COOPERATE_LAYOUT_COMPLETION) != 0;
        bool awaited = completion
            && (boundary & TILEFINCH_COOPERATE_EXTENSION_AWAITED) != 0;
        if (completion
            || (boundary & TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION) != 0) {
            psp_runtime_cooperate.optional_preemptible = completed_work_units != 0;
            psp_runtime_cooperate.forward_awaited =
                awaited && completed_work_units != 0;
            psp_runtime_cooperate.completion =
                completion && completed_work_units != 0;
            if (completed_work_units == 0) return true;
            /* A press can have been queued during the preceding decode.
               Do not require a second physical edge to interrupt flow. */
            if ((boundary & TILEFINCH_COOPERATE_IMAGE_PUBLICATION) != 0
                && (cooperate->pending_page_input_count != 0
                    || psp_completed_supervisor_page_input_count != 0))
                return false;
        }
        uint64_t now = sceKernelGetSystemTimeWide();
        if (!cooperate->active
            && now - psp_runtime_cooperate.started_us >= UINT64_C(8000)) {
            /* The activation's acknowledgement was presented as it began,
               with the status this scope would show: the periodic present
               is due an interval after that, not at the first tick. */
            psp_runtime_cooperate_activate(cooperate);
        }
        /* Owner-only checkpoints borrow completed page pixels. Sample at
           half a display interval so setup or Triangle release does not
           consume an entire frame before native feedback. Author dispatch
           remains deferred until the borrowed page work returns. */
        uint64_t poll_interval_us = cooperate->owner_thread_only
            ? UINT64_C(8000) : UINT64_C(16000);
        if (cooperate->active && cooperate->owner_thread_only
            && (psp_runtime_cooperate.last_poll_us == 0
                || now - psp_runtime_cooperate.last_poll_us >= poll_interval_us)) {
            psp_runtime_cooperate.last_poll_us = now;
            cooperate->page_input_yield_boundary =
                (boundary & PSP_COOPERATE_YIELD_BOUNDARY) != 0;
            psp_work_ui_tick(true);
            cooperate->page_input_yield_boundary = false;
        }
        if (cooperate->active && psp_runtime_cooperate.completion
            && psp_runtime_cooperate.engine != NULL
            && !psp_runtime_cooperate.serving
            /* Cache preparation borrows the render shell. Never reenter
               it to apply a scroll from one of its own checkpoints. */
            && (boundary & TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED) == 0)
            psp_runtime_serve_completion_scroll(cooperate, now);
        if ((boundary & PSP_COOPERATE_YIELD_BOUNDARY) != 0) {
            /* These are resumable owner boundaries, not VM interrupts.
               Leave jobs and prepared tiles intact; the main loop dispatches
               queued focus only after the borrowed page work has returned. */
            return cooperate->pending_page_input_count == 0
                && psp_completed_supervisor_page_input_count == 0;
        }
    }
    if (cooperate == NULL || !cooperate->active) return true;
    if (cooperate->engine != NULL)
        psp_show_page_script_clock(cooperate->ui, cooperate->engine);
    if (psp_home_exit_pending()
        && !tilefinch_cancellation_requested(&cooperate->cancellation)) {
        tilefinch_cancellation_request(&cooperate->cancellation);
        cooperate->cancellation_requested_us =
            sceKernelGetSystemTimeWide();
        psp_ui_show_status(cooperate->ui, "CLOSING...", 600);
    }
    psp_log_heartbeat();
    psp_log_set_stage(phase == NULL ? "cooperate" : phase);
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
    if (PSP_VALIDATION_KNOB(psp_validation_preview_scroll != 0)
        && !cooperate->validation_preview_scroll_injected
        && cooperate->engine != NULL
        && !tilefinch_cancellation_requested(
            &cooperate->cancellation)) {
        BrowserProvisionalViewport viewport = {0};
        if (browser_engine_provisional_viewport(
                cooperate->engine, &viewport)
            && viewport.maximum_scroll_y > 0
            && viewport.scroll_y == 0
            && browser_engine_scroll_provisional_page(
                cooperate->engine, 1)) {
            cooperate->validation_preview_scroll_injected = true;
            cooperate->input_acknowledgements++;
            (void) browser_engine_provisional_viewport(
                cooperate->engine, &viewport);
            printf("tilefinch-preview-validation: scroll=page-down "
                   "y=%d/%d content-end=%d\n",
                   viewport.scroll_y, viewport.maximum_scroll_y,
                   viewport.content_end_y);
        }
    }
    int provisional_scroll_requests = __sync_lock_test_and_set(
        &cooperate->provisional_scroll_requests, 0);
    if (cooperate->engine != NULL
        && provisional_scroll_requests != 0
        && !tilefinch_cancellation_requested(
            &cooperate->cancellation)) {
        int direction = provisional_scroll_requests > 0 ? 1 : -1;
        int remaining = provisional_scroll_requests * direction;
        while (remaining > 0
               && browser_engine_scroll_provisional_page(
                      cooperate->engine, direction)) {
            remaining--;
            psp_load_scroll_note_moved();
        }
        /* No preview yet, or the reader reached its end: keep one press
           for the checkpoint after a deeper preview is painted. */
        if (remaining > 0)
            psp_queue_provisional_scroll(cooperate, direction);
    }
    /* Fill the tiles a move uncovered: a raster slice, then one recompose
       and present, at most once per frame interval. */
    uint64_t fill_now_us = sceKernelGetSystemTimeWide();
    if (cooperate->engine != NULL
        && !tilefinch_cancellation_requested(&cooperate->cancellation)
        && fill_now_us - cooperate->provisional_fill_us
               >= UINT64_C(16000)) {
        cooperate->provisional_fill_us = fill_now_us;
        (void) browser_engine_provisional_raster_step(cooperate->engine);
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
                psp_queue_provisional_scroll(cooperate, direction);
                psp_load_scroll_note_press();
                cooperate->input_acknowledgements++;
                acknowledgement_started_us =
                    sceKernelGetSystemTimeWide();
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
        psp_present_supervisor_ui(
            psp_supervisor_page_frame(cooperate), cooperate->ui);
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
    bool running = !tilefinch_cancellation_requested(&cooperate->cancellation);
    /* Input/presentation above wins first. This never raises transport
       priority and does nothing unless its setup call is still active. */
    if (running) fetch_background_transport_cooperate(now_us);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    psp_network_dns_guard();
#endif
    return running;
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
