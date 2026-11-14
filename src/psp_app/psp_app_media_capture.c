/* Page microphone capture on the PSP: the platform media_capture services
 * (include/tilefinch/platform.h). The engine has already decided who may
 * capture (tilefinch/media_capture.h); this owns the hardware, the
 * browser-owned prompt and the remembered per-site answers.
 *
 * The capture worker is the only thread that touches sceAudioInput while
 * page capture runs. It moves 1024-sample blocks at 44.1 kHz mono into a
 * fixed ring and publishes them with one atomic word; the browser thread
 * reads the newest samples for Web Audio and nothing else ever sees them.
 * Its buffers are charged to the browser's Budget by the browser thread
 * when capture starts, zeroed and returned when it stops: no audio is kept
 * anywhere, never on the Memory Stick and never on the network.
 *
 * The worker issues non-blocking transfers and polls their end with short
 * sleeps (media_capture_input_run, tilefinch/media_capture_input.h), so a
 * stop is noticed within one poll and the browser thread's bounded join
 * always returns. The firmware cannot cancel a transfer, so the block pair
 * it writes is freed only once a poll proved its last transfer ended; a
 * pair whose transfer was still busy when the worker left stays in custody
 * and is reused, and a start waits for that proof. A worker that fails or
 * stalls after it started ends capture with MEDIA_CAPTURE_END_DEVICE
 * (psp_mic_failed). A PSP without a working microphone (the
 * PSP-E1000 has none; earlier models need the headset) fails the first
 * block or delivers none within MEDIA_CAPTURE_START_WAIT_MS, and the page
 * gets NotFoundError. Voice typing shares the input's one-time
 * sceAudioInputInit, and stops page capture before it listens; it listens
 * only once psp_mic_input_free has seen the input idle (no worker left, no
 * pair whose transfer may still be written), and otherwise says the
 * microphone is busy.
 *
 * The prompt is a nested modal on the browser thread inside the page's
 * script, as page dialogs are (psp_app_page_dialog.c, whose rules hold
 * here): it holds the presentation fence, owns the pad, forces the real
 * address bar visible above it, and its open time is paused out of the
 * navigation and script watchdogs. HOME, suspend and lost presentation
 * dismiss it.
 */
#include "psp_app_internal.h"
#include "../psp_thread_contract.h"

#include <pspaudio.h>
#include <pspctrl.h>
#include <psphprm.h>
#include <pspdisplay.h>
#include <psputils.h>

#include <stdatomic.h>
#include <string.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/media_capture.h"
#include "tilefinch/media_capture_input.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/work_ledger.h"

/* A first block normally lands in about 23 ms. */
#define MEDIA_CAPTURE_START_WAIT_MS 600u
/* sceAudioInputInit's gain, as voice typing uses. */
#define MEDIA_CAPTURE_INPUT_GAIN 0x2d
#define MEDIA_CAPTURE_JOIN_US 250000u
_Static_assert(MEDIA_CAPTURE_INPUT_DRAIN_US + 2u * MEDIA_CAPTURE_INPUT_POLL_US
                   < MEDIA_CAPTURE_JOIN_US,
               "a stopped worker leaves its drain inside the join");

static struct {
    PspUiState *ui;
    PspEngineViews *views;
    PspInteractiveState *interactive;
    Budget *budget;
    BrowserProfile *profile;
    PspProfileStore *profile_store;
    bool *voice_microphone_initialized;
    const char *argv0;
    int owner_thread;
    /* The capture worker and what it owns while it runs. */
    int thread;
    int16_t *ring_storage;
    int16_t *blocks;
    MediaCaptureRing ring;
    /* A pair the input may still be writing (media_capture_input.h). */
    MediaCaptureInputCustody custody;
    atomic_bool stop;
    atomic_int worker_state;
    /* Set by the worker as it leaves: its last transfer never ended. */
    atomic_bool transfer_pending;
    /* The held pair is from a start whose input never delivered. */
    bool held_without_device;
    atomic_uint blocks_captured;
    atomic_uint peak;
    bool synthetic;
    bool microphone_initialized;
    /* A worker that did not end in its join keeps its buffers. */
    bool abandoned;
    uint64_t started_us;
} psp_mic = {.thread = -1};

/* What is on screen with the prompt over it. */
static PspUiState psp_mic_prompt_ui;

void psp_media_capture_bind(PspUiState *ui, PspEngineViews *views,
                            PspTextInputService *text_input,
                            PspInteractiveState *interactive,
                            PspBrowserResources *browser, const char *argv0)
{
    psp_mic.ui = ui;
    psp_mic.views = views;
    psp_mic.interactive = interactive;
    psp_mic.budget = browser->budget;
    psp_mic.profile = browser->profile;
    psp_mic.profile_store = &browser->profile_store;
#ifdef TILEFINCH_HAVE_PSP_VOICE
    psp_mic.voice_microphone_initialized =
        &text_input->voice.microphone_initialized;
#else
    (void) text_input;
    psp_mic.voice_microphone_initialized = NULL;
#endif
    psp_mic.argv0 = argv0;
    psp_mic.owner_thread = sceKernelGetThreadId();
}

static bool psp_mic_owner_thread(void)
{
    return psp_mic.budget != NULL
        && sceKernelGetThreadId() == psp_mic.owner_thread;
}

/* ---- the capture worker ------------------------------------------------ */

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation: a scripted run hears a 440 Hz tone instead of the host's
   microphone, at the hardware's pace. Never in a shipping EBOOT. */
static void psp_mic_synthetic_block(int16_t *block, uint32_t *phase)
{
    /* A 100-step sine table would be exact; a parabola is close enough for
       level checks and keeps this off the FPU. */
    for (unsigned at = 0; at < MEDIA_CAPTURE_BLOCK_SAMPLES; at++) {
        uint32_t step = (*phase += 440u * 65536u / MEDIA_CAPTURE_SAMPLE_RATE)
                        & 0xffffu;
        int32_t x = (int32_t) (step & 0x7fffu) - 0x4000;
        int32_t value = 0x4000 - (x * x) / 0x4000;
        block[at] = (int16_t) ((step & 0x8000u) ? -value : value);
    }
}
#endif

static void psp_mic_note_block(const int16_t *block)
{
    unsigned peak = 0;
    media_capture_block_levels(block, MEDIA_CAPTURE_BLOCK_SAMPLES, &peak,
                               NULL);
    unsigned previous = atomic_load_explicit(&psp_mic.peak,
                                             memory_order_relaxed);
    if (peak > previous)
        atomic_store_explicit(&psp_mic.peak, peak, memory_order_relaxed);
    atomic_fetch_add_explicit(&psp_mic.blocks_captured, 1u,
                              memory_order_relaxed);
}

/* sceAudioInput for media_capture_input_run, on the worker thread. */
static int psp_mic_input_begin(void *user, int16_t *block)
{
    (void) user;
    sceKernelDcacheWritebackInvalidateRange(
        block, MEDIA_CAPTURE_BLOCK_SAMPLES * sizeof(int16_t));
    return sceAudioInput(MEDIA_CAPTURE_BLOCK_SAMPLES,
                         MEDIA_CAPTURE_SAMPLE_RATE, block);
}

static int psp_mic_input_poll(void *user)
{
    (void) user;
    return sceAudioPollInputEnd();
}

static void psp_mic_input_sleep(void *user, uint32_t microseconds)
{
    (void) user;
    (void) sceKernelDelayThread(microseconds);
}

static void psp_mic_input_arrived(void *user, const int16_t *block)
{
    (void) user;
    /* The finished block came from the hardware: drop stale cache lines
       before reading it. */
    sceKernelDcacheInvalidateRange(
        block, MEDIA_CAPTURE_BLOCK_SAMPLES * sizeof(int16_t));
    media_capture_ring_publish(&psp_mic.ring, block,
                               MEDIA_CAPTURE_BLOCK_SAMPLES);
    psp_mic_note_block(block);
}

static bool psp_mic_input_stop_requested(void *user)
{
    (void) user;
    return atomic_load_explicit(&psp_mic.stop, memory_order_acquire);
}

static void psp_mic_input_running(void *user)
{
    (void) user;
    atomic_store_explicit(&psp_mic.worker_state, MEDIA_CAPTURE_INPUT_RUNNING,
                          memory_order_release);
}

static const MediaCaptureInputBackend psp_mic_input = {
    .begin = psp_mic_input_begin,
    .poll = psp_mic_input_poll,
    .sleep_us = psp_mic_input_sleep,
    .arrived = psp_mic_input_arrived,
    .stop_requested = psp_mic_input_stop_requested,
    .running = psp_mic_input_running
};

static int psp_mic_worker(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (psp_mic.synthetic) {
        uint32_t phase = 0;
        atomic_store_explicit(&psp_mic.worker_state,
                              MEDIA_CAPTURE_INPUT_RUNNING,
                              memory_order_release);
        while (!atomic_load_explicit(&psp_mic.stop, memory_order_acquire)) {
            psp_mic_synthetic_block(psp_mic.blocks, &phase);
            media_capture_ring_publish(&psp_mic.ring, psp_mic.blocks,
                                       MEDIA_CAPTURE_BLOCK_SAMPLES);
            psp_mic_note_block(psp_mic.blocks);
            (void) sceKernelDelayThread(23220);
        }
        atomic_store_explicit(&psp_mic.worker_state,
                              MEDIA_CAPTURE_INPUT_EXITED,
                              memory_order_release);
        return 0;
    }
#endif
    bool pending = false;
    MediaCaptureInputState state =
        media_capture_input_run(&psp_mic_input, psp_mic.blocks, &pending);
    atomic_store_explicit(&psp_mic.transfer_pending, pending,
                          memory_order_relaxed);
    atomic_store_explicit(&psp_mic.worker_state, state, memory_order_release);
    return 0;
}

/* ---- the services ------------------------------------------------------ */

/* After the worker ended (or never started): the ring goes back, and the
   pair too unless the input may still write it. */
static void psp_mic_release_buffers(void)
{
    if (psp_mic.abandoned) return;
    /* Nothing heard outlives the capture. */
    if (psp_mic.ring_storage != NULL)
        memset(psp_mic.ring_storage, 0,
               MEDIA_CAPTURE_RING_SAMPLES * sizeof(int16_t));
    budget_free(psp_mic.budget, psp_mic.ring_storage);
    psp_mic.ring_storage = NULL;
    psp_mic.held_without_device =
        atomic_load_explicit(&psp_mic.worker_state, memory_order_acquire)
        == MEDIA_CAPTURE_INPUT_NO_DEVICE;
    media_capture_input_pair_release(
        &psp_mic.custody, psp_mic.budget, psp_mic.blocks,
        atomic_load_explicit(&psp_mic.transfer_pending,
                             memory_order_relaxed));
    psp_mic.blocks = NULL;
    atomic_store_explicit(&psp_mic.transfer_pending, false,
                          memory_order_relaxed);
    media_capture_ring_init(&psp_mic.ring, NULL, 0);
}

static void psp_mic_join(void)
{
    if (psp_mic.thread < 0) return;
    atomic_store_explicit(&psp_mic.stop, true, memory_order_release);
    int ended = psp_thread_wait_end_bounded(psp_mic.thread,
                                            MEDIA_CAPTURE_JOIN_US);
    if (ended < 0) {
        /* A worker stuck in firmware keeps its buffers rather than writing
           into memory given to someone else; its thread stays too. */
        printf("tilefinch-microphone: worker join failed 0x%08X\n", ended);
        psp_mic.abandoned = true;
        psp_mic.thread = -1;
        return;
    }
    (void) sceKernelDeleteThread(psp_mic.thread);
    psp_mic.thread = -1;
}

static bool psp_mic_available(void *context)
{
    (void) context;
    return psp_mic_owner_thread() && !psp_mic.abandoned;
}

/* With page capture stopped: whether voice typing may use the input now.
   A worker told to stop from another thread is collected first; a held
   pair goes back once one poll shows its transfer ended. */
static bool psp_mic_input_free(void *context)
{
    (void) context;
    if (psp_mic.abandoned) return false;
    /* Not bound yet: page capture has never run. */
    if (psp_mic.budget == NULL) return true;
    if (!psp_mic_owner_thread()) return false;
    if (psp_mic.thread >= 0
        && atomic_load_explicit(&psp_mic.stop, memory_order_acquire)) {
        psp_mic_join();
        psp_mic_release_buffers();
    }
    if (psp_mic.thread >= 0 || psp_mic.abandoned) return false;
    if (!media_capture_input_settle(&psp_mic.custody, psp_mic.budget,
                                    &psp_mic_input)) {
        printf("tilefinch-microphone: input still busy\n");
        return false;
    }
    psp_mic.held_without_device = false;
    return true;
}

static MediaCapturePermission psp_mic_permission(void *context,
                                                 const char *origin,
                                                 bool *can_remember)
{
    (void) context;
    if (can_remember != NULL)
        *can_remember = browser_profile_microphone_site_room(psp_mic.profile,
                                                             origin);
    return browser_profile_microphone_site(psp_mic.profile, origin);
}

static bool psp_mic_remember(void *context, const char *origin,
                             MediaCapturePermission permission)
{
    (void) context;
    if (!browser_profile_set_microphone_site(psp_mic.profile, origin,
                                             permission))
        return false;
    if (psp_mic.profile_store != NULL)
        psp_profile_store_mark_dirty(
            psp_mic.profile_store, (uint64_t) sceKernelGetSystemTimeWide());
    return true;
}

static MediaCaptureVerdict psp_mic_start(void *context,
                                         MediaCaptureFormat *format)
{
    (void) context;
    if (!psp_mic_owner_thread() || psp_mic.abandoned)
        return MEDIA_CAPTURE_NO_DEVICE;
    /* A worker told to stop from another thread is collected here. */
    if (psp_mic.thread >= 0
        && atomic_load_explicit(&psp_mic.stop, memory_order_acquire)) {
        psp_mic_join();
        psp_mic_release_buffers();
    }
    if (psp_mic.thread >= 0) return MEDIA_CAPTURE_BUSY;
    psp_mic.synthetic = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_mic.synthetic = psp_input_script_running();
#endif
    bool initialized = psp_mic.microphone_initialized
        || (psp_mic.voice_microphone_initialized != NULL
            && *psp_mic.voice_microphone_initialized);
    if (!initialized && !psp_mic.synthetic) {
        int result = sceAudioInputInit(0, MEDIA_CAPTURE_INPUT_GAIN, 0);
        if (result < 0) {
            printf("tilefinch-microphone: input init failed 0x%08X\n",
                   result);
            return MEDIA_CAPTURE_NO_DEVICE;
        }
        psp_mic.microphone_initialized = true;
        /* Voice typing must not initialize the input a second time. */
        if (psp_mic.voice_microphone_initialized != NULL)
            *psp_mic.voice_microphone_initialized = true;
    }
    /* The hardware writes the pair: whole cache lines of its own, or the
       held pair once the input is seen idle (BUSY while it is not). */
    MediaCaptureVerdict pair_verdict = MEDIA_CAPTURE_FAILED;
    psp_mic.blocks = media_capture_input_pair_acquire(
        &psp_mic.custody, psp_mic.budget, &psp_mic_input, &pair_verdict);
    if (psp_mic.blocks == NULL) {
        printf("tilefinch-microphone: input pair refused verdict=%d\n",
               (int) pair_verdict);
        /* An input that never delivered is still no microphone. */
        return pair_verdict == MEDIA_CAPTURE_BUSY
                && psp_mic.held_without_device
            ? MEDIA_CAPTURE_NO_DEVICE : pair_verdict;
    }
    psp_mic.ring_storage = budget_malloc_category(
        psp_mic.budget, BUDGET_CATEGORY_RESOURCE,
        MEDIA_CAPTURE_RING_SAMPLES * sizeof(int16_t));
    atomic_store_explicit(&psp_mic.transfer_pending, false,
                          memory_order_relaxed);
    if (psp_mic.ring_storage == NULL) {
        psp_mic_release_buffers();
        return MEDIA_CAPTURE_FAILED;
    }
    memset(psp_mic.ring_storage, 0,
           MEDIA_CAPTURE_RING_SAMPLES * sizeof(int16_t));
    media_capture_ring_init(&psp_mic.ring, psp_mic.ring_storage,
                            MEDIA_CAPTURE_RING_SAMPLES);
    atomic_store_explicit(&psp_mic.stop, false, memory_order_release);
    atomic_store_explicit(&psp_mic.worker_state, MEDIA_CAPTURE_INPUT_STARTING,
                          memory_order_release);
    atomic_store_explicit(&psp_mic.blocks_captured, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&psp_mic.peak, 0u, memory_order_relaxed);
    psp_mic.thread = sceKernelCreateThread(
        "tilefinch_microphone", psp_mic_worker,
        TILEFINCH_PSP_THREAD_PRIORITY_MICROPHONE, 4u * 1024u,
        PSP_THREAD_ATTR_USER, NULL);
    if (psp_mic.thread < 0
        || sceKernelStartThread(psp_mic.thread, 0, NULL) < 0) {
        if (psp_mic.thread >= 0) (void) sceKernelDeleteThread(psp_mic.thread);
        psp_mic.thread = -1;
        psp_mic_release_buffers();
        return MEDIA_CAPTURE_FAILED;
    }
    /* The page is told only once the input delivers: a missing or silent
       microphone is NotFoundError, not a track that never hears. Bounded. */
    int state = MEDIA_CAPTURE_INPUT_STARTING;
    for (unsigned waited = 0; waited < MEDIA_CAPTURE_START_WAIT_MS;
         waited += 4u) {
        state = atomic_load_explicit(&psp_mic.worker_state,
                                     memory_order_acquire);
        if (state != MEDIA_CAPTURE_INPUT_STARTING) break;
        (void) sceKernelDelayThread(4000);
    }
    if (state != MEDIA_CAPTURE_INPUT_RUNNING) {
        psp_mic_join();
        psp_mic_release_buffers();
        printf("tilefinch-microphone: start refused state=%d\n", state);
        return state == MEDIA_CAPTURE_INPUT_FAILED ? MEDIA_CAPTURE_FAILED
                                                   : MEDIA_CAPTURE_NO_DEVICE;
    }
    psp_mic.started_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (format != NULL) {
        format->sample_rate = MEDIA_CAPTURE_SAMPLE_RATE;
        format->channels = MEDIA_CAPTURE_CHANNELS;
        format->latency = (double) MEDIA_CAPTURE_BLOCK_SAMPLES
                          / (double) MEDIA_CAPTURE_SAMPLE_RATE;
    }
    if (psp_mic.ui != NULL) psp_mic.ui->microphone_live = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-microphone: started synthetic=%u\n",
           psp_mic.synthetic ? 1u : 0u);
#endif
    return MEDIA_CAPTURE_GRANTED;
}

static void psp_mic_stop(void *context)
{
    (void) context;
    /* Off the browser thread only the worker is told; the browser thread
       collects it and its buffers at the next start. */
    atomic_store_explicit(&psp_mic.stop, true, memory_order_release);
    if (!psp_mic_owner_thread() || (psp_mic.thread < 0
                                    && psp_mic.ring_storage == NULL))
        return;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    unsigned blocks = atomic_load_explicit(&psp_mic.blocks_captured,
                                           memory_order_relaxed);
    unsigned peak = atomic_load_explicit(&psp_mic.peak, memory_order_relaxed);
#endif
    psp_mic_join();
    psp_mic_release_buffers();
    if (psp_mic.ui != NULL) psp_mic.ui->microphone_live = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Validation builds only, and levels only: how long, how many blocks,
       the loudest sample. */
    printf("tilefinch-microphone: stopped ms=%llu blocks=%u peak=%u\n",
           (unsigned long long) ((now_us - psp_mic.started_us) / 1000u),
           blocks, peak);
#endif
}

/* Whether the worker is delivering: started, not told to stop, and not
   failed or stalled since. */
static bool psp_mic_delivering(void)
{
    return psp_mic.thread >= 0
        && !atomic_load_explicit(&psp_mic.stop, memory_order_acquire)
        && atomic_load_explicit(&psp_mic.worker_state, memory_order_acquire)
               == MEDIA_CAPTURE_INPUT_RUNNING;
}

/* The worker left after it started (the input failed or stalled): the
   engine ends capture with MEDIA_CAPTURE_END_DEVICE. */
static bool psp_mic_failed(void *context)
{
    (void) context;
    return psp_mic_owner_thread() && psp_mic.thread >= 0
        && !atomic_load_explicit(&psp_mic.stop, memory_order_acquire)
        && atomic_load_explicit(&psp_mic.worker_state, memory_order_acquire)
               != MEDIA_CAPTURE_INPUT_RUNNING;
}

static uint32_t psp_mic_read_since(void *context, float *output,
                                   uint32_t count, uint32_t *cursor)
{
    (void) context;
    if (!psp_mic_delivering()) return 0;
    return media_capture_ring_since(&psp_mic.ring, cursor, output, count);
}

/* Page microphone audio may play only into headphones: through the
   speaker it would feed back into the microphone. */
static bool psp_audio_output_private(void *context)
{
    (void) context;
    return sceHprmIsHeadphoneExist() > 0;
}

/* ---- the prompt -------------------------------------------------------- */

static uint32_t psp_mic_prompt_sample(uint32_t *raw)
{
    *raw = 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    PspUiInput scripted = {.analog_x = 128, .analog_y = 128};
    if (psp_input_script_text_frame(&scripted)) {
        uint32_t mapped = 0;
        if (scripted.held & PSP_UI_BUTTON_UP) mapped |= MEDIA_CAPTURE_BUTTON_UP;
        if (scripted.held & PSP_UI_BUTTON_DOWN)
            mapped |= MEDIA_CAPTURE_BUTTON_DOWN;
        if (scripted.held & PSP_UI_BUTTON_CONFIRM)
            mapped |= MEDIA_CAPTURE_BUTTON_CROSS;
        if (scripted.held & PSP_UI_BUTTON_CANCEL)
            mapped |= MEDIA_CAPTURE_BUTTON_CIRCLE;
        return mapped;
    }
#endif
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return 0;
    *raw = pad.Buttons;
    uint32_t mapped = 0;
    if (pad.Buttons & PSP_CTRL_UP) mapped |= MEDIA_CAPTURE_BUTTON_UP;
    if (pad.Buttons & PSP_CTRL_DOWN) mapped |= MEDIA_CAPTURE_BUTTON_DOWN;
    if (pad.Buttons & PSP_CTRL_CROSS) mapped |= MEDIA_CAPTURE_BUTTON_CROSS;
    if (pad.Buttons & PSP_CTRL_CIRCLE) mapped |= MEDIA_CAPTURE_BUTTON_CIRCLE;
    return mapped;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation: the first prompt a scripted run shows, beside the EBOOT as
   frame-mark-microphone-prompt.ppm. */
static void psp_mic_prompt_capture(void)
{
    static bool captured;
    if (captured || !psp_input_script_running()) return;
    const uint16_t *presented = psp_display_front_buffer(&psp_display);
    captured = presented != NULL
        && psp_dump_frame_strided_named(
               psp_mic.argv0, "frame-mark-microphone-prompt.ppm", presented,
               PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
}
#endif

static bool psp_mic_prompt(void *context,
                           const TilefinchMediaCapturePrompt *request,
                           MediaCapturePromptAnswer *answer)
{
    (void) context;
    if (request == NULL || answer == NULL || psp_mic.ui == NULL
        || psp_mic.views == NULL || !psp_mic_owner_thread()
        || psp_home_exit_pending()
        || !psp_lifecycle_presentation_allowed(&psp_lifecycle)
        || (psp_active_media != NULL && psp_active_media->ui.visible))
        return false;
    PspNavigationCooperate *cooperate = &psp_navigation_cooperate;
    bool fenced = psp_presentation_fence_acquire();
    const PspUiState *base = fenced
        ? &cooperate->supervisor_ui : psp_mic.ui;
    const uint16_t *frame = fenced
        ? psp_supervisor_page_frame(cooperate) : psp_mic.views->frame;
    if (frame == NULL || psp_ui_screen_is_native_surface(base->screen)) {
        psp_presentation_fence_release(fenced);
        return false;
    }
    PspLogPhase phase = psp_log_phase();
    psp_mic_prompt_ui = *base;
    psp_mic_prompt_ui.toast_frames = 0;
    psp_mic_prompt_ui.loading = false;
    /* The real address bar shows above the prompt, even over a page in
       fullscreen: the page cannot draw there. */
    psp_mic_prompt_ui.chrome_visible = true;
    psp_mic_prompt_ui.page_fullscreen = false;
    PspUiMicrophonePromptView view = {
        .host = request->host != NULL ? request->host : "",
        .can_remember = request->can_remember
    };
    uint64_t opened_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint32_t raw = 0;
    MediaCapturePromptModal modal;
    media_capture_prompt_modal_open(&modal, request->can_remember,
                                    psp_mic_prompt_sample(&raw),
                                    opened_us / 1000u);
    psp_ui_show_microphone_prompt(&psp_mic_prompt_ui, &view);
    bool shown = psp_present_internal(frame, &psp_mic_prompt_ui, false);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-microphone: prompt host=%s frame=%u remember=%u "
           "shown=%u\n", view.host, request->from_frame ? 1u : 0u,
           request->can_remember ? 1u : 0u, shown ? 1u : 0u);
    if (shown) psp_mic_prompt_capture();
#endif
    int step = shown ? MEDIA_CAPTURE_PROMPT_PENDING
                     : MEDIA_CAPTURE_PROMPT_DISMISSED;
    /* Bounded: the modal dismisses at MEDIA_CAPTURE_PROMPT_MAXIMUM_OPEN_MS. */
    while (step == MEDIA_CAPTURE_PROMPT_PENDING) {
        if (psp_home_exit_pending()
            || !psp_lifecycle_presentation_allowed(&psp_lifecycle)) {
            step = MEDIA_CAPTURE_PROMPT_DISMISSED;
            break;
        }
        psp_log_heartbeat();
        psp_log_set_stage("microphone-prompt");
        unsigned ledger = work_ledger_enter(WORK_LEDGER_SLEEP);
        sceDisplayWaitVblankStart();
        work_ledger_leave(ledger);
        uint8_t selection = modal.selection;
        step = media_capture_prompt_modal_step(
            &modal, psp_mic_prompt_sample(&raw),
            (uint64_t) sceKernelGetSystemTimeWide() / 1000u);
        if (step == MEDIA_CAPTURE_PROMPT_PENDING
            && modal.selection != selection) {
            view.selection = modal.selection;
            (void) psp_present_internal(frame, &psp_mic_prompt_ui, false);
        }
    }
    *answer = (MediaCapturePromptAnswer) step;
    psp_ui_clear_microphone_prompt(&psp_mic_prompt_ui);
    /* Hand the screen and pad back; the button that answered is taken as
       already seen everywhere. */
    if (shown) (void) psp_present_internal(frame, base, true);
    SceCtrlData pad = {0};
    uint32_t still = sceCtrlPeekBufferPositive(&pad, 1) > 0
        ? pad.Buttons : raw;
    (void) psp_controller_take_latched_pressed();
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t open_us = now_us > opened_us ? now_us - opened_us : 0;
    if (fenced) {
        cooperate->supervisor_previous_buttons = still;
        cooperate->fallback_previous_buttons = still;
        cooperate->last_checkpoint_us = now_us;
    }
    if (psp_mic.interactive != NULL) {
        psp_mic.interactive->previous_buttons = psp_ui_buttons(still);
        psp_navigation_watchdog_pause(
            &psp_mic.interactive->navigation_watchdog, open_us);
    }
    /* Load loops pause their watchdogs by the page-dialog total. */
    psp_page_dialog_add_open_us(open_us);
    psp_presentation_fence_release(fenced);
    psp_log_set_phase(phase);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-microphone: prompt answer=%d open-ms=%llu\n", step,
           (unsigned long long) (open_us / 1000u));
#endif
    return shown;
}

void psp_media_capture_install(TilefinchPlatformServices *services)
{
    if (services == NULL) return;
    services->media_capture_available = psp_mic_available;
    services->media_capture_permission = psp_mic_permission;
    services->media_capture_prompt = psp_mic_prompt;
    services->media_capture_remember = psp_mic_remember;
    services->media_capture_start = psp_mic_start;
    services->media_capture_stop = psp_mic_stop;
    services->media_capture_read_since = psp_mic_read_since;
    services->media_capture_failed = psp_mic_failed;
    services->media_capture_input_free = psp_mic_input_free;
    services->audio_output_private = psp_audio_output_private;
}
