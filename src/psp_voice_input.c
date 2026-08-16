#include "tilefinch/psp_voice_input.h"

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/psp_log.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/voice_job_lifecycle.h"
#include "psp_module_policy.h"
#include "psp_thread_contract.h"
#include "stt_engine.h"

#define printf psp_log_printf

#define VOICE_INPUT_BLOCK_SAMPLES 1024u
#define VOICE_DECODE_TIMEOUT_US UINT64_C(30000000)
/* How long a worker may ignore a cooperative cancel before the frontend
   stops calling it a graceful stop and tells the user what to do. */
#define VOICE_CANCEL_STALL_US UINT64_C(5000000)
#define VOICE_WORKER_STACK_BYTES (256u * 1024u)

typedef struct {
    size_t heap_free;
    size_t heap_largest;
    size_t budget_remaining;
    size_t effective_free;
    size_t effective_largest;
    unsigned system_free;
    unsigned system_largest;
} PspVoiceMemory;

static size_t voice_budget_available(const PspVoiceInput *voice)
{
    if (voice == NULL || voice->budget == NULL) return SIZE_MAX;
    size_t available = budget_remaining(voice->budget);
    return available > SIZE_MAX - voice->voice_reserved_bytes
        ? SIZE_MAX : available + voice->voice_reserved_bytes;
}

static PspVoiceMemory voice_memory_snapshot(const PspVoiceInput *voice)
{
    struct mallinfo heap = mallinfo();
    PspVoiceMemory memory = {
        .heap_free = heap.fordblks,
        /* dlmalloc exposes the contiguous top chunk separately.  It is a
           conservative lower bound when smaller free chunks also exist. */
        .heap_largest = heap.keepcost,
        .budget_remaining = voice_budget_available(voice),
        .system_free = sceKernelTotalFreeMemSize(),
        .system_largest = sceKernelMaxFreeMemSize()
    };
    memory.effective_free =
        memory.budget_remaining;
    memory.effective_largest = memory.budget_remaining;
    return memory;
}

/*
 * The browser asks newlib to claim the largest available user-memory block,
 * but dlmalloc commits that arena lazily. mallinfo() therefore cannot
 * distinguish "uncommitted but allocatable" from unavailable memory. Verify
 * the actual contract directly:
 * hold the required largest block plus bounded one-MiB chunks until the full
 * working set and safety margin have simultaneously been admitted, then free
 * them before recording. This changes no page state and leaves no retained
 * probe allocation.
 */
static bool voice_memory_probe(size_t working_bytes)
{
    enum { PROBE_BLOCKS = 20 };
    void *blocks[PROBE_BLOCKS] = {0};
    size_t total = working_bytes;
    size_t margin = voice_model_selection_margin_bytes();
    if (total > SIZE_MAX - margin) return false;
    total += margin;
    size_t largest = voice_model_largest_allocation_bytes();
    if (largest > total) largest = total;
    size_t remaining = total;
    size_t count = 0;
    while (remaining != 0 && count < PROBE_BLOCKS) {
        size_t bytes = count == 0 ? largest
            : (remaining > 1024u * 1024u
               ? 1024u * 1024u : remaining);
        if (bytes > remaining) bytes = remaining;
        blocks[count] = memalign(64, bytes);
        if (blocks[count] == NULL) break;
        remaining -= bytes;
        count++;
    }
    for (size_t at = count; at != 0; at--) free(blocks[at - 1]);
    printf(
        "tilefinch-voice: physical-probe working=%zu margin=%zu "
        "largest=%zu blocks=%zu admitted=%s\n",
        working_bytes, margin, largest, count,
        remaining == 0 ? "yes" : "no");
    return remaining == 0;
}

typedef struct {
    SttEngine *engine;
    int16_t *capture;
    size_t capture_samples;
    SceUID completion_event;
    VoiceModelTier requested_tier;
    VoiceModelTier tier;
    VoiceCacheRows cache_rows;
    size_t working_bytes;
    bool create_engine;
    bool fell_back;
    VoiceJobLifecycle lifecycle;
    char model_root[768];
    SttStatus status;
    SttResult result;
} PspVoiceJob;

#define VOICE_JOB_COMPLETE 0x1u

static uint64_t voice_clock_microseconds(void *user)
{
    (void) user;
    return sceKernelGetSystemTimeWide();
}

static SttStatus voice_create_tier(
    PspVoiceJob *job, VoiceModelTier tier)
{
    if (job == NULL || tier == VOICE_MODEL_NONE)
        return STT_STATUS_INVALID_ARGUMENT;

    char acoustic[896];
    char dictionary[896];
    char language_model[896];
    const char *search_directory =
        tier == VOICE_MODEL_EXTRA_WIDE ? "extra-wide" : "search";
    if (snprintf(
            acoustic, sizeof(acoustic), "%s/en-us",
            job->model_root) >= (int) sizeof(acoustic)
        || snprintf(
            dictionary, sizeof(dictionary), "%s/%s/search.dict",
            job->model_root, search_directory) >= (int) sizeof(dictionary)
        || snprintf(
            language_model, sizeof(language_model),
            "%s/%s/search.lm.bin",
            job->model_root, search_directory)
               >= (int) sizeof(language_model))
        return STT_STATUS_INVALID_ARGUMENT;

    SttEngineConfig config;
    stt_engine_config_init(&config);
    stt_engine_config_set_preset(
        &config, tier == VOICE_MODEL_EXTRA_WIDE
                     ? STT_PRESET_LATENCY_FAST : STT_PRESET_QUALITY);
    config.acoustic_model_path = acoustic;
    config.dictionary_path = dictionary;
    config.language_model_path = language_model;
    config.sendump_cache_rows = (size_t) job->cache_rows;
    config.clock_microseconds = voice_clock_microseconds;
    config.callback_user = NULL;
    SttEngine *engine = NULL;
    SttStatus status = stt_engine_create(&engine, &config);
    if (status == STT_STATUS_OK) {
        job->engine = engine;
        job->tier = tier;
    }
    PspVoiceMemory memory = voice_memory_snapshot(NULL);
    printf(
        "tilefinch-voice: tier-init tier=%s preset=%s status=%s "
        "cache-rows=%u working=%zu heap-free=%zu heap-largest=%zu "
        "system-free=%u system-largest=%u\n",
        voice_model_tier_name(tier), stt_preset_name(config.preset),
        stt_status_name(status), (unsigned) job->cache_rows,
        voice_model_tier_working_bytes_for_cache(
            tier, job->cache_rows),
        memory.heap_free, memory.heap_largest,
        memory.system_free, memory.system_largest);
    return status;
}

static SttStatus voice_create_engine(PspVoiceJob *job)
{
    if (job == NULL || job->requested_tier == VOICE_MODEL_NONE)
        return STT_STATUS_INVALID_ARGUMENT;
    SttStatus status = voice_create_tier(job, job->requested_tier);
    if (status == STT_STATUS_OK
        || job->requested_tier != VOICE_MODEL_EXTRA_WIDE) {
        return status;
    }

    PspVoiceMemory memory = voice_memory_snapshot(NULL);
    printf(
        "tilefinch-voice: fallback from=extra-wide to=small "
        "previous-status=%s heap-free=%zu heap-largest=%zu "
        "system-free=%u system-largest=%u\n",
        stt_status_name(status), memory.heap_free, memory.heap_largest,
        memory.system_free, memory.system_largest);
    job->fell_back = true;
    return voice_create_tier(job, VOICE_MODEL_SMALL);
}

static int voice_decode_cancelled(void *user)
{
    const PspVoiceJob *job = user;
    return job != NULL
        && voice_job_lifecycle_cancelled(&job->lifecycle);
}

static int voice_decode_thread(SceSize argument_size, void *arguments)
{
    PspVoiceJob *job = NULL;
    if (arguments != NULL && argument_size == sizeof(job))
        memcpy(&job, arguments, sizeof(job));
    if (job == NULL) return -1;
    job->status = job->create_engine
        ? voice_create_engine(job) : STT_STATUS_OK;
    if (job->status == STT_STATUS_OK) {
        job->status = stt_engine_decode_capture_pcm_cancelable(
            job->engine, job->capture, job->capture_samples,
            voice_decode_cancelled, job, &job->result);
    }
    int signalled = sceKernelSetEventFlag(
        job->completion_event, VOICE_JOB_COMPLETE);
    return signalled < 0 ? signalled : 0;
}

typedef struct {
    PspVoiceJob *job;
    SceUID thread;
    uint32_t completed_bits;
    int event_poll_status;
    int worker_observe_status;
    uint32_t worker_status;
    int worker_exit_status;
    bool completed_without_event;
    bool worker_killed;
} PspVoiceLifecyclePlatform;

static bool voice_lifecycle_spawn(void *user)
{
    PspVoiceLifecyclePlatform *platform = user;
    PspVoiceJob *job_pointer =
        platform == NULL ? NULL : platform->job;
    return platform != NULL && job_pointer != NULL
        && sceKernelStartThread(
               platform->thread, sizeof(job_pointer), &job_pointer) >= 0;
}

static bool voice_lifecycle_poll_complete(void *user)
{
    PspVoiceLifecyclePlatform *platform = user;
    if (platform == NULL || platform->job == NULL) return false;
    int polled = sceKernelPollEventFlag(
        platform->job->completion_event, VOICE_JOB_COMPLETE,
        PSP_EVENT_WAITOR, &platform->completed_bits);
    platform->event_poll_status = polled;
    if (polled >= 0) return true;

    /* The event is the cheap happy path, but it is not proof that the worker
       is still alive. A stack fault, explicit kernel kill, or a failure in
       the worker's final sceKernelSetEventFlag used to leave the frontend in
       an unbounded polling loop. ReferThreadStatus is nonblocking and its
       size-sensitive firmware contract is centralized in the wrapper. Only
       a positively observed terminal state permits the owner to reclaim the
       worker buffers; a query error is not treated as completion. */
    PspThreadObservation observation;
    int referred = psp_thread_observe(platform->thread, &observation);
    platform->worker_observe_status = referred;
    if (referred < 0
        || psp_module_worker_poll_disposition(
               referred, observation.status)
               != PSP_MODULE_WORKER_POLL_TERMINAL) {
        return false;
    }
    platform->worker_status = observation.status;
    platform->worker_exit_status = observation.exit_status;
    platform->worker_killed = psp_module_worker_was_killed(
        observation.status);
    platform->completed_without_event = true;
    if (platform->worker_killed)
        platform->job->status = STT_STATUS_DECODER_INIT_FAILED;
    return true;
}

static uint64_t voice_lifecycle_now(void *user)
{
    (void) user;
    return sceKernelGetSystemTimeWide();
}

static void voice_progress(
    PspVoiceProgress progress, void *user, const char *status)
{
    if (progress != NULL) progress(user, status);
}

static void voice_log_early_end(
    uint32_t attempt, uint64_t attempt_started, const char *outcome,
    size_t captured, uint64_t capture_us)
{
    PspVoiceMemory memory = voice_memory_snapshot(NULL);
    printf(
        "tilefinch-voice-calibration: version=1 attempt=%lu phase=end "
        "outcome=%s capture=%zu capture-us=%llu elapsed-us=%llu "
        "heap-free=%zu heap-largest=%zu "
        "system-free=%u system-largest=%u\n",
        attempt, outcome == NULL ? "failed" : outcome, captured,
        (unsigned long long) capture_us,
        (unsigned long long)
            (sceKernelGetSystemTimeWide() - attempt_started),
        memory.heap_free, memory.heap_largest,
        memory.system_free, memory.system_largest);
}

void psp_voice_input_init(
    PspVoiceInput *voice, Budget *budget, const char *model_root)
{
    if (voice == NULL) return;
    memset(voice, 0, sizeof(*voice));
    voice->budget = budget;
    voice->cache_rows = VOICE_CACHE_FULL_ROWS;
    snprintf(
        voice->model_root, sizeof(voice->model_root), "%s",
        model_root == NULL ? "voice-model" : model_root);
    printf("tilefinch-voice: startup-headroom protected=0 experimental=off\n");
}

bool psp_voice_input_set_enabled(PspVoiceInput *voice, bool enabled)
{
    if (voice == NULL) return false;
    if (!enabled) {
        psp_voice_input_evict(voice);
        budget_reservation_release(&voice->voice_external);
        voice->voice_reserved_bytes = 0;
        voice->enabled = false;
        return true;
    }
    if (voice->enabled) return true;
    size_t protected_bytes =
        voice_model_tier_working_bytes_for_cache(
            VOICE_MODEL_EXTRA_WIDE, VOICE_CACHE_FULL_ROWS)
        + STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(int16_t)
        + VOICE_WORKER_STACK_BYTES + sizeof(PspVoiceJob)
        + voice_model_selection_margin_bytes();
    if (voice->budget == NULL
        || !budget_reservation_acquire(
               &voice->voice_external, voice->budget,
               BUDGET_CATEGORY_RESOURCE, protected_bytes)) {
        printf(
            "tilefinch-voice: experimental-enable refused requested=%zu\n",
            protected_bytes);
        return false;
    }
    voice->voice_reserved_bytes = protected_bytes;
    voice->enabled = true;
    printf(
        "tilefinch-voice: experimental-enable protected=%zu\n",
        protected_bytes);
    return true;
}

void psp_voice_input_set_adaptive_memory(
    PspVoiceInput *voice, bool enabled)
{
    if (voice == NULL || voice->adaptive_memory == enabled) return;
    psp_voice_input_evict(voice);
    voice->adaptive_memory = enabled;
    voice->cache_rows = VOICE_CACHE_FULL_ROWS;
}

void psp_voice_input_set_cancel_requested(
    PspVoiceInput *voice, PspVoiceCancelRequested cancel_requested,
    void *cancel_user)
{
    if (voice == NULL) return;
    voice->cancel_requested = cancel_requested;
    voice->cancel_user = cancel_user;
}

static bool voice_external_cancel_requested(const PspVoiceInput *voice)
{
    return voice != NULL && voice->cancel_requested != NULL
        && voice->cancel_requested(voice->cancel_user);
}

static size_t voice_capture(
    PspVoiceInput *voice, int16_t *capture, size_t capacity,
    PspVoiceProgress progress, void *progress_user, bool *cancelled)
{
    if (cancelled != NULL) *cancelled = false;
    if (!voice->microphone_initialized) {
        int initialized = sceAudioInputInit(0, 0x2d, 0);
        if (initialized < 0) {
            printf(
                "tilefinch-voice: microphone init failed 0x%08X\n",
                initialized);
            return 0;
        }
        voice->microphone_initialized = true;
    }

    size_t captured = 0;
    unsigned shown_seconds = STT_ENGINE_DEFAULT_MAX_SECONDS + 1u;
    unsigned previous_buttons = 0;
    while (captured + VOICE_INPUT_BLOCK_SAMPLES <= capacity) {
        if (voice_external_cancel_requested(voice)) {
            if (cancelled != NULL) *cancelled = true;
            break;
        }
        size_t remaining_samples = capacity - captured;
        unsigned remaining_seconds = (unsigned) (
            (remaining_samples + STT_ENGINE_CAPTURE_RATE - 1u)
            / STT_ENGINE_CAPTURE_RATE);
        if (remaining_seconds != shown_seconds) {
            char status[64];
            shown_seconds = remaining_seconds;
            snprintf(
                status, sizeof(status),
                "LISTENING  %uS LEFT - X STOP  O CANCEL",
                remaining_seconds);
            voice_progress(progress, progress_user, status);
        }
        psp_log_heartbeat();
        int result = sceAudioInputBlocking(
            VOICE_INPUT_BLOCK_SAMPLES, STT_ENGINE_CAPTURE_RATE,
            capture + captured);
        if (result < 0) {
            printf("tilefinch-voice: capture failed 0x%08X\n", result);
            break;
        }
        captured += VOICE_INPUT_BLOCK_SAMPLES;
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
            if (pad.Buttons & PSP_CTRL_CIRCLE) {
                if (cancelled != NULL) *cancelled = true;
                break;
            }
            if ((pad.Buttons & PSP_CTRL_CROSS)
                && !(previous_buttons & PSP_CTRL_CROSS)) break;
            previous_buttons = pad.Buttons;
        }
    }
    return captured;
}

static const char *voice_failure_status(const PspVoiceJob *job)
{
    if (job == NULL) return "VOICE INPUT FAILED";
    if (job->status == STT_STATUS_OUT_OF_MEMORY)
        return "NOT ENOUGH FREE MEMORY FOR VOICE";
    if (job->status == STT_STATUS_NO_HYPOTHESIS)
        return "NO SPEECH RECOGNIZED";
    if (job->status == STT_STATUS_INPUT_REJECTED) {
        switch (job->result.input_status) {
            case STT_INPUT_TOO_SHORT: return "SPEAK A LITTLE LONGER";
            case STT_INPUT_TOO_QUIET: return "MICROPHONE INPUT TOO QUIET";
            case STT_INPUT_TOO_CLIPPED: return "MICROPHONE INPUT CLIPPED";
            case STT_INPUT_TOO_NOISY: return "MICROPHONE INPUT TOO NOISY";
            default: return "NO USABLE SPEECH";
        }
    }
    if (job->status == STT_STATUS_DECODER_INIT_FAILED)
        return "VOICE MODEL UNAVAILABLE";
    return "VOICE INPUT FAILED";
}

bool psp_voice_input_transcribe(
    PspVoiceInput *voice, PspVoiceProgress progress, void *progress_user,
    char *output, size_t output_capacity)
{
    if (voice == NULL || !voice->enabled) {
        voice_progress(
            progress, progress_user, "EXPERIMENTAL VOICE IS DISABLED");
        return false;
    }
    if (voice != NULL && voice->disabled_after_timeout) {
        voice_progress(progress, progress_user, "VOICE DISABLED AFTER TIMEOUT");
        return false;
    }
    if (output == NULL || output_capacity == 0) return false;
    output[0] = '\0';
    size_t transient_bytes =
        STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(int16_t)
        + VOICE_WORKER_STACK_BYTES + sizeof(PspVoiceJob);
    size_t full_working = voice_model_tier_working_bytes_for_cache(
        VOICE_MODEL_EXTRA_WIDE, VOICE_CACHE_FULL_ROWS);
    size_t required_reservation =
        full_working > SIZE_MAX - transient_bytes
            ? SIZE_MAX : full_working + transient_bytes;
    size_t selection_margin = voice_model_selection_margin_bytes();
    required_reservation =
        required_reservation > SIZE_MAX - selection_margin
            ? SIZE_MAX : required_reservation + selection_margin;
    if (voice->budget == NULL
        || (voice->voice_reserved_bytes < required_reservation
            && !budget_reservation_acquire(
                &voice->voice_external, voice->budget,
                BUDGET_CATEGORY_RESOURCE, required_reservation))) {
        voice_progress(progress, progress_user, "VOICE MEMORY UNAVAILABLE");
        return false;
    }
    if (voice->voice_reserved_bytes < required_reservation)
        voice->voice_reserved_bytes = required_reservation;
    psp_log_set_phase(PSP_LOG_PHASE_VOICE);
    psp_log_heartbeat();
    psp_voice_input_trim(voice);
    uint32_t attempt = ++voice->attempts;
    uint64_t attempt_started = sceKernelGetSystemTimeWide();
    PspVoiceMemory before_memory = voice_memory_snapshot(voice);
    printf(
        "tilefinch-voice-calibration: version=1 attempt=%lu phase=begin "
        "resident-tier=%s heap-free=%zu heap-largest=%zu "
        "budget-remaining=%zu effective=%zu/%zu "
        "system-free=%u system-largest=%u\n",
        attempt, voice_model_tier_name(voice->tier),
        before_memory.heap_free, before_memory.heap_largest,
        before_memory.budget_remaining,
        before_memory.effective_free, before_memory.effective_largest,
        before_memory.system_free, before_memory.system_largest);

    /*
     * Decide whether a model can be admitted *before* inviting the user to
     * speak.
     *
     * The admission check used to run after capture, so a session with no
     * hope of completing still recorded several seconds of audio and only
     * then reported "NOT ENOUGH FREE MEMORY FOR VOICE". Worse, the capture
     * buffer was allocated first, so recording actively reduced the memory
     * the model was about to be measured against. Refuse up front instead.
     *
     * This is a pre-flight, not the reservation: memory can change while the
     * user speaks, so the authoritative acquire still happens below and can
     * still fail. What it removes is the guaranteed-futile recording.
     */
    if (voice->engine == NULL) {
        VoiceModelAdmission admissible = voice_model_policy_admit(
            before_memory.budget_remaining, before_memory.budget_remaining,
            before_memory.budget_remaining, voice->adaptive_memory);
        size_t probe_working =
            admissible.working_bytes > SIZE_MAX - transient_bytes
                ? SIZE_MAX : admissible.working_bytes + transient_bytes;
        voice_progress(progress, progress_user, "PREPARING VOICE...");
        if (admissible.tier == VOICE_MODEL_NONE
            || !voice_memory_probe(probe_working)) {
            printf("tilefinch-voice: pre-flight refused tier=none "
                   "heap-free=%zu heap-largest=%zu budget-remaining=%zu "
                   "effective=%zu/%zu\n",
                   before_memory.heap_free, before_memory.heap_largest,
                   before_memory.budget_remaining,
                   before_memory.effective_free,
                   before_memory.effective_largest);
            voice_progress(
                progress, progress_user,
                "NOT ENOUGH FREE MEMORY FOR VOICE");
            voice_log_early_end(
                attempt, attempt_started, "model-preflight-refused", 0, 0);
            return false;
        }
    }

    int16_t *capture = memalign(
        64, STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
    if (capture == NULL) {
        voice_progress(
            progress, progress_user, "VOICE BUFFER UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started, "capture-buffer-oom", 0, 0);
        return false;
    }
    bool cancelled = false;
    uint64_t capture_started = sceKernelGetSystemTimeWide();
    size_t captured = voice_capture(
        voice, capture, STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES,
        progress, progress_user, &cancelled);
    uint64_t capture_us = sceKernelGetSystemTimeWide() - capture_started;
    if (cancelled || captured == 0) {
        memset(
            capture, 0,
            STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
        free(capture);
        voice_progress(
            progress, progress_user,
            cancelled ? "VOICE INPUT CANCELLED" : "MICROPHONE UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started,
            cancelled ? "cancelled" : "capture-failed",
            captured, capture_us);
        return false;
    }

    PspVoiceJob *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        memset(
            capture, 0,
            STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
        free(capture);
        voice_progress(progress, progress_user, "VOICE WORKER UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started, "worker-job-oom",
            captured, capture_us);
        return false;
    }
    job->capture = capture;
    job->capture_samples = captured;
    job->status = STT_STATUS_INVALID_ARGUMENT;
    job->engine = (SttEngine *) voice->engine;
    job->tier = voice->tier;
    job->create_engine = job->engine == NULL;
    if (job->create_engine) {
        PspVoiceMemory memory = voice_memory_snapshot(voice);
        VoiceModelAdmission admission = voice_model_policy_admit(
            memory.budget_remaining, memory.budget_remaining,
            memory.budget_remaining, voice->adaptive_memory);
        job->requested_tier = admission.tier;
        job->cache_rows = admission.cache_rows;
        job->working_bytes = admission.working_bytes;
        printf(
            "tilefinch-voice: select tier=%s cache-rows=%u adaptive=%s "
            "working=%zu heap-free=%zu "
            "heap-largest=%zu budget-remaining=%zu effective=%zu/%zu "
            "system-free=%u system-largest=%u\n",
            voice_model_tier_name(job->requested_tier),
            (unsigned) job->cache_rows,
            voice->adaptive_memory ? "yes" : "no",
            job->working_bytes,
            memory.heap_free, memory.heap_largest,
            memory.budget_remaining,
            memory.effective_free, memory.effective_largest,
            memory.system_free, memory.system_largest);
        if (job->requested_tier == VOICE_MODEL_NONE
            || voice->budget == NULL
            || voice->voice_reserved_bytes < job->working_bytes) {
            free(job);
            memset(
                capture, 0,
                STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
            free(capture);
            voice_progress(
                progress, progress_user,
                "NOT ENOUGH FREE MEMORY FOR VOICE");
            voice_log_early_end(
                attempt, attempt_started, "model-admission-failed",
                captured, capture_us);
            return false;
        }
        snprintf(
            job->model_root, sizeof(job->model_root), "%s",
            voice->model_root);
    }
    job->completion_event = sceKernelCreateEventFlag(
        "tilefinch_voice_complete", 0, 0, NULL);
    if (job->completion_event < 0) {
        free(job);
        memset(
            capture, 0,
            STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
        free(capture);
        voice_progress(progress, progress_user, "VOICE WORKER UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started, "worker-event-failed",
            captured, capture_us);
        return false;
    }
    SceUID thread = sceKernelCreateThread(
        "tilefinch_voice_decode", voice_decode_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_VOICE, VOICE_WORKER_STACK_BYTES,
        PSP_THREAD_ATTR_USER, NULL);
    if (thread < 0) {
        PspVoiceMemory memory = voice_memory_snapshot(voice);
        printf(
            "tilefinch-voice: worker create failed native=0x%08x "
            "stack=%u heap-free=%zu heap-largest=%zu "
            "system-free=%u system-largest=%u\n",
            (unsigned) thread, (unsigned) VOICE_WORKER_STACK_BYTES,
            memory.heap_free, memory.heap_largest,
            memory.system_free, memory.system_largest);
        sceKernelDeleteEventFlag(job->completion_event);
        free(job);
        memset(
            capture, 0,
            STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
        free(capture);
        voice_progress(progress, progress_user, "VOICE WORKER UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started, "worker-create-failed",
            captured, capture_us);
        return false;
    }
    PspVoiceLifecyclePlatform lifecycle_platform_state = {
        .job = job, .thread = thread
    };
    VoiceJobLifecyclePlatform lifecycle_platform = {
        .spawn = voice_lifecycle_spawn,
        .poll_complete = voice_lifecycle_poll_complete,
        .now_microseconds = voice_lifecycle_now,
        .user = &lifecycle_platform_state
    };
    if (voice_job_lifecycle_start(
            &job->lifecycle, &lifecycle_platform,
            VOICE_DECODE_TIMEOUT_US)
        == VOICE_JOB_LIFECYCLE_START_FAILED) {
        sceKernelDeleteThread(thread);
        sceKernelDeleteEventFlag(job->completion_event);
        free(job);
        memset(
            capture, 0,
            STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
        free(capture);
        voice_progress(progress, progress_user, "VOICE WORKER UNAVAILABLE");
        voice_log_early_end(
            attempt, attempt_started, "worker-start-failed",
            captured, capture_us);
        return false;
    }

    bool timed_out = false;
    bool user_cancelled = false;
    bool cancellation_announced = false;
    bool stall_announced = false;
    uint64_t cancellation_started_us = 0;
    SceCtrlData initial_pad = {0};
    unsigned previous_buttons =
        sceCtrlPeekBufferPositive(&initial_pad, 1) > 0
            ? initial_pad.Buttons : 0;
    VoiceJobLifecycleResult lifecycle_result =
        VOICE_JOB_LIFECYCLE_RUNNING;
    unsigned shown_decode_seconds =
        (unsigned) (VOICE_DECODE_TIMEOUT_US / UINT64_C(1000000)) + 1u;
    while ((lifecycle_result = voice_job_lifecycle_pump(
                &job->lifecycle, &lifecycle_platform))
           != VOICE_JOB_LIFECYCLE_COMPLETED
           && lifecycle_result
              != VOICE_JOB_LIFECYCLE_COMPLETED_AFTER_TIMEOUT) {
        /*
         * Heartbeat on every pass, including after a timeout.
         *
         * Reclaiming the capture buffer while the worker may still be writing
         * it is not an option, so this wait is deliberately unbounded. That
         * makes an honest heartbeat more important, not less: suppressing it
         * made the watchdog record "suspected-hang" for a frontend that is
         * polling correctly, which is exactly the signal that should stay
         * reserved for a genuinely wedged browser thread.
         */
        psp_log_heartbeat();
        if (!timed_out
            && lifecycle_result
               == VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED) {
            timed_out = true;
            voice->timeouts++;
            voice->disabled_after_timeout =
                voice_job_lifecycle_disables_voice(&job->lifecycle);
        }
        if (voice_external_cancel_requested(voice)
            && !voice_job_lifecycle_cancelled(&job->lifecycle)) {
            user_cancelled = true;
            voice_job_lifecycle_request_cancel(&job->lifecycle);
            voice_progress(
                progress, progress_user, "PAUSING VOICE...");
            printf(
                "tilefinch-voice: system cancellation requested\n");
        }
        sceDisplayWaitVblankStart();
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
            unsigned pressed = pad.Buttons & ~previous_buttons;
            previous_buttons = pad.Buttons;
            if ((pressed & PSP_CTRL_CIRCLE) != 0
                && !voice_job_lifecycle_cancelled(&job->lifecycle)) {
                user_cancelled = true;
                voice_job_lifecycle_request_cancel(&job->lifecycle);
                voice_progress(
                    progress, progress_user,
                    "STOPPING VOICE...");
                printf(
                    "tilefinch-voice: user cancellation requested\n");
            }
        }
        if (timed_out && !cancellation_announced) {
            cancellation_announced = true;
            cancellation_started_us = sceKernelGetSystemTimeWide();
            printf(
                "tilefinch-voice: decode timeout limit-us=%llu "
                "cooperative-cancel=yes\n",
                (unsigned long long) VOICE_DECODE_TIMEOUT_US);
            voice_progress(
                progress, progress_user,
                "VOICE TOOK TOO LONG - STOPPING...");
        } else if (timed_out && !stall_announced
                   && cancellation_started_us != 0
                   && sceKernelGetSystemTimeWide() - cancellation_started_us
                          >= VOICE_CANCEL_STALL_US) {
            /*
             * The worker has ignored a cooperative cancel for long enough
             * that it is not going to answer. Its buffers cannot be reclaimed
             * from here without risking a write into freed memory, so the
             * wait continues — but say so, rather than leaving a frozen
             * "cancelling safely" on screen with no explanation and no
             * suggested action.
             */
            stall_announced = true;
            printf("tilefinch-voice: decode stalled past cancel deadline "
                   "stall-us=%llu recovery=exit-to-xmb\n",
                   (unsigned long long) VOICE_CANCEL_STALL_US);
            voice_progress(
                progress, progress_user,
                "VOICE STUCK - PRESS HOME TO EXIT");
        } else if (!timed_out) {
            uint64_t now = sceKernelGetSystemTimeWide();
            uint64_t elapsed =
                now > job->lifecycle.started_microseconds
                    ? now - job->lifecycle.started_microseconds : 0;
            uint64_t remaining =
                elapsed < job->lifecycle.timeout_microseconds
                    ? job->lifecycle.timeout_microseconds - elapsed : 0;
            unsigned remaining_seconds = (unsigned) (
                (remaining + UINT64_C(999999)) / UINT64_C(1000000));
            if (remaining_seconds != shown_decode_seconds) {
                char status[64];
                shown_decode_seconds = remaining_seconds;
                snprintf(
                    status, sizeof(status),
                    "RECOGNIZING - UP TO %uS LEFT",
                    remaining_seconds);
                voice_progress(progress, progress_user, status);
            }
        }
    }
    timed_out = voice_job_lifecycle_disables_voice(&job->lifecycle);
    sceKernelWaitThreadEnd(thread, NULL);
    sceKernelDeleteThread(thread);
    sceKernelDeleteEventFlag(job->completion_event);

    if (lifecycle_platform_state.completed_without_event) {
        printf(
            "tilefinch-voice: worker terminal without completion event "
            "event=0x%08x refer=0x%08x status=0x%08x exit=0x%08x "
            "killed=%s\n",
            (unsigned) lifecycle_platform_state.event_poll_status,
            (unsigned) lifecycle_platform_state.worker_observe_status,
            (unsigned) lifecycle_platform_state.worker_status,
            (unsigned) lifecycle_platform_state.worker_exit_status,
            lifecycle_platform_state.worker_killed ? "yes" : "no");
    }
    if (lifecycle_platform_state.worker_killed) {
        /* A killed decoder may have left PocketSphinx's heap graph midway
           through a mutation. Do not call back into it from the browser
           thread. Quarantine the bounded one-attempt allocation, detach a
           previously resident engine as well, and disable voice until the
           process restarts. This trades memory for process integrity only on
           an already-fatal worker path. */
        voice->disabled_after_timeout = true;
        if (voice->engine == job->engine) {
            voice->engine = NULL;
            voice->tier = VOICE_MODEL_NONE;
        }
        job->engine = NULL;
    }

    if (job->create_engine) {
        if (!lifecycle_platform_state.worker_killed
            && !timed_out && job->status != STT_STATUS_CANCELLED
            && job->engine != NULL) {
            voice->engine = job->engine;
            job->engine = NULL;
            voice->tier = job->tier;
            voice->cache_rows = job->cache_rows;
            voice->total_initialization_us +=
                stt_engine_init_microseconds(
                    (SttEngine *) voice->engine);
            if (voice->tier == VOICE_MODEL_EXTRA_WIDE)
                voice->extra_wide_initializations++;
            else
                voice->small_initializations++;
            if (job->fell_back) voice->fallbacks++;
        } else {
            stt_engine_destroy(job->engine);
            job->engine = NULL;
        }
    }

    bool success = !timed_out && job->status == STT_STATUS_OK
        && job->result.text[0] != '\0';
    if (success) {
        voice->successes++;
        snprintf(output, output_capacity, "%s", job->result.text);
        char status[96];
        snprintf(status, sizeof(status), "HEARD: %.80s", output);
        voice_progress(progress, progress_user, status);
    } else if (user_cancelled) {
        voice_progress(progress, progress_user, "VOICE INPUT CANCELLED");
    } else if (!timed_out) {
        voice_progress(
            progress, progress_user, voice_failure_status(job));
    }
    if (!timed_out)
        voice->total_decode_us += job->result.timings.total_decode_us;
    PspVoiceMemory after_memory = voice_memory_snapshot(voice);
    printf(
        "tilefinch-voice: status=%s input=%s capture=%zu model=%zu "
        "tier=%s cache-rows=%u preset=%s init-us=%llu decode-us=%llu "
        "heap-free=%zu heap-largest=%zu budget-remaining=%zu "
        "system-free=%u system-largest=%u\n",
        timed_out ? "timeout" : stt_status_name(job->status),
        stt_input_status_name(job->result.input_status),
        captured, job->result.model_samples,
        voice_model_tier_name(voice->tier),
        (unsigned) voice->cache_rows,
        voice->engine == NULL ? "none"
            : stt_preset_name(stt_engine_preset(
                  (SttEngine *) voice->engine)),
        (unsigned long long) stt_engine_init_microseconds(
            (SttEngine *) voice->engine),
        (unsigned long long) job->result.timings.total_decode_us,
        after_memory.heap_free, after_memory.heap_largest,
        after_memory.budget_remaining,
        after_memory.system_free, after_memory.system_largest);
    printf(
        "tilefinch-voice-calibration: version=1 attempt=%lu phase=end "
        "outcome=%s tier=%s cache-rows=%u preset=%s "
        "capture=%zu capture-us=%llu "
        "init-us=%llu decode-us=%llu elapsed-us=%llu "
        "before-heap=%zu before-largest=%zu "
        "after-heap=%zu after-largest=%zu "
        "before-system=%u/%u after-system=%u/%u\n",
        attempt, success ? "success" : stt_status_name(job->status),
        voice_model_tier_name(voice->tier),
        (unsigned) voice->cache_rows,
        voice->engine == NULL ? "none"
            : stt_preset_name(stt_engine_preset(
                  (SttEngine *) voice->engine)),
        captured, (unsigned long long) capture_us,
        (unsigned long long) stt_engine_init_microseconds(
            (SttEngine *) voice->engine),
        (unsigned long long) job->result.timings.total_decode_us,
        (unsigned long long)
            (sceKernelGetSystemTimeWide() - attempt_started),
        before_memory.heap_free, before_memory.heap_largest,
        after_memory.heap_free, after_memory.heap_largest,
        before_memory.system_free, before_memory.system_largest,
        after_memory.system_free, after_memory.system_largest);
    memset(
        capture, 0,
        STT_ENGINE_DEFAULT_MAX_CAPTURE_SAMPLES * sizeof(*capture));
    free(capture);
    free(job);
    return success;
}

void psp_voice_input_evict(PspVoiceInput *voice)
{
    if (voice == NULL || voice->engine == NULL) return;
    VoiceModelTier tier = voice->tier;
    stt_engine_destroy((SttEngine *) voice->engine);
    voice->engine = NULL;
    voice->tier = VOICE_MODEL_NONE;
    voice->cache_rows = VOICE_CACHE_FULL_ROWS;
    PspVoiceMemory memory = voice_memory_snapshot(voice);
    printf(
        "tilefinch-voice: evicted tier=%s heap-free=%zu "
        "heap-largest=%zu budget-remaining=%zu system-free=%u "
        "system-largest=%u\n",
        voice_model_tier_name(tier), memory.heap_free,
        memory.heap_largest, memory.budget_remaining,
        memory.system_free, memory.system_largest);
}

void psp_voice_input_trim(PspVoiceInput *voice)
{
    if (voice == NULL || voice->engine == NULL) return;
    PspVoiceMemory memory = voice_memory_snapshot(voice);
    if (!voice_model_policy_should_evict(
            voice->tier,
            memory.effective_free, memory.effective_largest)) return;
    voice->pressure_evictions++;
    printf(
        "tilefinch-voice: pressure tier=%s heap-free=%zu "
        "heap-largest=%zu budget-remaining=%zu effective=%zu/%zu "
        "system-free=%u system-largest=%u\n",
        voice_model_tier_name(voice->tier),
        memory.heap_free, memory.heap_largest, memory.budget_remaining,
        memory.effective_free, memory.effective_largest,
        memory.system_free, memory.system_largest);
    psp_voice_input_evict(voice);
}

void psp_voice_input_report(const PspVoiceInput *voice)
{
    if (voice == NULL) return;
    PspVoiceMemory memory = voice_memory_snapshot(voice);
    printf(
        "tilefinch-voice-calibration: version=1 summary attempts=%lu "
        "successes=%lu extra-wide-inits=%lu small-inits=%lu fallbacks=%lu "
        "pressure-evictions=%lu timeouts=%lu "
        "init-us-total=%llu decode-us-total=%llu "
        "resident-tier=%s cache-rows=%u adaptive=%s "
        "heap-free=%zu heap-largest=%zu "
        "budget-remaining=%zu system-free=%u system-largest=%u\n",
        voice->attempts, voice->successes,
        voice->extra_wide_initializations, voice->small_initializations,
        voice->fallbacks, voice->pressure_evictions, voice->timeouts,
        (unsigned long long) voice->total_initialization_us,
        (unsigned long long) voice->total_decode_us,
        voice_model_tier_name(voice->tier),
        (unsigned) voice->cache_rows,
        voice->adaptive_memory ? "yes" : "no",
        memory.heap_free, memory.heap_largest, memory.budget_remaining,
        memory.system_free, memory.system_largest);
}

void psp_voice_input_shutdown(PspVoiceInput *voice)
{
    if (voice == NULL) return;
    (void) psp_voice_input_set_enabled(voice, false);
}
