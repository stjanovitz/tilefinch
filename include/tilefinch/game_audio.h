#ifndef TILEFINCH_GAME_AUDIO_H
#define TILEFINCH_GAME_AUDIO_H

#include "tilefinch/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_GAME_AUDIO_BUFFER_LIMIT 8u
#define TILEFINCH_GAME_AUDIO_VOICE_LIMIT 4u
#define TILEFINCH_GAME_AUDIO_ENVELOPE_SEGMENT_LIMIT 2u
#define TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT 64u
#define TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT (512u * 1024u)
#define TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES 512u
#define TILEFINCH_GAME_AUDIO_SCHEDULE_LIMIT_SECONDS 10.0

typedef enum {
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE = 1,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SQUARE,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SAWTOOTH,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE
} TilefinchGameAudioOscillatorType;

/* Opcodes of the JS Web Audio reference's command channel: the first
   argument of __tilefinchGameAudioCommand (js_game_audio_command), as sent by
   src/bootstrap/game-audio.js and replayed by tests/game_audio_render.c.
   game-audio.js names each one the same without the prefix;
   tests/test_psp_sdk_contracts.py keeps the two lists equal. */
typedef enum {
    TILEFINCH_GAME_AUDIO_COMMAND_RESUME = 0,
    TILEFINCH_GAME_AUDIO_COMMAND_SUSPEND = 1,
    TILEFINCH_GAME_AUDIO_COMMAND_CLOSE = 2,
    TILEFINCH_GAME_AUDIO_COMMAND_START_BUFFER = 3,
    TILEFINCH_GAME_AUDIO_COMMAND_STOP = 4,
    TILEFINCH_GAME_AUDIO_COMMAND_START_OSCILLATOR = 5,
    TILEFINCH_GAME_AUDIO_COMMAND_SET_GAIN = 6,
    TILEFINCH_GAME_AUDIO_COMMAND_SET_FREQUENCY = 7,
    TILEFINCH_GAME_AUDIO_COMMAND_GAIN_TARGET = 8,
    TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_GAIN = 9,
    TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_PITCH = 10,
    TILEFINCH_GAME_AUDIO_COMMAND_CURVE = 11,
    TILEFINCH_GAME_AUDIO_COMMAND_COUNT
} TilefinchGameAudioCommand;

typedef struct TilefinchGameAudio TilefinchGameAudio;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Silence hardware output only; keep voices, envelopes and mixing running. */
void tilefinch_game_audio_validation_mute_output(bool muted);
#define TILEFINCH_GAME_AUDIO_CPU_HISTOGRAM_BINS 128u
#define TILEFINCH_GAME_AUDIO_BLOCK_RECORD_LIMIT 2048u
typedef struct {
    uint64_t begin_before_us, begin_after_us;
    uint64_t mixed_before_us, mixed_after_us;
    uint64_t submitted_before_us, submitted_after_us;
    uint64_t begin_cpu_us, mixed_cpu_us, submitted_cpu_us;
    uint32_t output_frame, frames;
    uint32_t silent, audible, constant, automated, fast_forwarded;
    int32_t output_result;
    bool clocks_valid, work_counted;
} TilefinchGameAudioBlockRecord;
typedef struct {
    uint32_t blocks, mix_cpu_us, output_cpu_us, observer_us;
    uint32_t silent_samples, audible_samples, constant_samples, automated_samples;
    uint32_t fast_forwarded_samples, failed_clock_samples, dropped_blocks;
    uint32_t mix_cpu_max_us, output_cpu_max_us;
    uint32_t block_records, record_overflow;
    /* 100 us bins, last bin includes overflow. CPU, not output blocking time. */
    uint32_t mix_cpu_histogram[TILEFINCH_GAME_AUDIO_CPU_HISTOGRAM_BINS];
} TilefinchGameAudioMixMetrics;
/* Opt-in intrusive block accounting, absent from ordinary builds. Start may
   refuse while the worker publishes; the caller must check the result. */
bool tilefinch_game_audio_validation_mix_measure(bool enabled);
/* Change only between measurement windows. Clocks-only still retains block
   records, but does not execute per-voice/sample diagnostic increments. */
bool tilefinch_game_audio_validation_mix_work(bool enabled);
bool tilefinch_game_audio_validation_mix_metrics(TilefinchGameAudioMixMetrics *out);
/* Post-window only. Records retain clock brackets so crossing blocks are not
   charged in full to every overlapping game frame. Overflow invalidates the
   capture; there is no overwrite/drop-oldest policy. */
bool tilefinch_game_audio_validation_mix_block(
    size_t index, TilefinchGameAudioBlockRecord *out);
/* Same-binary reference arithmetic for device A/B; ordinary builds always
   use the optimized mixer and do not carry this switch. */
void tilefinch_game_audio_validation_mix_fast(bool enabled);
void tilefinch_game_audio_validation_constant(bool enabled);
void tilefinch_game_audio_validation_automation(bool enabled);
typedef struct {
    uint32_t blocks, pcm_mismatches, state_mismatches;
    uint32_t mix_cpu_us[6], observer_us[6], mix_max_us[6];
} TilefinchGameAudioMixProbe;
/* Silent, synchronous command-corpus A/B, outside a gameplay timing window. */
bool tilefinch_game_audio_validation_mix_probe(TilefinchGameAudioMixProbe *out);
/* Identical commands/PCM, alternating clocks-only and counted mixing. */
bool tilefinch_game_audio_validation_observer_probe(TilefinchGameAudioMixProbe *out);
bool tilefinch_game_audio_validation_constant_probe(TilefinchGameAudioMixProbe *out);
bool tilefinch_game_audio_validation_automation_probe(TilefinchGameAudioMixProbe *out);
#endif

typedef struct {
    uint32_t handle;
    uint32_t frames;
    uint32_t sample_rate;
    uint8_t channels;
} TilefinchGameAudioBufferInfo;

TilefinchGameAudio *tilefinch_game_audio_create(Budget *budget);
void tilefinch_game_audio_destroy(TilefinchGameAudio *audio);
bool tilefinch_game_audio_decode_wav(
    TilefinchGameAudio *audio, const unsigned char *bytes, size_t length,
    TilefinchGameAudioBufferInfo *info);
bool tilefinch_game_audio_resume(TilefinchGameAudio *audio);
void tilefinch_game_audio_suspend(TilefinchGameAudio *audio);
bool tilefinch_game_audio_start(
    TilefinchGameAudio *audio, uint32_t handle, double offset_seconds,
    double duration_seconds, double playback_rate, double gain_left,
    double gain_right, bool loop, double loop_start_seconds,
    double loop_end_seconds, double start_delay_seconds,
    uint32_t *voice_handle);
bool tilefinch_game_audio_start_oscillator(
    TilefinchGameAudio *audio, TilefinchGameAudioOscillatorType type,
    double frequency, double gain_left, double gain_right,
    double start_delay_seconds, uint32_t *voice_handle);
bool tilefinch_game_audio_update_voice(
    TilefinchGameAudio *audio, uint32_t voice_handle,
    double gain_left, double gain_right);
/* Two scheduled target segments cover the short attack/release envelopes
   used by game effects without making the browser thread responsible for
   ticking gain. The PSP mixer applies them even while JavaScript is stalled. */
bool tilefinch_game_audio_cancel_envelope(
    TilefinchGameAudio *audio, uint32_t voice_handle);
bool tilefinch_game_audio_schedule_envelope_target(
    TilefinchGameAudio *audio, uint32_t voice_handle,
    double gain_left, double gain_right,
    double start_delay_seconds, double time_constant_seconds);
bool tilefinch_game_audio_update_oscillator(
    TilefinchGameAudio *audio, uint32_t voice_handle, double frequency);
/* One copied curve per parameter, at most 64 points / ten seconds. The
   mixer owns interpolation; gain and oscillator-frequency curves are
   independent. Cancellation restores the parameter's unscheduled value. */
bool tilefinch_game_audio_schedule_curve(
    TilefinchGameAudio *audio, uint32_t voice_handle, bool pitch,
    const float *values, size_t count, double gain_left, double gain_right,
    double start_delay_seconds, double duration_seconds);
bool tilefinch_game_audio_cancel_pitch_curve(
    TilefinchGameAudio *audio, uint32_t voice_handle);
void tilefinch_game_audio_stop(TilefinchGameAudio *audio,
                               uint32_t voice_handle,
                               double stop_delay_seconds);
/* Browser-thread half of the bounded completion mailbox. A completed voice
   remains unavailable for reuse until its generation-tagged notice is
   consumed, so completion can never be attributed to a later sound. */
bool tilefinch_game_audio_take_completion(TilefinchGameAudio *audio,
                                          uint32_t *voice_handle);
/* Deterministic host/test seam and the PSP worker's one bounded mixer unit. */
bool tilefinch_game_audio_mix(TilefinchGameAudio *audio, int16_t *stereo,
                              size_t frames);

#endif
