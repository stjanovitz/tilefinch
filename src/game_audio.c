#include "tilefinch/game_audio.h"

#include <math.h>
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
    uint8_t kind;
    uint8_t oscillator_type;
    bool loop;
} GameAudioVoice;

struct TilefinchGameAudio {
    Budget *budget;
    GameAudioBuffer buffers[TILEFINCH_GAME_AUDIO_BUFFER_LIMIT];
    GameAudioVoice voices[TILEFINCH_GAME_AUDIO_VOICE_LIMIT];
    size_t pcm_bytes;
    _Atomic uint32_t output_frame;
    _Atomic bool suspended;
    _Atomic bool stop;
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
        (void) tilefinch_game_audio_mix(
            audio, block, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
        if (sceAudioOutputBlocking(
                audio->channel, PSP_AUDIO_VOLUME_MAX, block) < 0) break;
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

bool tilefinch_game_audio_mix(TilefinchGameAudio *audio, int16_t *stereo,
                              size_t frames)
{
    if (audio == NULL || stereo == NULL
        || frames > TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES) return false;
    memset(stereo, 0, frames * 2u * sizeof(*stereo));
    if (atomic_load_explicit(&audio->suspended, memory_order_acquire))
        return true;
    bool claimed[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    bool alive[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    bool completed[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {false};
    uint32_t steps[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t stop_frames[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t envelope_counts[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint16_t gains_left[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint16_t gains_right[TILEFINCH_GAME_AUDIO_VOICE_LIMIT] = {0};
    uint32_t block_start = atomic_load_explicit(
        &audio->output_frame, memory_order_acquire);
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        uint32_t expected = 1u;
        claimed[i] = atomic_compare_exchange_strong_explicit(
            &audio->voices[i].active, &expected, 3u,
            memory_order_acq_rel, memory_order_acquire);
        alive[i] = claimed[i];
        if (claimed[i]) {
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
        }
    }
    for (size_t frame = 0; frame < frames; frame++) {
        int left = 0, right = 0;
        uint32_t output_frame = block_start + (uint32_t) frame;
        for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
            GameAudioVoice *voice = &audio->voices[i];
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
                int32_t delta_left = voice->mixer_target_left_q12
                    - voice->mixer_gain_left_q12;
                int32_t delta_right = voice->mixer_target_right_q12
                    - voice->mixer_gain_right_q12;
                voice->mixer_gain_left_q12 +=
                    (delta_left * (int32_t) voice->mixer_coefficient_q15)
                    >> 15;
                voice->mixer_gain_right_q12 +=
                    (delta_right * (int32_t) voice->mixer_coefficient_q15)
                    >> 15;
                if (delta_left >= -1 && delta_left <= 1)
                    voice->mixer_gain_left_q12 =
                        voice->mixer_target_left_q12;
                if (delta_right >= -1 && delta_right <= 1)
                    voice->mixer_gain_right_q12 =
                        voice->mixer_target_right_q12;
                gains_left[i] = (uint16_t) voice->mixer_gain_left_q12;
                gains_right[i] = (uint16_t) voice->mixer_gain_right_q12;
            }
            int sample_left = 0, sample_right = 0;
            if (voice->kind == 1u) {
                sample_left = game_audio_oscillator_sample(
                    voice->oscillator_type, voice->oscillator_phase);
                sample_right = sample_left;
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
                size_t at =
                    (size_t) voice->position_frame * buffer->channels;
                sample_left = buffer->samples[at];
                sample_right = buffer->channels == 2
                    ? buffer->samples[at + 1u] : sample_left;
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
            left += sample_left * gains_left[i] / 4096;
            right += sample_right * gains_right[i] / 4096;
        }
        if (left < INT16_MIN) left = INT16_MIN;
        if (left > INT16_MAX) left = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        stereo[frame * 2u] = (int16_t) left;
        stereo[frame * 2u + 1u] = (int16_t) right;
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
    budget_free(budget, audio);
}
