#ifndef TILEFINCH_GAME_AUDIO_H
#define TILEFINCH_GAME_AUDIO_H

#include "tilefinch/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_GAME_AUDIO_BUFFER_LIMIT 8u
#define TILEFINCH_GAME_AUDIO_VOICE_LIMIT 4u
#define TILEFINCH_GAME_AUDIO_ENVELOPE_SEGMENT_LIMIT 2u
#define TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT (512u * 1024u)
#define TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES 512u
#define TILEFINCH_GAME_AUDIO_SCHEDULE_LIMIT_SECONDS 10.0

typedef enum {
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE = 1,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SQUARE,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_SAWTOOTH,
    TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE
} TilefinchGameAudioOscillatorType;

typedef struct TilefinchGameAudio TilefinchGameAudio;

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
