#include "tilefinch/game_audio.h"
#include "tilefinch/psp_fpu.h"

#include <math.h>
#include <float.h>
#include <stdatomic.h>
#include <string.h>

#if defined(PSP)
#include "tilefinch/psp_threads.h"
#include <pspaudio.h>
#include <pspkernel.h>
#endif

typedef struct {
    _Atomic uint32_t start_output_frame;
    _Atomic uint32_t targets_q12;
    _Atomic uint32_t coefficient_q15;
} GameAudioEnvelopeSegment;

typedef struct {
    _Atomic uint32_t generation;
    _Atomic uint32_t start;
    _Atomic uint32_t duration;
    _Atomic uint32_t count;
    _Atomic uint32_t multipliers_q8;
    _Atomic uint32_t points[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
} GameAudioCurve;

typedef struct {
    uint32_t generation, start, duration, count, multipliers_q8;
    uint32_t step, remainder, position, error, last_frame;
    bool positioned, finished;
    uint32_t points[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
} GameAudioCurveState;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static _Atomic bool game_audio_validation_muted;
static _Atomic bool game_audio_validation_mix_enabled;
static _Atomic bool game_audio_validation_work_enabled = true;
static _Atomic bool game_audio_validation_mix_fast_enabled = true;
static _Atomic bool game_audio_validation_constant_enabled = true;
static _Atomic bool game_audio_validation_automation_enabled = true;
static _Atomic bool game_audio_validation_metrics_owned;
static _Atomic uint32_t game_audio_validation_mix_generation;
static _Atomic uint32_t game_audio_validation_dropped_blocks;
static TilefinchGameAudioMixMetrics game_audio_validation_metrics;
static TilefinchGameAudioBlockRecord game_audio_validation_blocks[
    TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT];

/* The caller owns metrics_owned. No allocation, clocks, I/O or waiting in
   the audio worker's publication path. Old slots are hidden by the count. */
static void game_audio_validation_store_block(
    const TilefinchGameAudioBlockRecord *record)
{
    TilefinchGameAudioMixMetrics *metrics = &game_audio_validation_metrics;
    if (metrics->block_records < TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT)
        game_audio_validation_blocks[metrics->block_records++] = *record;
    else if (metrics->record_overflow != UINT32_MAX)
        metrics->record_overflow++;
}

void tilefinch_game_audio_validation_mix_fast(bool enabled)
{
    atomic_store_explicit(&game_audio_validation_mix_fast_enabled, enabled, memory_order_release);
}

void tilefinch_game_audio_validation_constant(bool enabled)
{
    atomic_store_explicit(&game_audio_validation_constant_enabled, enabled,
                          memory_order_release);
}

void tilefinch_game_audio_validation_automation(bool enabled)
{
    atomic_store_explicit(&game_audio_validation_automation_enabled, enabled,
                          memory_order_release);
}

bool tilefinch_game_audio_validation_mix_measure(bool enabled)
{
    atomic_store_explicit(&game_audio_validation_mix_enabled, false, memory_order_release);
    if (!enabled) return true;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&game_audio_validation_metrics_owned,
            &expected, true, memory_order_acquire, memory_order_relaxed)) return false;
    atomic_fetch_add_explicit(&game_audio_validation_mix_generation, 1u, memory_order_relaxed);
    atomic_store_explicit(&game_audio_validation_dropped_blocks, 0u, memory_order_relaxed);
    memset(&game_audio_validation_metrics, 0, sizeof(game_audio_validation_metrics));
    atomic_store_explicit(&game_audio_validation_metrics_owned, false, memory_order_release);
    atomic_store_explicit(&game_audio_validation_mix_enabled, true, memory_order_release);
    return true;
}

bool tilefinch_game_audio_validation_mix_work(bool enabled)
{
    if (atomic_load_explicit(&game_audio_validation_mix_enabled, memory_order_acquire))
        return false;
    atomic_store_explicit(&game_audio_validation_work_enabled, enabled, memory_order_release);
    return true;
}

bool tilefinch_game_audio_validation_mix_metrics(TilefinchGameAudioMixMetrics *out)
{
    if (out == NULL) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&game_audio_validation_metrics_owned,
            &expected, true, memory_order_acquire, memory_order_relaxed)) return false;
    *out = game_audio_validation_metrics;
    out->dropped_blocks = atomic_load_explicit(&game_audio_validation_dropped_blocks, memory_order_relaxed);
    atomic_store_explicit(&game_audio_validation_metrics_owned, false, memory_order_release);
    return true;
}

bool tilefinch_game_audio_validation_mix_block(
    size_t index, TilefinchGameAudioBlockRecord *out)
{
    if (out == NULL) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&game_audio_validation_metrics_owned,
            &expected, true, memory_order_acquire, memory_order_relaxed)) return false;
    bool okay = index < game_audio_validation_metrics.block_records;
    if (okay) *out = game_audio_validation_blocks[index];
    atomic_store_explicit(&game_audio_validation_metrics_owned, false, memory_order_release);
    return okay;
}

typedef struct {
    uint32_t silent, audible, constant, automated, fast_forwarded;
} GameAudioBlockWork;
void tilefinch_game_audio_validation_mute_output(bool muted)
{
    atomic_store_explicit(&game_audio_validation_muted, muted, memory_order_relaxed);
}
#endif

typedef struct {
    int16_t *samples;
    uint32_t frames;
    uint32_t sample_rate;
    uint32_t generation;
    uint8_t channels;
} GameAudioBuffer;

typedef struct {
    /* 0 = free, 1 = ready, 2 = browser initialization,
       3 = mixer-owned, 4 = stop requested while mixer-owned,
       5 = generation-tagged completion awaiting browser consumption. */
    _Atomic uint32_t active;
    uint32_t generation;
    uint32_t buffer_index;
    uint32_t start_output_frame;
    _Atomic uint32_t stop_output_frame;
    uint32_t position_frame;
    uint32_t fraction_q16;
    uint32_t step_whole;
    _Atomic uint32_t step_fraction;
    uint32_t loop_begin_frame;
    uint32_t loop_end_frame;
    uint32_t end_frame;
    uint32_t oscillator_phase;
    _Atomic uint32_t gains_q8;
    GameAudioEnvelopeSegment
        envelope[TILEFINCH_GAME_AUDIO_ENVELOPE_SEGMENT_LIMIT];
    _Atomic uint32_t envelope_count;
    _Atomic uint32_t envelope_generation;
    uint32_t mixer_envelope_generation;
    uint32_t mixer_envelope_cursor;
    int32_t mixer_gain_left_q12;
    int32_t mixer_gain_right_q12;
    int32_t mixer_target_left_q12;
    int32_t mixer_target_right_q12;
    uint32_t mixer_coefficient_q15;
    GameAudioCurve gain_curve, pitch_curve;
    GameAudioCurveState mixer_gain_curve, mixer_pitch_curve;
    uint8_t kind;
    uint8_t oscillator_type;
    bool loop;
} GameAudioVoice;

struct TilefinchGameAudio {
    Budget *budget;
    int32_t *constant_accum;
    GameAudioBuffer buffers[TILEFINCH_GAME_AUDIO_BUFFER_LIMIT];
    GameAudioVoice voices[TILEFINCH_GAME_AUDIO_VOICE_LIMIT];
    size_t pcm_bytes;
    _Atomic uint32_t output_frame;
    _Atomic bool suspended;
    _Atomic bool stop;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    GameAudioBlockWork validation_work;
    bool validation_work_counted;
    uint8_t validation_work_override; /* 0 normal, 1 clocks-only, 2 counted corpus */
#endif
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
    /* The standalone mixer test includes this implementation to compare the
       optimized path against the original arithmetic, without a release knob. */
    bool reference_mix, reference_constant_mix, reference_automation_mix;
    uint32_t fast_forwarded_samples, silent_samples, constant_path_samples;
    uint32_t constant_span_blocks, active_span_blocks;
#endif
#if defined(PSP)
    SceUID event;
    SceUID thread;
    int channel;
    _Atomic bool worker_exited;
#endif
};

static uint16_t read_u16(const unsigned char *at)
{
    return (uint16_t) at[0] | (uint16_t) ((uint16_t) at[1] << 8);
}

static uint32_t read_u32(const unsigned char *at)
{
    return (uint32_t) at[0] | ((uint32_t) at[1] << 8)
        | ((uint32_t) at[2] << 16) | ((uint32_t) at[3] << 24);
}

TilefinchGameAudio *tilefinch_game_audio_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    TilefinchGameAudio *audio = budget_calloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, 1, sizeof(*audio));
    if (audio == NULL) return NULL;
    audio->budget = budget;
    /* Optional fixed scratch, immutable after construction. Allocation refusal
       keeps the ordinary mixer available; mixing never allocates. */
    audio->constant_accum = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT,
        TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u * sizeof(int32_t));
    atomic_init(&audio->suspended, true);
    atomic_init(&audio->stop, false);
    atomic_init(&audio->output_frame, 0u);
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        atomic_init(&audio->voices[i].active, 0u);
        atomic_init(&audio->voices[i].stop_output_frame, UINT32_MAX);
        atomic_init(&audio->voices[i].step_fraction, 0u);
        atomic_init(&audio->voices[i].gains_q8, 0u);
        atomic_init(&audio->voices[i].envelope_count, 0u);
        atomic_init(&audio->voices[i].envelope_generation, 1u);
        GameAudioCurve *curves[] = {
            &audio->voices[i].gain_curve, &audio->voices[i].pitch_curve};
        for (size_t at = 0; at < 2; at++) {
            atomic_init(&curves[at]->generation, 2u);
            atomic_init(&curves[at]->start, 0u);
            atomic_init(&curves[at]->duration, 0u);
            atomic_init(&curves[at]->count, 0u);
            atomic_init(&curves[at]->multipliers_q8, 0u);
            for (size_t point = 0;
                 point < TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT; point++)
                atomic_init(&curves[at]->points[point], 0u);
        }
        for (size_t segment = 0;
             segment < TILEFINCH_GAME_AUDIO_ENVELOPE_SEGMENT_LIMIT;
             segment++) {
            atomic_init(
                &audio->voices[i].envelope[segment].start_output_frame, 0u);
            atomic_init(
                &audio->voices[i].envelope[segment].targets_q12, 0u);
            atomic_init(
                &audio->voices[i].envelope[segment].coefficient_q15, 0u);
        }
    }
#if defined(PSP)
    audio->event = -1;
    audio->thread = -1;
    audio->channel = -1;
    atomic_init(&audio->worker_exited, false);
#endif
    return audio;
}

bool tilefinch_game_audio_decode_wav(
    TilefinchGameAudio *audio, const unsigned char *bytes, size_t length,
    TilefinchGameAudioBufferInfo *info)
{
    if (audio == NULL || bytes == NULL || info == NULL || length < 44
        || memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0)
        return false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    const unsigned char *payload = NULL;
    size_t payload_length = 0;
    for (size_t at = 12, chunks = 0; at <= length - 8 && chunks < 64;
         chunks++) {
        uint32_t chunk_length = read_u32(bytes + at + 4);
        size_t body = at + 8;
        if ((size_t) chunk_length > length - body) return false;
        if (memcmp(bytes + at, "fmt ", 4) == 0 && chunk_length >= 16) {
            format = read_u16(bytes + body);
            channels = read_u16(bytes + body + 2);
            sample_rate = read_u32(bytes + body + 4);
            bits = read_u16(bytes + body + 14);
        } else if (memcmp(bytes + at, "data", 4) == 0) {
            payload = bytes + body;
            payload_length = chunk_length;
        }
        size_t padded = (size_t) chunk_length + (chunk_length & 1u);
        if (padded > length - body) break;
        at = body + padded;
    }
    if (format != 1 || (channels != 1 && channels != 2)
        || (bits != 8 && bits != 16) || sample_rate < 8000
        || sample_rate > 48000 || payload == NULL) return false;
    size_t bytes_per_frame = (size_t) channels * (size_t) (bits / 8u);
    size_t frames = payload_length / bytes_per_frame;
    if (frames == 0 || frames > UINT32_MAX) return false;
    size_t sample_count = frames * (size_t) channels;
    if (sample_count > SIZE_MAX / sizeof(int16_t)) return false;
    size_t decoded_bytes = sample_count * sizeof(int16_t);
    if (decoded_bytes > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT
        || decoded_bytes
               > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT - audio->pcm_bytes)
        return false;
    size_t slot = TILEFINCH_GAME_AUDIO_BUFFER_LIMIT;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_BUFFER_LIMIT; i++) {
        if (audio->buffers[i].samples == NULL) { slot = i; break; }
    }
    if (slot == TILEFINCH_GAME_AUDIO_BUFFER_LIMIT) return false;
    int16_t *decoded = budget_malloc_category(
        audio->budget, BUDGET_CATEGORY_JAVASCRIPT, decoded_bytes);
    if (decoded == NULL) return false;
    if (bits == 16) {
        for (size_t i = 0; i < sample_count; i++)
            decoded[i] = (int16_t) read_u16(payload + i * 2u);
    } else {
        for (size_t i = 0; i < sample_count; i++)
            decoded[i] = (int16_t) (((int) payload[i] - 128) << 8);
    }
    GameAudioBuffer *buffer = &audio->buffers[slot];
    buffer->generation++;
    if (buffer->generation == 0) buffer->generation = 1;
    buffer->samples = decoded;
    buffer->frames = (uint32_t) frames;
    buffer->sample_rate = sample_rate;
    buffer->channels = (uint8_t) channels;
    audio->pcm_bytes += decoded_bytes;
    info->handle = (buffer->generation << 4) | (uint32_t) (slot + 1u);
    info->frames = buffer->frames;
    info->sample_rate = buffer->sample_rate;
    info->channels = buffer->channels;
    return true;
}

static GameAudioBuffer *game_audio_buffer(
    TilefinchGameAudio *audio, uint32_t handle, size_t *index)
{
    size_t slot = (size_t) (handle & 0x0fu);
    uint32_t generation = handle >> 4;
    if (audio == NULL || slot == 0 || slot > TILEFINCH_GAME_AUDIO_BUFFER_LIMIT)
        return NULL;
    GameAudioBuffer *buffer = &audio->buffers[slot - 1u];
    if (generation == 0 || buffer->generation != generation
        || buffer->samples == NULL) return NULL;
    if (index != NULL) *index = slot - 1u;
    return buffer;
}

#if defined(PSP)
#define GAME_AUDIO_EVENT_WAKE 1u
#define GAME_AUDIO_EVENT_STOP 2u

#ifdef TILEFINCH_PSP_VALIDATION_LOG
typedef struct {
    uint64_t cpu_us;
    uint64_t query_begin_us, query_end_us;
    uint32_t observer_us;
    bool valid;
} GameAudioCpuSample;

static GameAudioCpuSample game_audio_cpu_sample(void)
{
    GameAudioCpuSample result = {0};
    SceKernelThreadRunStatus status;
    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    result.query_begin_us = (uint64_t) sceKernelGetSystemTimeWide();
    int rc = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status);
    result.query_end_us = (uint64_t) sceKernelGetSystemTimeWide();
    result.observer_us = (uint32_t) (result.query_end_us - result.query_begin_us);
    result.valid = rc >= 0;
    if (result.valid)
        result.cpu_us = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
    return result;
}

static void game_audio_publish_block_metrics(uint32_t generation,
    const GameAudioBlockWork *work, GameAudioCpuSample before,
    GameAudioCpuSample mixed, GameAudioCpuSample submitted,
    uint32_t output_frame, int output_result, bool work_counted)
{
    if (!atomic_load_explicit(&game_audio_validation_mix_enabled, memory_order_acquire))
        return;
    if (generation != atomic_load_explicit(&game_audio_validation_mix_generation, memory_order_acquire))
        return;
    bool expected = false;
    /* Never block the audio thread for a diagnostic reader. */
    if (!atomic_compare_exchange_strong_explicit(&game_audio_validation_metrics_owned,
            &expected, true, memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&game_audio_validation_dropped_blocks, 1u, memory_order_relaxed);
        return;
    }
    if (generation != atomic_load_explicit(&game_audio_validation_mix_generation, memory_order_acquire)) {
        atomic_store_explicit(&game_audio_validation_metrics_owned, false, memory_order_release);
        return;
    }
    TilefinchGameAudioMixMetrics *m = &game_audio_validation_metrics;
    bool valid = before.valid && mixed.valid && submitted.valid
        && before.cpu_us <= mixed.cpu_us && mixed.cpu_us <= submitted.cpu_us
        && before.query_begin_us <= before.query_end_us
        && before.query_end_us <= mixed.query_begin_us
        && mixed.query_begin_us <= mixed.query_end_us
        && mixed.query_end_us <= submitted.query_begin_us
        && submitted.query_begin_us <= submitted.query_end_us;
    TilefinchGameAudioBlockRecord record = {
        .begin_before_us = before.query_begin_us, .begin_after_us = before.query_end_us,
        .mixed_before_us = mixed.query_begin_us, .mixed_after_us = mixed.query_end_us,
        .submitted_before_us = submitted.query_begin_us, .submitted_after_us = submitted.query_end_us,
        .begin_cpu_us = before.cpu_us, .mixed_cpu_us = mixed.cpu_us,
        .submitted_cpu_us = submitted.cpu_us,
        .output_frame = output_frame, .frames = TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES,
        .silent = work->silent, .audible = work->audible,
        .constant = work->constant, .automated = work->automated,
        .fast_forwarded = work->fast_forwarded,
        .output_result = output_result, .clocks_valid = valid,
        .work_counted = work_counted
    };
    game_audio_validation_store_block(&record);
    m->observer_us += before.observer_us + mixed.observer_us + submitted.observer_us;
    if (!valid) {
        m->failed_clock_samples++;
    } else {
        uint32_t mix = (uint32_t) (mixed.cpu_us - before.cpu_us);
        uint32_t output = (uint32_t) (submitted.cpu_us - mixed.cpu_us);
        m->blocks++;
        m->mix_cpu_us += mix;
        m->output_cpu_us += output;
        if (mix > m->mix_cpu_max_us) m->mix_cpu_max_us = mix;
        if (output > m->output_cpu_max_us) m->output_cpu_max_us = output;
        uint32_t bin = mix / 100u;
        if (bin >= TILEFINCH_GAME_AUDIO_CPU_HISTOGRAM_BINS)
            bin = TILEFINCH_GAME_AUDIO_CPU_HISTOGRAM_BINS - 1u;
        m->mix_cpu_histogram[bin]++;
        m->silent_samples += work->silent;
        m->audible_samples += work->audible;
        m->constant_samples += work->constant;
        m->automated_samples += work->automated;
        m->fast_forwarded_samples += work->fast_forwarded;
    }
    atomic_store_explicit(&game_audio_validation_metrics_owned, false, memory_order_release);
}
#endif

static bool game_audio_any_voice(TilefinchGameAudio *audio)
{
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        if (atomic_load_explicit(
                &audio->voices[i].active, memory_order_acquire) == 1u)
            return true;
    }
    return false;
}

static int game_audio_thread(SceSize argument_size, void *arguments)
{
    psp_fpu_mask_exceptions();
    TilefinchGameAudio *audio = NULL;
    if (arguments != NULL && argument_size == sizeof(audio))
        memcpy(&audio, arguments, sizeof(audio));
    if (audio == NULL) return -1;
    int16_t block[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    while (!atomic_load_explicit(&audio->stop, memory_order_acquire)) {
        if (atomic_load_explicit(&audio->suspended, memory_order_acquire)
            || !game_audio_any_voice(audio)) {
            uint32_t bits = 0;
            int status = sceKernelWaitEventFlag(
                audio->event, GAME_AUDIO_EVENT_WAKE | GAME_AUDIO_EVENT_STOP,
                PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
            if (status < 0 || (bits & GAME_AUDIO_EVENT_STOP) != 0) break;
            continue;
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        bool measure = atomic_load_explicit(&game_audio_validation_mix_enabled, memory_order_acquire);
        uint32_t measure_generation = atomic_load_explicit(
            &game_audio_validation_mix_generation, memory_order_acquire);
        GameAudioCpuSample before = {0}, mixed = {0}, submitted = {0};
        uint32_t measured_output_frame = measure ? atomic_load_explicit(
            &audio->output_frame, memory_order_acquire) : 0u;
        if (measure) before = game_audio_cpu_sample();
#endif
        (void) tilefinch_game_audio_mix(
            audio, block, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (measure) mixed = game_audio_cpu_sample();
#endif
        int output_volume = PSP_AUDIO_VOLUME_MAX;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (atomic_load_explicit(&game_audio_validation_muted, memory_order_relaxed))
            output_volume = 0;
#endif
        int output_result = sceAudioOutputBlocking(audio->channel, output_volume, block);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (measure) {
            submitted = game_audio_cpu_sample();
            game_audio_publish_block_metrics(measure_generation, &audio->validation_work,
                before, mixed, submitted, measured_output_frame, output_result,
                audio->validation_work_counted);
        }
#endif
        if (output_result < 0) break;
    }
    atomic_store_explicit(&audio->worker_exited, true, memory_order_release);
    return 0;
}

static void game_audio_platform_suspend(TilefinchGameAudio *audio);

static bool game_audio_platform_start(TilefinchGameAudio *audio)
{
    /* A device output fault can end the worker without a browser-thread
       command. Reap that completed generation before creating a replacement;
       a nonnegative SceUID alone does not prove that a thread is live. */
    if (audio->thread >= 0
        && atomic_load_explicit(
               &audio->worker_exited, memory_order_acquire)) {
        game_audio_platform_suspend(audio);
    }
    if (audio->thread >= 0) return true;
    audio->channel = sceAudioChReserve(
        PSP_AUDIO_NEXT_CHANNEL, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES,
        PSP_AUDIO_FORMAT_STEREO);
    if (audio->channel < 0) return false;
    audio->event = sceKernelCreateEventFlag(
        "tilefinch_game_audio", 0, 0, NULL);
    if (audio->event < 0) goto fail;
    audio->thread = sceKernelCreateThread(
        "tilefinch_game_audio", game_audio_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_AUDIO, 8u * 1024u,
        PSP_THREAD_ATTR_USER, NULL);
    if (audio->thread < 0) goto fail;
    TilefinchGameAudio *argument = audio;
    atomic_store_explicit(&audio->worker_exited, false, memory_order_release);
    if (sceKernelStartThread(
            audio->thread, sizeof(argument), &argument) < 0) goto fail;
    return true;
fail:
    if (audio->thread >= 0) {
        sceKernelDeleteThread(audio->thread);
        audio->thread = -1;
    }
    if (audio->event >= 0) {
        sceKernelDeleteEventFlag(audio->event);
        audio->event = -1;
    }
    if (audio->channel >= 0) {
        sceAudioChRelease(audio->channel);
        audio->channel = -1;
    }
    return false;
}

static void game_audio_platform_wake(TilefinchGameAudio *audio)
{
    if (audio->event >= 0)
        (void) sceKernelSetEventFlag(audio->event, GAME_AUDIO_EVENT_WAKE);
}
static void game_audio_platform_suspend(TilefinchGameAudio *audio)
{
    if (audio->thread < 0) return;
    atomic_store_explicit(&audio->stop, true, memory_order_release);
    if (audio->event >= 0)
        (void) sceKernelSetEventFlag(audio->event, GAME_AUDIO_EVENT_STOP);
    (void) sceKernelWaitThreadEnd(audio->thread, NULL);
    (void) sceKernelDeleteThread(audio->thread);
    audio->thread = -1;
    if (audio->event >= 0) (void) sceKernelDeleteEventFlag(audio->event);
    audio->event = -1;
    if (audio->channel >= 0) (void) sceAudioChRelease(audio->channel);
    audio->channel = -1;
    atomic_store_explicit(&audio->stop, false, memory_order_release);
    atomic_store_explicit(&audio->worker_exited, false, memory_order_release);
}
#else
static bool game_audio_platform_start(TilefinchGameAudio *audio)
{
    (void) audio;
    return true;
}
static void game_audio_platform_wake(TilefinchGameAudio *audio)
{
    (void) audio;
}
static void game_audio_platform_suspend(TilefinchGameAudio *audio)
{
    (void) audio;
}
#endif

bool tilefinch_game_audio_resume(TilefinchGameAudio *audio)
{
    if (audio == NULL || !game_audio_platform_start(audio)) return false;
    atomic_store_explicit(&audio->suspended, false, memory_order_release);
    game_audio_platform_wake(audio);
    return true;
}

void tilefinch_game_audio_suspend(TilefinchGameAudio *audio)
{
    if (audio == NULL) return;
    atomic_store_explicit(&audio->suspended, true, memory_order_release);
    /* Release the PSP's scarce hardware channel instead of merely feeding
       silence. Decoded buffers stay resident, so resume can recreate the
       worker without refetching or re-decoding sounds. */
    game_audio_platform_suspend(audio);
}

static bool game_audio_valid_gain(double gain)
{
    return isfinite(gain) && gain >= 0.0 && gain <= 4.0;
}

static uint32_t game_audio_gains_q8(double left, double right)
{
    uint32_t left_q8 = (uint32_t) (left * 256.0 + 0.5);
    uint32_t right_q8 = (uint32_t) (right * 256.0 + 0.5);
    return left_q8 | (right_q8 << 16);
}

static bool game_audio_delay_frames(double seconds, uint32_t *frames)
{
    if (frames == NULL || !isfinite(seconds) || seconds < 0.0
        || seconds > TILEFINCH_GAME_AUDIO_SCHEDULE_LIMIT_SECONDS)
        return false;
    *frames = (uint32_t) (seconds * 44100.0 + 0.5);
    return true;
}

static GameAudioVoice *game_audio_claim_voice(
    TilefinchGameAudio *audio, uint32_t *voice_handle)
{
    if (audio == NULL || voice_handle == NULL) return NULL;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        GameAudioVoice *voice = &audio->voices[i];
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &voice->active, &expected, 2u,
                memory_order_acq_rel, memory_order_relaxed)) continue;
        voice->generation++;
        if (voice->generation == 0) voice->generation = 1;
        /* The slot is exclusively browser-owned during initialization. */
        atomic_store_explicit(&voice->gain_curve.count, 0u, memory_order_relaxed);
        atomic_fetch_add_explicit(&voice->gain_curve.generation, 2u,
                                  memory_order_release);
        atomic_store_explicit(&voice->pitch_curve.count, 0u, memory_order_relaxed);
        atomic_fetch_add_explicit(&voice->pitch_curve.generation, 2u,
                                  memory_order_release);
        atomic_store_explicit(
            &voice->envelope_count, 0u, memory_order_relaxed);
        uint32_t envelope_generation = atomic_load_explicit(
            &voice->envelope_generation, memory_order_relaxed) + 1u;
        if (envelope_generation == 0u) envelope_generation = 1u;
        atomic_store_explicit(
            &voice->envelope_generation, envelope_generation,
            memory_order_release);
        *voice_handle = (voice->generation << 4) | (uint32_t) (i + 1u);
        return voice;
    }
    return NULL;
}

static void game_audio_publish_voice(TilefinchGameAudio *audio,
                                     GameAudioVoice *voice)
{
    atomic_store_explicit(&voice->active, 1u, memory_order_release);
    game_audio_platform_wake(audio);
}

bool tilefinch_game_audio_start(
    TilefinchGameAudio *audio, uint32_t handle, double offset_seconds,
    double duration_seconds, double playback_rate, double gain_left,
    double gain_right, bool loop, double loop_start_seconds,
    double loop_end_seconds, double start_delay_seconds,
    uint32_t *voice_handle)
{
    size_t buffer_index = 0;
    uint32_t delay_frames = 0;
    GameAudioBuffer *buffer = game_audio_buffer(audio, handle, &buffer_index);
    if (buffer == NULL || voice_handle == NULL || !isfinite(offset_seconds)
        || !isfinite(duration_seconds) || !isfinite(playback_rate)
        || !isfinite(loop_start_seconds) || !isfinite(loop_end_seconds)
        || offset_seconds < 0 || duration_seconds < 0
        || loop_start_seconds < 0 || loop_end_seconds < 0
        || playback_rate < 0.25 || playback_rate > 4.0
        || !game_audio_valid_gain(gain_left)
        || !game_audio_valid_gain(gain_right)
        || !game_audio_delay_frames(start_delay_seconds, &delay_frames))
        return false;
    uint32_t begin = offset_seconds >= (double) buffer->frames
                            / (double) buffer->sample_rate
        ? buffer->frames
        : (uint32_t) (offset_seconds * buffer->sample_rate);
    uint32_t end = buffer->frames;
    if (duration_seconds > 0) {
        double duration_frames = duration_seconds * buffer->sample_rate;
        uint32_t count = duration_frames >= UINT32_MAX
            ? UINT32_MAX : (uint32_t) duration_frames;
        if (count < end - begin) end = begin + count;
    }
    if (begin >= end) return false;
    uint32_t loop_begin = loop_start_seconds == 0.0 ? 0u
        : (loop_start_seconds >= (double) buffer->frames
              / (double) buffer->sample_rate
            ? buffer->frames
            : (uint32_t) (loop_start_seconds * buffer->sample_rate));
    uint32_t loop_end = loop_end_seconds == 0.0 ? buffer->frames
        : (loop_end_seconds >= (double) buffer->frames
              / (double) buffer->sample_rate
            ? buffer->frames
            : (uint32_t) (loop_end_seconds * buffer->sample_rate));
    if (loop && loop_end > end) loop_end = end;
    if (loop && loop_begin >= loop_end) return false;
    GameAudioVoice *voice = game_audio_claim_voice(audio, voice_handle);
    if (voice == NULL) return false;
    double step = ((double) buffer->sample_rate / 44100.0) * playback_rate;
    uint32_t whole = (uint32_t) step;
    uint32_t fraction = (uint32_t) ((step - whole) * 65536.0);
    if (whole == 0 && fraction == 0) fraction = 1;
    voice->kind = 0u;
    voice->oscillator_type = 0u;
    voice->buffer_index = (uint32_t) buffer_index;
    voice->start_output_frame = atomic_load_explicit(
        &audio->output_frame, memory_order_acquire) + delay_frames;
    if (voice->start_output_frame == UINT32_MAX)
        voice->start_output_frame--;
    atomic_store_explicit(
        &voice->stop_output_frame, UINT32_MAX, memory_order_relaxed);
    voice->position_frame = begin;
    voice->fraction_q16 = 0;
    voice->step_whole = whole;
    atomic_store_explicit(
        &voice->step_fraction, fraction, memory_order_relaxed);
    voice->loop_begin_frame = loop_begin;
    voice->loop_end_frame = loop_end;
    voice->end_frame = end;
    voice->oscillator_phase = 0;
    atomic_store_explicit(
        &voice->gains_q8, game_audio_gains_q8(gain_left, gain_right),
        memory_order_relaxed);
    voice->loop = loop;
    game_audio_publish_voice(audio, voice);
    return true;
}

static bool game_audio_oscillator_step(double frequency, uint32_t *step)
{
    if (step == NULL || !isfinite(frequency)
        || frequency < 1.0 || frequency > 20000.0) return false;
    double scaled = frequency * (4294967296.0 / 44100.0);
    if (scaled < 1.0 || scaled > UINT32_MAX) return false;
    *step = (uint32_t) (scaled + 0.5);
    return true;
}

bool tilefinch_game_audio_start_oscillator(
    TilefinchGameAudio *audio, TilefinchGameAudioOscillatorType type,
    double frequency, double gain_left, double gain_right,
    double start_delay_seconds, uint32_t *voice_handle)
{
    uint32_t step = 0, delay_frames = 0;
    if (type < TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE
        || type > TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE
        || !game_audio_oscillator_step(frequency, &step)
        || !game_audio_valid_gain(gain_left)
        || !game_audio_valid_gain(gain_right)
        || !game_audio_delay_frames(start_delay_seconds, &delay_frames))
        return false;
    GameAudioVoice *voice = game_audio_claim_voice(audio, voice_handle);
    if (voice == NULL) return false;
    voice->kind = 1u;
    voice->oscillator_type = (uint8_t) type;
    voice->start_output_frame = atomic_load_explicit(
        &audio->output_frame, memory_order_acquire) + delay_frames;
    if (voice->start_output_frame == UINT32_MAX)
        voice->start_output_frame--;
    atomic_store_explicit(
        &voice->stop_output_frame, UINT32_MAX, memory_order_relaxed);
    voice->oscillator_phase = 0;
    atomic_store_explicit(
        &voice->step_fraction, step, memory_order_relaxed);
    atomic_store_explicit(
        &voice->gains_q8, game_audio_gains_q8(gain_left, gain_right),
        memory_order_relaxed);
    voice->loop = false;
    game_audio_publish_voice(audio, voice);
    return true;
}

static GameAudioVoice *game_audio_voice(TilefinchGameAudio *audio,
                                        uint32_t voice_handle)
{
    size_t slot = (size_t) (voice_handle & 0x0fu);
    uint32_t generation = voice_handle >> 4;
    if (audio == NULL || slot == 0
        || slot > TILEFINCH_GAME_AUDIO_VOICE_LIMIT) return NULL;
    GameAudioVoice *voice = &audio->voices[slot - 1u];
    if (generation == 0 || voice->generation != generation) return NULL;
    return voice;
}

bool tilefinch_game_audio_update_voice(
    TilefinchGameAudio *audio, uint32_t voice_handle,
    double gain_left, double gain_right)
{
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL || !game_audio_valid_gain(gain_left)
        || !game_audio_valid_gain(gain_right)) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    atomic_store_explicit(
        &voice->gains_q8, game_audio_gains_q8(gain_left, gain_right),
        memory_order_release);
    return true;
}

bool tilefinch_game_audio_cancel_envelope(
    TilefinchGameAudio *audio, uint32_t voice_handle)
{
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    uint32_t curve_generation = atomic_load_explicit(
        &voice->gain_curve.generation, memory_order_relaxed);
    atomic_store_explicit(&voice->gain_curve.generation,
        curve_generation + 1u, memory_order_seq_cst);
    atomic_store_explicit(&voice->gain_curve.count, 0u, memory_order_relaxed);
    atomic_store_explicit(&voice->gain_curve.generation,
        curve_generation + 2u, memory_order_release);
    atomic_store_explicit(&voice->envelope_count, 0u, memory_order_release);
    uint32_t generation = atomic_load_explicit(
        &voice->envelope_generation, memory_order_relaxed) + 1u;
    if (generation == 0u) generation = 1u;
    atomic_store_explicit(
        &voice->envelope_generation, generation, memory_order_release);
    return true;
}

bool tilefinch_game_audio_schedule_envelope_target(
    TilefinchGameAudio *audio, uint32_t voice_handle,
    double gain_left, double gain_right,
    double start_delay_seconds, double time_constant_seconds)
{
    uint32_t delay_frames = 0;
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL || !game_audio_valid_gain(gain_left)
        || !game_audio_valid_gain(gain_right)
        || !game_audio_delay_frames(start_delay_seconds, &delay_frames)
        || !isfinite(time_constant_seconds) || time_constant_seconds <= 0.0
        || time_constant_seconds > TILEFINCH_GAME_AUDIO_SCHEDULE_LIMIT_SECONDS)
        return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    /* Combining the legacy target timeline and a curve is outside this
       bounded profile. Refuse rather than silently replacing either. */
    if (atomic_load_explicit(&voice->gain_curve.count,
                             memory_order_acquire) != 0u) return false;
    uint32_t count = atomic_load_explicit(
        &voice->envelope_count, memory_order_acquire);
    if (count >= TILEFINCH_GAME_AUDIO_ENVELOPE_SEGMENT_LIMIT) return false;
    uint32_t start = atomic_load_explicit(
        &audio->output_frame, memory_order_acquire) + delay_frames;
    if (start == UINT32_MAX) start--;
    if (count != 0u) {
        uint32_t previous = atomic_load_explicit(
            &voice->envelope[count - 1u].start_output_frame,
            memory_order_acquire);
        if ((int32_t) (start - previous) < 0) return false;
    }
    uint32_t left_q12 = (uint32_t) (gain_left * 4096.0 + 0.5);
    uint32_t right_q12 = (uint32_t) (gain_right * 4096.0 + 0.5);
    /* The admitted game-effect constants are small-step envelopes. The
       first-order 1/(tau*rate) coefficient is indistinguishable at PSP
       output resolution here and avoids pulling exponential evaluation into
       a browser-thread command that exists to keep the mixer cheap. */
    double coefficient = 1.0 / (time_constant_seconds * 44100.0);
    uint32_t coefficient_q15 = (uint32_t) (coefficient * 32768.0 + 0.5);
    if (coefficient_q15 == 0u) coefficient_q15 = 1u;
    if (coefficient_q15 > 32767u) coefficient_q15 = 32767u;
    GameAudioEnvelopeSegment *segment = &voice->envelope[count];
    atomic_store_explicit(
        &segment->start_output_frame, start, memory_order_relaxed);
    atomic_store_explicit(
        &segment->targets_q12,
        left_q12 | (right_q12 << 16), memory_order_relaxed);
    atomic_store_explicit(
        &segment->coefficient_q15, coefficient_q15, memory_order_relaxed);
    atomic_store_explicit(
        &voice->envelope_count, count + 1u, memory_order_release);
    return true;
}

bool tilefinch_game_audio_update_oscillator(
    TilefinchGameAudio *audio, uint32_t voice_handle, double frequency)
{
    uint32_t step = 0;
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL || voice->kind != 1u
        || !game_audio_oscillator_step(frequency, &step)) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    atomic_store_explicit(&voice->step_fraction, step, memory_order_release);
    return true;
}

bool tilefinch_game_audio_cancel_pitch_curve(
    TilefinchGameAudio *audio, uint32_t voice_handle)
{
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL || voice->kind != 1u) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    uint32_t generation = atomic_load_explicit(
        &voice->pitch_curve.generation, memory_order_relaxed);
    atomic_store_explicit(&voice->pitch_curve.generation,
        generation + 1u, memory_order_seq_cst);
    atomic_store_explicit(&voice->pitch_curve.count, 0u, memory_order_relaxed);
    atomic_store_explicit(&voice->pitch_curve.generation,
        generation + 2u, memory_order_release);
    return true;
}

/* For admitted binary32 gains [0,4], this is exactly floor(gain*65536+.5)
   from the double reference, including accepted negative zero. */
static bool game_audio_gain_q16(float value, uint32_t *out)
{
#if FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128
    _Static_assert(sizeof(float) == sizeof(uint32_t), "binary32 storage");
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t magnitude = bits & 0x7fffffffu;
    if (((bits >> 31) != 0u && magnitude != 0u)
        || magnitude > 0x40800000u) return false;
    unsigned exponent = magnitude >> 23;
    if (exponent < 110u) { *out = 0; return true; }
    unsigned shift = 134u - exponent; /* Admitted exponents110..129:24..5. */
    uint32_t significand = (magnitude & 0x7fffffu) | 0x800000u;
    *out = (significand + (1u << (shift - 1u))) >> shift;
    return true;
#else
    if (!game_audio_valid_gain(value)) return false;
    *out = (uint32_t) ((double) value * 65536.0 + .5);
    return true;
#endif
}

bool tilefinch_game_audio_schedule_curve(
    TilefinchGameAudio *audio, uint32_t voice_handle, bool pitch,
    const float *values, size_t count, double gain_left, double gain_right,
    double start_delay_seconds, double duration_seconds)
{
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    uint32_t delay = 0, duration = 0;
    if (voice == NULL || values == NULL || count < 2u
        || count > TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT
        || (pitch && voice->kind != 1u)
        || !game_audio_valid_gain(gain_left)
        || !game_audio_valid_gain(gain_right)
        || !game_audio_delay_frames(start_delay_seconds, &delay)
        || !game_audio_delay_frames(duration_seconds, &duration)
        || duration == 0u
        || duration > 441000u - delay) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    GameAudioCurve *curve = pitch ? &voice->pitch_curve : &voice->gain_curve;
    uint32_t now = atomic_load_explicit(&audio->output_frame, memory_order_acquire);
    if (atomic_load_explicit(&curve->count, memory_order_acquire) != 0u) {
        /* Retain the completed curve's endpoint until explicit cancellation.
           Replacing it with a delayed curve would otherwise expose the old
           unscheduled base value in the gap. */
        return false;
    }
    if (!pitch && atomic_load_explicit(&voice->envelope_count,
                                       memory_order_acquire) != 0u) return false;
    uint32_t points[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
    for (size_t at = 0; at < count; at++) {
        if (pitch) {
            if (!game_audio_oscillator_step(values[at], &points[at])) return false;
        } else {
            if (!game_audio_gain_q16(values[at], &points[at])) return false;
        }
    }
    uint32_t generation = atomic_load_explicit(
        &curve->generation, memory_order_relaxed);
    atomic_store_explicit(&curve->generation, generation + 1u, memory_order_seq_cst);
    atomic_store_explicit(&curve->start, now + delay, memory_order_relaxed);
    atomic_store_explicit(&curve->duration, duration, memory_order_relaxed);
    atomic_store_explicit(&curve->count, (uint32_t) count, memory_order_relaxed);
    atomic_store_explicit(&curve->multipliers_q8,
        game_audio_gains_q8(gain_left, gain_right), memory_order_relaxed);
    for (size_t at = 0; at < count; at++)
        atomic_store_explicit(&curve->points[at], points[at], memory_order_relaxed);
    atomic_store_explicit(&curve->generation, generation + 2u, memory_order_release);
    return true;
}

/* Copy only on publication, never once per sample. All shared words are
   atomic; an interrupted writer keeps the old complete mixer snapshot. */
static void game_audio_curve_snapshot(
    const GameAudioCurve *curve, GameAudioCurveState *state)
{
    for (size_t attempt = 0; attempt < 2u; attempt++) {
        uint32_t generation = atomic_load_explicit(
            &curve->generation, memory_order_acquire);
        if ((generation & 1u) != 0u || generation == state->generation) return;
        GameAudioCurveState next = {0};
        next.generation = generation;
        next.count = atomic_load_explicit(&curve->count, memory_order_relaxed);
        if (next.count > TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT) return;
        next.start = atomic_load_explicit(&curve->start, memory_order_relaxed);
        next.duration = atomic_load_explicit(&curve->duration, memory_order_relaxed);
        next.multipliers_q8 = atomic_load_explicit(
            &curve->multipliers_q8, memory_order_relaxed);
        for (size_t at = 0; at < next.count; at++)
            next.points[at] = atomic_load_explicit(
                &curve->points[at], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (generation != atomic_load_explicit(
                &curve->generation, memory_order_acquire)) continue;
        if (next.count >= 2u && next.duration != 0u) {
            uint32_t distance = (next.count - 1u) << 16;
            next.step = distance / next.duration;
            next.remainder = distance % next.duration;
        }
        *state = next;
        return;
    }
}

static uint32_t game_audio_curve_interpolate(
    uint32_t from, uint32_t to, uint32_t fraction)
{
    /* An unsigned magnitude needs one Allegrex multiply. Subtracting the
       truncated magnitude for descending segments preserves the original
       signed division's truncation toward zero (not a weighted-average
       floor). The product fits uint64_t for every uint32_t endpoint. */
    if (to >= from)
        return from + (uint32_t) (((uint64_t) (to - from) * fraction) >> 16);
    return from - (uint32_t) (((uint64_t) (from - to) * fraction) >> 16);
}

static bool game_audio_curve_value(
    GameAudioCurveState *state, uint32_t frame, uint32_t *value,
    bool optimized)
{
    if (state->count < 2u || state->duration == 0u
        || (int32_t) (frame - state->start) < 0) return false;
    uint32_t elapsed = frame - state->start;
    if (elapsed >= state->duration) {
        state->finished = true;
        *value = state->points[state->count - 1u];
        return true;
    }
    if (!state->positioned || frame != state->last_frame + 1u) {
        uint64_t distance = (uint64_t) elapsed * ((state->count - 1u) << 16);
        state->position = (uint32_t) (distance / state->duration);
        state->error = (uint32_t) (distance % state->duration);
        state->positioned = true;
    } else {
        state->position += state->step;
        state->error += state->remainder;
        if (state->error >= state->duration) {
            state->position++;
            state->error -= state->duration;
        }
    }
    state->last_frame = frame;
    uint32_t at = state->position >> 16;
    uint32_t fraction = state->position & 0xffffu;
#if defined(TILEFINCH_PSP_VALIDATION_LOG) || defined(TILEFINCH_GAME_AUDIO_MIX_TEST)
    if (!optimized) {
        int64_t delta = (int64_t) state->points[at + 1u] - state->points[at];
        *value = (uint32_t) ((int64_t) state->points[at]
            + (delta * fraction) / 65536);
        return true;
    }
#else
    (void) optimized;
#endif
    *value = game_audio_curve_interpolate(
        state->points[at], state->points[at + 1u], fraction);
    return true;
}

void tilefinch_game_audio_stop(TilefinchGameAudio *audio,
                               uint32_t voice_handle,
                               double stop_delay_seconds)
{
    uint32_t delay_frames = 0;
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL
        || !game_audio_delay_frames(stop_delay_seconds, &delay_frames))
        return;
    if (delay_frames > 0) {
        uint32_t now = atomic_load_explicit(
            &audio->output_frame, memory_order_acquire);
        uint32_t deadline = now + delay_frames;
        if (deadline == UINT32_MAX) deadline--;
        atomic_store_explicit(
            &voice->stop_output_frame, deadline, memory_order_release);
        game_audio_platform_wake(audio);
        return;
    }
    uint32_t state = atomic_load_explicit(
        &voice->active, memory_order_acquire);
    while (state == 1u || state == 3u) {
        uint32_t replacement = state == 3u ? 4u : 5u;
        if (atomic_compare_exchange_weak_explicit(
                &voice->active, &state, replacement,
                memory_order_acq_rel, memory_order_acquire)) return;
    }
}

bool tilefinch_game_audio_take_completion(TilefinchGameAudio *audio,
                                          uint32_t *voice_handle)
{
    if (audio == NULL || voice_handle == NULL) return false;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        GameAudioVoice *voice = &audio->voices[i];
        uint32_t expected = 5u;
        if (!atomic_compare_exchange_strong_explicit(
                &voice->active, &expected, 0u,
                memory_order_acq_rel, memory_order_acquire)) continue;
        *voice_handle = (voice->generation << 4) | (uint32_t) (i + 1u);
        return true;
    }
    return false;
}

/* One quarter-wave is enough for all four quadrants and keeps generated
   effects deterministic without floating point in the audio worker. */
static const int16_t game_audio_sine_quarter[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962,
    8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732,
    15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787,
    21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832,
    26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621,
    29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};

static int game_audio_oscillator_sample(uint8_t type, uint32_t phase)
{
    uint32_t point = phase >> 24;
    if (type == TILEFINCH_GAME_AUDIO_OSCILLATOR_SQUARE)
        return point < 128u ? 32767 : -32767;
    if (type == TILEFINCH_GAME_AUDIO_OSCILLATOR_SAWTOOTH)
        return (int) (phase >> 16) - 32768;
    if (type == TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE) {
        uint32_t position = phase >> 16;
        return position < 32768u
            ? -32767 + (int) (position * 2u)
            : 98303 - (int) (position * 2u);
    }
    uint32_t quadrant = point >> 6;
    uint32_t offset = point & 63u;
    if (quadrant == 0u) return game_audio_sine_quarter[offset];
    if (quadrant == 1u) return game_audio_sine_quarter[64u - offset];
    if (quadrant == 2u) return -game_audio_sine_quarter[offset];
    return -game_audio_sine_quarter[64u - offset];
}

static bool game_audio_frame_reached(uint32_t current, uint32_t target)
{
    return target != UINT32_MAX && (int32_t) (current - target) >= 0;
}

/* Called only for a silent block with no scheduled transition. Keep the
   sample loop's delayed wrap: landing exactly on loop_end is observable in
   the next block's voice state and must not wrap until its first sample. */
static bool game_audio_fast_forward_silent(
    GameAudioVoice *voice, uint32_t step, size_t frames)
{
    if (voice->kind == 1u) {
        voice->oscillator_phase += step * (uint32_t) frames;
        return true;
    }
    /* Sub-frame buffer rates advance at most one source frame per sample.
       Larger strides discard overshoot at wrap in the reference loop, so
       leave those and non-looping completion on that path. */
    if (!voice->loop || voice->step_whole != 0u || step > UINT16_MAX
        || voice->loop_begin_frame >= voice->loop_end_frame) return false;
    uint32_t fraction = voice->fraction_q16 + step * (uint32_t) (frames - 1u);
    uint32_t advance = fraction >> 16;
    uint32_t position = voice->position_frame;
    if (position >= voice->loop_end_frame) position = voice->loop_begin_frame;
    uint32_t remaining = voice->loop_end_frame - position;
    if (advance < remaining) position += advance;
    else position = voice->loop_begin_frame
        + (advance - remaining)
            % (voice->loop_end_frame - voice->loop_begin_frame);
    fraction = (fraction & UINT16_MAX) + step;
    voice->position_frame = position + (fraction >> 16);
    voice->fraction_q16 = fraction & UINT16_MAX;
    return true;
}

static void game_audio_mix_constant_spans(
    TilefinchGameAudio *audio, int16_t *stereo, size_t frames,
    const uint8_t *slots, size_t count, const uint8_t *paths,
    const uint32_t *steps, const uint16_t *left_gains,
    const uint16_t *right_gains)
{
    /* The admission below excludes automation, events and stereo buffers.
       Retain voice order, exact signed division, phase/position updates, and
       one final clip. Four scaled voices cannot overflow the int32 sum. */
    _Static_assert(TILEFINCH_GAME_AUDIO_VOICE_LIMIT <= 4u,
                   "constant accumulation bound");
    int32_t *sum = audio->constant_accum;
    memset(sum, 0, frames * 2u * sizeof(*sum));
    for (size_t at = 0; at < count; at++) {
        size_t i = slots[at];
        GameAudioVoice *voice = &audio->voices[i];
        uint8_t path = paths[i];
        bool silent = (path & 8u) != 0u, shared = (path & 4u) != 0u;
        if ((path & 3u) == 1u) {
            if (silent) voice->oscillator_phase += steps[i] * (uint32_t) frames;
            else if (shared) {
                for (size_t frame = 0; frame < frames; frame++) {
                    int sample = game_audio_oscillator_sample(
                        voice->oscillator_type, voice->oscillator_phase);
                    int mixed = sample * left_gains[i] / 4096;
                    sum[frame * 2u] += mixed;
                    sum[frame * 2u + 1u] += mixed;
                    voice->oscillator_phase += steps[i];
                }
            } else {
                for (size_t frame = 0; frame < frames; frame++) {
                    int sample = game_audio_oscillator_sample(
                        voice->oscillator_type, voice->oscillator_phase);
                    sum[frame * 2u] += sample * left_gains[i] / 4096;
                    sum[frame * 2u + 1u] += sample * right_gains[i] / 4096;
                    voice->oscillator_phase += steps[i];
                }
            }
        } else {
            GameAudioBuffer *buffer = &audio->buffers[voice->buffer_index];
            for (size_t frame = 0; frame < frames; frame++) {
                if (voice->loop && voice->position_frame >= voice->loop_end_frame)
                    voice->position_frame = voice->loop_begin_frame;
                if (!silent) {
                    int sample = buffer->samples[voice->position_frame];
                    int mixed = sample * left_gains[i] / 4096;
                    sum[frame * 2u] += mixed;
                    sum[frame * 2u + 1u] += shared ? mixed
                        : sample * right_gains[i] / 4096;
                }
                uint32_t fraction = voice->fraction_q16 + steps[i];
                voice->position_frame += voice->step_whole + (fraction >> 16);
                voice->fraction_q16 = fraction & 0xffffu;
            }
        }
    }
    for (size_t frame = 0; frame < frames; frame++) {
        int left = sum[frame * 2u], right = sum[frame * 2u + 1u];
        if (left < INT16_MIN) left = INT16_MIN;
        if (left > INT16_MAX) left = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        stereo[frame * 2u] = (int16_t) left;
        stereo[frame * 2u + 1u] = (int16_t) right;
    }
}

#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
static uint32_t game_audio_test_curve_fast_samples;
#endif

/* Active-span callers hold complete, nonempty, unfinished mixer snapshots.
   Keep the general evaluator for first samples, discontinuities, delayed
   starts and endpoints; inline only its ordinary sequential interior arm. */
static inline __attribute__((always_inline)) bool game_audio_active_curve_value(
    GameAudioCurveState *state, uint32_t frame, uint32_t *value, bool optimized)
{
    uint32_t elapsed = frame - state->start;
    if (!optimized || !state->positioned || frame != state->last_frame + 1u
        || elapsed >= state->duration || (int32_t) elapsed < 0)
        return game_audio_curve_value(state, frame, value, optimized);
    state->position += state->step;
    state->error += state->remainder;
    if (state->error >= state->duration) {
        state->position++;
        state->error -= state->duration;
    }
    state->last_frame = frame;
    uint32_t at = state->position >> 16;
    *value = game_audio_curve_interpolate(state->points[at], state->points[at + 1u],
                                         state->position & 0xffffu);
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
    game_audio_test_curve_fast_samples++;
#endif
    return true;
}

/* Compact specialization. Source and shared gains are selected once per
   voice, not once per sample. */
static __attribute__((noinline)) void game_audio_mix_active_voice(
    TilefinchGameAudio *audio, GameAudioVoice *voice, int32_t *sum, size_t frames,
    uint32_t block_start, uint32_t step, uint16_t left_gain, uint16_t right_gain,
    bool fast_enabled, bool automation_enabled, bool measure, bool general,
    bool shared)
{
    GameAudioCurveState *gain = &voice->mixer_gain_curve;
    GameAudioCurveState *pitch = &voice->mixer_pitch_curve;
    uint32_t multipliers = gain->multipliers_q8;
    const uint32_t left_multiplier = multipliers & 0xffffu;
    const uint32_t right_multiplier = multipliers >> 16;
    bool oscillator = voice->kind == 1u;
    uint32_t phase = voice->oscillator_phase;
    uint32_t position = voice->position_frame;
    uint32_t fraction = voice->fraction_q16;
    const uint32_t whole = voice->step_whole;
    GameAudioBuffer *buffer = oscillator ? NULL
        : &audio->buffers[voice->buffer_index];
    for (size_t frame = 0; frame < frames; frame++) {
        uint32_t output_frame = block_start + (uint32_t) frame;
        uint32_t curve_value = 0u;
        if (gain->count != 0u && !gain->finished
            && game_audio_active_curve_value(gain, output_frame, &curve_value,
                                             automation_enabled)) {
            uint32_t curve_left = curve_value * left_multiplier >> 12;
            uint32_t curve_right = shared ? curve_left
                : automation_enabled && left_multiplier == right_multiplier
                    ? curve_left : curve_value * right_multiplier >> 12;
            left_gain = (uint16_t) (curve_left > 16384u ? 16384u : curve_left);
            right_gain = (uint16_t) (curve_right > 16384u ? 16384u : curve_right);
        }
        bool gain_zero = left_gain == 0u && right_gain == 0u;
        bool silent = fast_enabled && gain_zero;
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
        if (general && silent && !audio->reference_mix) audio->silent_samples++;
#endif
        int sample_left = 0;
        int sample_right = 0;
        if (oscillator) {
            if (!silent) {
                sample_left = game_audio_oscillator_sample(
                    voice->oscillator_type, phase);
                sample_right = sample_left;
            }
            if (pitch->count != 0u && !pitch->finished)
                game_audio_active_curve_value(pitch, output_frame, &step,
                                              automation_enabled);
            phase += step;
        } else {
            if (voice->loop && position >= voice->loop_end_frame)
                position = voice->loop_begin_frame;
            if (!silent) {
                sample_left = buffer->samples[position];
                sample_right = sample_left;
            }
            fraction += step;
            position += whole + (fraction >> 16);
            fraction &= 0xffffu;
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (measure && general) {
            if (gain_zero) audio->validation_work.silent++;
            else audio->validation_work.audible++;
            if ((gain->count != 0u && !gain->finished)
                || (pitch->count != 0u && !pitch->finished))
                audio->validation_work.automated++;
            else audio->validation_work.constant++;
        }
#else
        (void) measure;
        (void) general;
#endif
        if (!silent) {
            int mixed_left = sample_left * left_gain / 4096;
            sum[frame * 2u] += mixed_left;
            sum[frame * 2u + 1u] += shared ? mixed_left
                : automation_enabled && sample_left == sample_right
                    && left_gain == right_gain ? mixed_left
                    : sample_right * right_gain / 4096;
        }
    }
    if (oscillator) voice->oscillator_phase = phase;
    else {
        voice->position_frame = position;
        voice->fraction_q16 = fraction;
    }
}

static void game_audio_mix_active_spans(
    TilefinchGameAudio *audio, int16_t *stereo, size_t frames,
    uint32_t block_start, const uint8_t *slots, size_t count,
    const uint32_t *steps, const uint16_t *left, const uint16_t *right,
    bool fast_enabled, bool automation_enabled, bool measure,
    const uint8_t *paths)
{
    int32_t *sum = audio->constant_accum;
    memset(sum, 0, frames * 2u * sizeof(*sum));
    for (size_t at = 0; at < count; at++) {
        size_t i = slots[at];
        GameAudioVoice *v = &audio->voices[i];
        GameAudioCurveState *g = &v->mixer_gain_curve;
        bool shared = automation_enabled && left[i] == right[i]
            && (g->count == 0u || g->finished
                || (g->multipliers_q8 & 0xffffu) == (g->multipliers_q8 >> 16));
        game_audio_mix_active_voice(audio, v, sum, frames, block_start, steps[i],
            left[i], right[i], fast_enabled, automation_enabled, measure,
            paths[i] == 0u, shared);
    }
    for (size_t frame = 0; frame < frames; frame++) {
        int left = sum[frame * 2u];
        int right = sum[frame * 2u + 1u];
        if (left < INT16_MIN) left = INT16_MIN;
        if (left > INT16_MAX) left = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        stereo[frame * 2u] = (int16_t) left;
        stereo[frame * 2u + 1u] = (int16_t) right;
    }
}


bool tilefinch_game_audio_mix(TilefinchGameAudio *audio, int16_t *stereo,
                              size_t frames)
{
    if (audio == NULL || stereo == NULL
        || frames > TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool measure = atomic_load_explicit(&game_audio_validation_mix_enabled, memory_order_acquire)
        && atomic_load_explicit(&game_audio_validation_work_enabled, memory_order_acquire);
    if (audio->validation_work_override != 0u)
        measure = audio->validation_work_override == 2u;
    audio->validation_work_counted = measure;
    memset(&audio->validation_work, 0, sizeof(audio->validation_work));
#endif
    memset(stereo, 0, frames * 2u * sizeof(*stereo));
    if (atomic_load_explicit(&audio->suspended, memory_order_acquire))
        return true;
    bool fast_enabled = true;
    bool constant_enabled = true;
    bool automation_enabled = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    fast_enabled = atomic_load_explicit(&game_audio_validation_mix_fast_enabled, memory_order_acquire);
    constant_enabled = atomic_load_explicit(&game_audio_validation_constant_enabled, memory_order_acquire);
    automation_enabled = atomic_load_explicit(&game_audio_validation_automation_enabled, memory_order_acquire);
#endif
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
    fast_enabled = !audio->reference_mix;
    constant_enabled = !audio->reference_mix && !audio->reference_constant_mix;
    automation_enabled = !audio->reference_mix && !audio->reference_automation_mix;
#endif
    bool claimed[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    bool alive[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    bool completed[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    bool fast_forwarded[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    uint32_t steps[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t stop_frames[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t envelope_counts[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint16_t gains_left[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint16_t gains_right[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint8_t constant_paths[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t block_start = atomic_load_explicit(
        &audio->output_frame, memory_order_acquire);
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        uint32_t expected = 1u;
        claimed[i] = atomic_compare_exchange_strong_explicit(
            &audio->voices[i].active, &expected, 3u,
            memory_order_acq_rel, memory_order_acquire);
        alive[i] = claimed[i];
        if (claimed[i]) {
            game_audio_curve_snapshot(&audio->voices[i].gain_curve,
                                       &audio->voices[i].mixer_gain_curve);
            game_audio_curve_snapshot(&audio->voices[i].pitch_curve,
                                       &audio->voices[i].mixer_pitch_curve);
            steps[i] = atomic_load_explicit(
                &audio->voices[i].step_fraction, memory_order_acquire);
            stop_frames[i] = atomic_load_explicit(
                &audio->voices[i].stop_output_frame, memory_order_acquire);
            uint32_t gains = atomic_load_explicit(
                &audio->voices[i].gains_q8, memory_order_acquire);
            uint32_t envelope_generation = 0u;
            uint32_t envelope_count = 0u;
            bool envelope_snapshot_stable = false;
            /* A cancellation publishes a new generation separately from the
               replacement segment count. Take a bounded seqlock-style
               snapshot so a browser-thread reschedule cannot splice the old
               generation to the new segment array. A racing update waits at
               most one 512-frame output block. */
            for (size_t attempt = 0u; attempt < 2u; attempt++) {
                envelope_generation = atomic_load_explicit(
                    &audio->voices[i].envelope_generation,
                    memory_order_acquire);
                envelope_count = atomic_load_explicit(
                    &audio->voices[i].envelope_count,
                    memory_order_acquire);
                uint32_t generation_after = atomic_load_explicit(
                    &audio->voices[i].envelope_generation,
                    memory_order_acquire);
                if (generation_after == envelope_generation) {
                    envelope_snapshot_stable = true;
                    break;
                }
            }
            if (!envelope_snapshot_stable) envelope_count = 0u;
            envelope_counts[i] = envelope_count;
            if (audio->voices[i].mixer_envelope_generation
                    != envelope_generation) {
                audio->voices[i].mixer_envelope_generation =
                    envelope_generation;
                audio->voices[i].mixer_envelope_cursor = 0u;
                audio->voices[i].mixer_coefficient_q15 = 0u;
                audio->voices[i].mixer_gain_left_q12 =
                    (int32_t) (gains & 0xffffu) << 4;
                audio->voices[i].mixer_gain_right_q12 =
                    (int32_t) (gains >> 16) << 4;
            } else if (envelope_count == 0u) {
                audio->voices[i].mixer_gain_left_q12 =
                    (int32_t) (gains & 0xffffu) << 4;
                audio->voices[i].mixer_gain_right_q12 =
                    (int32_t) (gains >> 16) << 4;
                audio->voices[i].mixer_coefficient_q15 = 0u;
            }
            gains_left[i] = (uint16_t)
                audio->voices[i].mixer_gain_left_q12;
            gains_right[i] = (uint16_t)
                audio->voices[i].mixer_gain_right_q12;
            /* A completed curve holds one constant endpoint. Apply it once
               per block, not through interpolation and stereo multipliers
               on every sample for the rest of this voice's lifetime. */
            GameAudioCurveState *gain_curve = &audio->voices[i].mixer_gain_curve;
            if (gain_curve->count >= 2u && gain_curve->finished) {
                uint32_t value = gain_curve->points[gain_curve->count - 1u];
                uint32_t multipliers = gain_curve->multipliers_q8;
                uint32_t left = value * (multipliers & 0xffffu) >> 12;
                uint32_t right = value * (multipliers >> 16) >> 12;
                gains_left[i] = (uint16_t) (left > 16384u ? 16384u : left);
                gains_right[i] = (uint16_t) (right > 16384u ? 16384u : right);
            }
            GameAudioCurveState *pitch_curve = &audio->voices[i].mixer_pitch_curve;
            if (pitch_curve->count >= 2u && pitch_curve->finished)
                steps[i] = pitch_curve->points[pitch_curve->count - 1u];
            GameAudioVoice *voice = &audio->voices[i];
            /* Only skip a whole silent block when the stable snapshots
               prove there is no event within it. Unsigned multiplication has
               exactly the same phase wrap as repeated per-sample addition.
               Sub-frame looping buffers also have an exact bulk advance;
               other buffer rates and complicated automation stay unchanged. */
            if (fast_enabled && frames != 0u
                && gains_left[i] == 0u && gains_right[i] == 0u
                && (gain_curve->count == 0u || gain_curve->finished)
                && (pitch_curve->count == 0u || pitch_curve->finished)
                && envelope_count == voice->mixer_envelope_cursor
                && (voice->mixer_coefficient_q15 == 0u
                    || (voice->mixer_gain_left_q12 == 0
                        && voice->mixer_gain_right_q12 == 0
                        && voice->mixer_target_left_q12 == 0
                        && voice->mixer_target_right_q12 == 0))
                && game_audio_frame_reached(block_start, voice->start_output_frame)
                && !game_audio_frame_reached(
                    block_start + (uint32_t) frames - 1u, stop_frames[i])
                && game_audio_fast_forward_silent(voice, steps[i], frames)) {
                fast_forwarded[i] = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                if (measure) {
                    audio->validation_work.silent += (uint32_t) frames;
                    audio->validation_work.constant += (uint32_t) frames;
                    audio->validation_work.fast_forwarded += (uint32_t) frames;
                }
#endif
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
                audio->fast_forwarded_samples += (uint32_t) frames;
#endif
            }
        }
    }
    /* Compact once per block, retaining voice order. Silent fast-forwarded
       voices and unclaimed slots need no per-sample branch at all. */
    uint8_t mixing_slots[TILEFINCH_GAME_AUDIO_VOICE_LIMIT];
    size_t mixing_count = 0;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++)
        if (!fast_enabled || (alive[i] && !fast_forwarded[i])) {
            mixing_slots[mixing_count++] = (uint8_t) i;
            GameAudioVoice *voice = &audio->voices[i];
            GameAudioCurveState *gain = &voice->mixer_gain_curve;
            GameAudioCurveState *pitch = &voice->mixer_pitch_curve;
            /* Admit the whole block only from stable snapshots. No start,
               stop, envelope segment or curve transition can occur here.
               Keep recurring-envelope state intact for later cancellation. */
            if (!constant_enabled || !fast_enabled || !alive[i] || frames == 0u
                || (gain->count != 0u && !gain->finished)
                || (pitch->count != 0u && !pitch->finished)
                || envelope_counts[i] != voice->mixer_envelope_cursor
                || (voice->mixer_coefficient_q15 != 0u
                    && (gain->count != 0u
                        || voice->mixer_gain_left_q12 != voice->mixer_target_left_q12
                        || voice->mixer_gain_right_q12 != voice->mixer_target_right_q12))
                || !game_audio_frame_reached(block_start, voice->start_output_frame)
                || game_audio_frame_reached(
                    block_start + (uint32_t) frames - 1u, stop_frames[i])) continue;
            uint8_t path = 1u; /* oscillator */
            if (voice->kind != 1u) {
                if (!voice->loop) {
                    /* Nonlooping completion remains on the general path.
                       Use wide admission arithmetic, never a wrapped sum. */
                    if (voice->position_frame >= voice->end_frame) continue;
                    uint64_t advance = (uint64_t) voice->step_whole * frames
                        + (((uint64_t) steps[i] * frames + voice->fraction_q16) >> 16);
                    if (advance >= voice->end_frame - voice->position_frame) continue;
                }
                path = audio->buffers[voice->buffer_index].channels == 1u ? 2u : 3u;
            }
            if (path != 3u && gains_left[i] == gains_right[i]) path |= 4u;
            if (gains_left[i] == 0u && gains_right[i] == 0u) path |= 8u;
            constant_paths[i] = path;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (measure) {
                audio->validation_work.constant += (uint32_t) frames;
                if ((path & 8u) != 0u) audio->validation_work.silent += (uint32_t) frames;
                else audio->validation_work.audible += (uint32_t) frames;
            }
#endif
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
            audio->constant_path_samples += (uint32_t) frames;
            if ((path & 8u) != 0u) audio->silent_samples += (uint32_t) frames;
#endif
        }
    bool span_safe = false;
    if (audio->constant_accum != NULL && fast_enabled) {
        bool admitted = true, audible_oscillator = false;
        size_t buffers = 0;
        /* Reuse the existing stable-snapshot admission exactly. Incomplete
           automation, stereo buffers and more than one buffer keep the old
           sample-major loop. Silent fast-forwarded voices are already done. */
        for (size_t at = 0; at < mixing_count; at++) {
            size_t i = mixing_slots[at];
            uint8_t path = constant_paths[i], kind = path & 3u;
            if (path == 0u || kind == 3u
                || (kind == 2u && ++buffers > 1u)) {
                admitted = false;
                break;
            }
            if (kind == 1u && (path & 8u) == 0u) audible_oscillator = true;
        }
        span_safe = admitted && audible_oscillator;
    }
    bool active_safe = false;
    if (!span_safe && audio->constant_accum && fast_enabled && frames != 0u) {
        bool admitted = true;
        bool has_active = false;
        size_t buffers = 0;
        for (size_t at = 0; at < mixing_count; at++) {
            size_t i = mixing_slots[at];
            GameAudioVoice *v = &audio->voices[i];
            GameAudioCurveState *g = &v->mixer_gain_curve;
            GameAudioCurveState *p = &v->mixer_pitch_curve;
            if (!alive[i]
                || envelope_counts[i] != v->mixer_envelope_cursor
                || v->mixer_coefficient_q15 != 0u
                || !game_audio_frame_reached(block_start, v->start_output_frame)
                || game_audio_frame_reached(
                    block_start + (uint32_t) frames - 1u, stop_frames[i])) {
                admitted = false;
                break;
            }
            if (v->kind != 1u) {
                if (audio->buffers[v->buffer_index].channels != 1u
                    || ++buffers > 1u) {
                    admitted = false;
                    break;
                }
                if (!v->loop) {
                    if (v->position_frame >= v->end_frame) {
                        admitted = false;
                        break;
                    }
                    uint64_t advance = (uint64_t) v->step_whole * frames
                        + (((uint64_t) steps[i] * frames + v->fraction_q16) >> 16);
                    if (advance >= v->end_frame - v->position_frame) {
                        admitted = false;
                        break;
                    }
                }
            }
            has_active |= (g->count != 0u && !g->finished)
                || (v->kind == 1u && p->count != 0u && !p->finished);
        }
        active_safe = admitted && has_active;
    }
    if (span_safe) {
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
        audio->constant_span_blocks++;
#endif
        game_audio_mix_constant_spans(audio, stereo, frames, mixing_slots,
            mixing_count, constant_paths, steps, gains_left, gains_right);
    } else if (active_safe) {
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
        audio->active_span_blocks++;
#endif
        game_audio_mix_active_spans(audio, stereo, frames, block_start,
            mixing_slots, mixing_count, steps, gains_left, gains_right,
            fast_enabled, automation_enabled,
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            measure
#else
            false
#endif
            , constant_paths);
    } else {
    for (size_t frame = 0; frame < frames; frame++) {
        int left = 0, right = 0;
        uint32_t output_frame = block_start + (uint32_t) frame;
        for (size_t mixing_at = 0; mixing_at < mixing_count; mixing_at++) {
            size_t i = mixing_slots[mixing_at];
            GameAudioVoice *voice = &audio->voices[i];
            uint8_t path = constant_paths[i];
            if (path != 0u) {
                int sample_left = 0, sample_right = 0;
                if ((path & 3u) == 1u) {
                    if ((path & 8u) == 0u) {
                        sample_left = game_audio_oscillator_sample(
                            voice->oscillator_type, voice->oscillator_phase);
                        sample_right = sample_left;
                    }
                    voice->oscillator_phase += steps[i];
                } else {
                    if (voice->loop && voice->position_frame >= voice->loop_end_frame)
                        voice->position_frame = voice->loop_begin_frame;
                    GameAudioBuffer *buffer = &audio->buffers[voice->buffer_index];
                    if ((path & 8u) == 0u) {
                        size_t at = (size_t) voice->position_frame * buffer->channels;
                        sample_left = buffer->samples[at];
                        sample_right = (path & 3u) == 3u ? buffer->samples[at + 1u] : sample_left;
                    }
                    uint32_t fraction = voice->fraction_q16 + steps[i];
                    voice->position_frame += voice->step_whole + (fraction >> 16);
                    voice->fraction_q16 = fraction & 0xffffu;
                }
                if ((path & 8u) == 0u) {
                    int mixed_left = sample_left * gains_left[i] / 4096;
                    left += mixed_left;
                    right += (path & 4u) != 0u ? mixed_left
                        : sample_right * gains_right[i] / 4096;
                }
                continue;
            }
            if (!alive[i]) continue;
            if (game_audio_frame_reached(output_frame, stop_frames[i])) {
                alive[i] = false;
                completed[i] = true;
                continue;
            }
            if (!game_audio_frame_reached(
                    output_frame, voice->start_output_frame)) continue;
            uint32_t envelope_count = envelope_counts[i];
            while (voice->mixer_envelope_cursor < envelope_count) {
                GameAudioEnvelopeSegment *segment =
                    &voice->envelope[voice->mixer_envelope_cursor];
                uint32_t start = atomic_load_explicit(
                    &segment->start_output_frame, memory_order_relaxed);
                if (!game_audio_frame_reached(output_frame, start)) break;
                uint32_t targets = atomic_load_explicit(
                    &segment->targets_q12, memory_order_relaxed);
                voice->mixer_target_left_q12 =
                    (int32_t) (targets & 0xffffu);
                voice->mixer_target_right_q12 =
                    (int32_t) (targets >> 16);
                voice->mixer_coefficient_q15 = atomic_load_explicit(
                    &segment->coefficient_q15, memory_order_relaxed);
                voice->mixer_envelope_cursor++;
            }
            if (voice->mixer_coefficient_q15 != 0u) {
                bool shared = automation_enabled
                    && voice->mixer_gain_left_q12 == voice->mixer_gain_right_q12
                    && voice->mixer_target_left_q12 == voice->mixer_target_right_q12;
                int32_t delta_left = voice->mixer_target_left_q12
                    - voice->mixer_gain_left_q12;
                int32_t delta_right = voice->mixer_target_right_q12
                    - voice->mixer_gain_right_q12;
                voice->mixer_gain_left_q12 +=
                    (delta_left * (int32_t) voice->mixer_coefficient_q15)
                    >> 15;
                if (!shared)
                    voice->mixer_gain_right_q12 +=
                        (delta_right * (int32_t) voice->mixer_coefficient_q15)
                        >> 15;
                if (delta_left >= -1 && delta_left <= 1)
                    voice->mixer_gain_left_q12 =
                        voice->mixer_target_left_q12;
                if (shared)
                    voice->mixer_gain_right_q12 = voice->mixer_gain_left_q12;
                else if (delta_right >= -1 && delta_right <= 1)
                    voice->mixer_gain_right_q12 =
                        voice->mixer_target_right_q12;
                gains_left[i] = (uint16_t) voice->mixer_gain_left_q12;
                gains_right[i] = (uint16_t) voice->mixer_gain_right_q12;
            }
            uint32_t curve_value = 0u;
            if (voice->mixer_gain_curve.count != 0u
                && !voice->mixer_gain_curve.finished
                && game_audio_curve_value(
                    &voice->mixer_gain_curve, output_frame, &curve_value,
                    automation_enabled)) {
                uint32_t multipliers = voice->mixer_gain_curve.multipliers_q8;
                uint32_t curve_left = curve_value * (multipliers & 0xffffu) >> 12;
                uint32_t curve_right = automation_enabled
                    && (multipliers & 0xffffu) == (multipliers >> 16)
                    ? curve_left : curve_value * (multipliers >> 16) >> 12;
                gains_left[i] = (uint16_t) (curve_left > 16384u ? 16384u : curve_left);
                gains_right[i] = (uint16_t) (curve_right > 16384u ? 16384u : curve_right);
            }
            int sample_left = 0, sample_right = 0;
            bool gain_zero = gains_left[i] == 0u && gains_right[i] == 0u;
            bool silent = fast_enabled && gain_zero;
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
            if (silent && !audio->reference_mix) audio->silent_samples++;
#endif
            if (voice->kind == 1u) {
                if (!silent) {
                    sample_left = game_audio_oscillator_sample(
                        voice->oscillator_type, voice->oscillator_phase);
                    sample_right = sample_left;
                }
                if (voice->mixer_pitch_curve.count != 0u
                    && !voice->mixer_pitch_curve.finished)
                    game_audio_curve_value(
                        &voice->mixer_pitch_curve, output_frame, &steps[i],
                        automation_enabled);
                voice->oscillator_phase += steps[i];
            } else {
                uint32_t boundary = voice->loop
                    ? voice->loop_end_frame : voice->end_frame;
                if (voice->position_frame >= boundary) {
                    if (voice->loop)
                        voice->position_frame = voice->loop_begin_frame;
                    else {
                        alive[i] = false;
                        completed[i] = true;
                        continue;
                    }
                }
                GameAudioBuffer *buffer =
                    &audio->buffers[voice->buffer_index];
                if (!silent) {
                    size_t at =
                        (size_t) voice->position_frame * buffer->channels;
                    sample_left = buffer->samples[at];
                    sample_right = buffer->channels == 2
                        ? buffer->samples[at + 1u] : sample_left;
                }
                uint32_t fraction = voice->fraction_q16 + steps[i];
                voice->position_frame +=
                    voice->step_whole + (fraction >> 16);
                voice->fraction_q16 = fraction & 0xffffu;
                if (!voice->loop
                    && voice->position_frame >= voice->end_frame) {
                    alive[i] = false;
                    completed[i] = true;
                }
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            if (measure) {
                if (gain_zero) audio->validation_work.silent++;
                else audio->validation_work.audible++;
                if ((voice->mixer_coefficient_q15 != 0u
                        && (voice->mixer_gain_left_q12 != voice->mixer_target_left_q12
                            || voice->mixer_gain_right_q12 != voice->mixer_target_right_q12))
                    || (voice->mixer_gain_curve.count != 0u && !voice->mixer_gain_curve.finished)
                    || (voice->mixer_pitch_curve.count != 0u && !voice->mixer_pitch_curve.finished))
                    audio->validation_work.automated++;
                else audio->validation_work.constant++;
            }
#endif
            if (!silent) {
                int mixed_left = sample_left * gains_left[i] / 4096;
                left += mixed_left;
                right += automation_enabled && sample_left == sample_right
                    && gains_left[i] == gains_right[i] ? mixed_left
                    : sample_right * gains_right[i] / 4096;
            }
        }
        if (left < INT16_MIN) left = INT16_MIN;
        if (left > INT16_MAX) left = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        stereo[frame * 2u] = (int16_t) left;
        stereo[frame * 2u + 1u] = (int16_t) right;
    }
    }
    atomic_store_explicit(
        &audio->output_frame, block_start + (uint32_t) frames,
        memory_order_release);
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        if (!claimed[i]) continue;
        if (!alive[i]) {
            atomic_store_explicit(
                &audio->voices[i].active,
                completed[i] ? 5u : 0u, memory_order_release);
            continue;
        }
        uint32_t expected = 3u;
        if (!atomic_compare_exchange_strong_explicit(
                &audio->voices[i].active, &expected, 1u,
                memory_order_acq_rel, memory_order_acquire)) {
            /* The only valid competing transition is stop(): 3 -> 4. Keep
               the slot pinned until JavaScript observes its ended event. */
            atomic_store_explicit(
                &audio->voices[i].active, 5u, memory_order_release);
        }
    }
    return true;
}

void tilefinch_game_audio_destroy(TilefinchGameAudio *audio)
{
    if (audio == NULL) return;
    tilefinch_game_audio_suspend(audio);
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_BUFFER_LIMIT; i++)
        budget_free(audio->budget, audio->buffers[i].samples);
    Budget *budget = audio->budget;
    budget_free(budget, audio->constant_accum);
    budget_free(budget, audio);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static TilefinchGameAudio *game_audio_probe_create(Budget *budget, uint32_t handles[4])
{
    TilefinchGameAudio *audio = tilefinch_game_audio_create(budget);
    if (audio == NULL) return NULL;
    GameAudioBuffer *buffer = &audio->buffers[0];
    buffer->frames = 97u;
    buffer->sample_rate = 44100u;
    buffer->channels = 1u;
    buffer->generation = 1u;
    buffer->samples = budget_malloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT,
                                             buffer->frames * sizeof(int16_t));
    if (buffer->samples == NULL) goto fail;
    audio->pcm_bytes = buffer->frames * sizeof(int16_t);
    for (size_t at = 0; at < buffer->frames; at++)
        buffer->samples[at] = (int16_t) (at * 7919u);
    /* Do not resume: this corpus has no hardware output or worker thread. */
    atomic_store_explicit(&audio->suspended, false, memory_order_release);
    for (size_t at = 0; at < 3u; at++)
        if (!tilefinch_game_audio_start_oscillator(audio,
                TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE, at == 2u ? 96 : 617,
                at == 2u ? .018 : 0, at == 2u ? .018 : 0, 0, &handles[at])) goto fail;
    if (!tilefinch_game_audio_start(audio, 17u, 0, 0, 1, 0, 0,
                                   true, 0, 0, 0, &handles[3])) goto fail;
    return audio;
fail:
    tilefinch_game_audio_destroy(audio);
    return NULL;
}

static bool game_audio_probe_commands(TilefinchGameAudio *audio,
                                     const uint32_t handles[4], size_t block)
{
    static const float effect[] = {0, .35f, .08f, 0};
    static const float noise[] = {0, .2f, 0};
    static const float pitch[] = {900, 180};
    if (block % 24u == 0u)
        if (!tilefinch_game_audio_cancel_envelope(audio, handles[0])
            || !tilefinch_game_audio_cancel_pitch_curve(audio, handles[0])
            || !tilefinch_game_audio_schedule_curve(audio, handles[0], false,
                effect, 4u, .7, .7, 0, .03)
            || !tilefinch_game_audio_schedule_curve(audio, handles[0], true,
                pitch, 2u, 1, 1, 0, .03)) return false;
    if (block % 41u == 0u)
        if (!tilefinch_game_audio_cancel_envelope(audio, handles[1])
            || !tilefinch_game_audio_schedule_curve(audio, handles[1], false,
                effect, 4u, .35, .35, 0, .04)
            || !tilefinch_game_audio_cancel_envelope(audio, handles[3])
            || !tilefinch_game_audio_schedule_curve(audio, handles[3], false,
                noise, 3u, .6, .6, 0, .05)) return false;
    if (block % 9u == 0u)
        if (!tilefinch_game_audio_update_oscillator(audio, handles[2],
                64 + (block % 3u) * 17u)) return false;
    return true;
}

typedef enum {
    GAME_AUDIO_PROBE_SILENCE,
    GAME_AUDIO_PROBE_CONSTANT,
    GAME_AUDIO_PROBE_AUTOMATION,
    GAME_AUDIO_PROBE_OBSERVER
} GameAudioProbeKind;

static bool game_audio_validation_probe(
    TilefinchGameAudioMixProbe *out, GameAudioProbeKind kind)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    bool old_fast = atomic_load_explicit(&game_audio_validation_mix_fast_enabled, memory_order_acquire);
    bool old_constant = atomic_load_explicit(&game_audio_validation_constant_enabled, memory_order_acquire);
    bool old_automation = atomic_load_explicit(&game_audio_validation_automation_enabled, memory_order_acquire);
    bool constant_compare = kind == GAME_AUDIO_PROBE_CONSTANT;
    bool automation_compare = kind == GAME_AUDIO_PROBE_AUTOMATION;
    bool observer_compare = kind == GAME_AUDIO_PROBE_OBSERVER;
    bool old_measure = atomic_exchange_explicit(&game_audio_validation_mix_enabled, false, memory_order_acq_rel);
    Budget budget;
    budget_init(&budget, 64u * 1024u);
    uint32_t handles_a[4], handles_b[4];
    TilefinchGameAudio *a = game_audio_probe_create(&budget, handles_a);
    TilefinchGameAudio *b = game_audio_probe_create(&budget, handles_b);
    bool okay = a != NULL && b != NULL;
    if (observer_compare && okay) {
        a->validation_work_override = 1u;
        b->validation_work_override = 2u;
    }
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
    if (a != NULL) a->reference_mix = true;
#endif
    int16_t reference[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    int16_t actual[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    for (size_t block = 0; okay && block < 256u; block++) {
        okay = game_audio_probe_commands(a, handles_a, block)
            && game_audio_probe_commands(b, handles_b, block);
        if (!okay) break;
        tilefinch_game_audio_validation_mix_fast(constant_compare || automation_compare || observer_compare);
        tilefinch_game_audio_validation_constant(!constant_compare);
        tilefinch_game_audio_validation_automation(!automation_compare);
#ifdef TILEFINCH_GAME_AUDIO_MIX_TEST
        a->reference_mix = !constant_compare && !automation_compare && !observer_compare;
        a->reference_constant_mix = constant_compare;
        a->reference_automation_mix = automation_compare;
#endif
        okay = okay && tilefinch_game_audio_mix(a, reference, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
        tilefinch_game_audio_validation_mix_fast(true);
        tilefinch_game_audio_validation_constant(true);
        tilefinch_game_audio_validation_automation(true);
        okay = okay && tilefinch_game_audio_mix(b, actual, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
        if (!okay) break;
        if (memcmp(actual, reference, sizeof(actual)) != 0) out->pcm_mismatches++;
        if (memcmp(a->voices, b->voices, sizeof(a->voices)) != 0
            || atomic_load(&a->output_frame) != atomic_load(&b->output_frame))
            out->state_mismatches++;
        out->blocks++;
    }
    tilefinch_game_audio_destroy(a);
    tilefinch_game_audio_destroy(b);
#if defined(PSP)
    for (size_t run = 0; okay && run < 6u; run++) {
        a = game_audio_probe_create(&budget, handles_a);
        okay = a != NULL;
        if (observer_compare && okay) a->validation_work_override = (run & 1u) ? 2u : 1u;
        tilefinch_game_audio_validation_mix_fast(constant_compare || automation_compare || observer_compare || (run & 1u) != 0u);
        tilefinch_game_audio_validation_constant(!constant_compare || (run & 1u) != 0u);
        tilefinch_game_audio_validation_automation(!automation_compare || (run & 1u) != 0u);
        for (size_t block = 0; okay && block < 256u; block++) {
            okay = game_audio_probe_commands(a, handles_a, block);
            if (!okay) break;
            GameAudioCpuSample before = game_audio_cpu_sample();
            okay = okay && tilefinch_game_audio_mix(a, actual, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
            GameAudioCpuSample after = game_audio_cpu_sample();
            if (!before.valid || !after.valid || after.cpu_us < before.cpu_us) {
                okay = false;
                break;
            }
            uint32_t elapsed = (uint32_t) (after.cpu_us - before.cpu_us);
            out->mix_cpu_us[run] += elapsed;
            out->observer_us[run] += before.observer_us + after.observer_us;
            if (elapsed > out->mix_max_us[run]) out->mix_max_us[run] = elapsed;
        }
        tilefinch_game_audio_destroy(a);
    }
#endif
    tilefinch_game_audio_validation_mix_fast(old_fast);
    tilefinch_game_audio_validation_constant(old_constant);
    tilefinch_game_audio_validation_automation(old_automation);
    atomic_store_explicit(&game_audio_validation_mix_enabled, old_measure, memory_order_release);
    return okay && budget.current == 0u && out->pcm_mismatches == 0u && out->state_mismatches == 0u;
}

bool tilefinch_game_audio_validation_mix_probe(TilefinchGameAudioMixProbe *out)
{
    return game_audio_validation_probe(out, GAME_AUDIO_PROBE_SILENCE);
}

bool tilefinch_game_audio_validation_observer_probe(TilefinchGameAudioMixProbe *out)
{
    return game_audio_validation_probe(out, GAME_AUDIO_PROBE_OBSERVER);
}

bool tilefinch_game_audio_validation_constant_probe(TilefinchGameAudioMixProbe *out)
{
    return game_audio_validation_probe(out, GAME_AUDIO_PROBE_CONSTANT);
}

bool tilefinch_game_audio_validation_automation_probe(TilefinchGameAudioMixProbe *out)
{
    return game_audio_validation_probe(out, GAME_AUDIO_PROBE_AUTOMATION);
}
#endif
