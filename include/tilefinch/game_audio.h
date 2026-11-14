#ifndef TILEFINCH_GAME_AUDIO_H
#define TILEFINCH_GAME_AUDIO_H

#include "tilefinch/budget.h"
#include "tilefinch/game_audio_timeline.h"

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
/* Automated AudioParams, engine-wide. A lane exists only once a parameter
   has been automated, and returns when its node is collected. */
#define TILEFINCH_GAME_AUDIO_LANE_LIMIT 16u
/* Automated gains one source passes through (static ones fold together). */
#define TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES 4u
/* AudioBuffer sample rates the engine resamples from. */
#define TILEFINCH_GAME_AUDIO_BUFFER_RATE_MIN 8000u
#define TILEFINCH_GAME_AUDIO_BUFFER_RATE_MAX 96000u
/* BiquadFilterNode coefficients follow automation once per sub-block. */
#define TILEFINCH_GAME_AUDIO_FILTER_FRAMES 32u
/* AnalyserNodes per engine. Each keeps the newest samples of its input in
   a fixed float history the mixer appends to and the browser thread reads. */
#define TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT 2u
#define TILEFINCH_GAME_AUDIO_ANALYSER_HISTORY 4096u
/* The largest AnalyserNode fftSize: its transform runs on the browser
   thread when the page asks for frequency data. */
#define TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX 2048u
/* Microphone samples (MediaStreamAudioSourceNode) wait in one fixed ring
   the browser thread fills from the capture session; a stream voice starts
   once PREFILL samples are waiting and again after running dry. */
#define TILEFINCH_GAME_AUDIO_STREAM_SAMPLES 8192u
#define TILEFINCH_GAME_AUDIO_STREAM_PREFILL 1024u

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

typedef enum {
    TILEFINCH_GAME_AUDIO_FILTER_NONE = 0,
    TILEFINCH_GAME_AUDIO_FILTER_LOWPASS,
    TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS,
    TILEFINCH_GAME_AUDIO_FILTER_BANDPASS,
    TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF,
    TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF,
    TILEFINCH_GAME_AUDIO_FILTER_PEAKING,
    TILEFINCH_GAME_AUDIO_FILTER_NOTCH,
    TILEFINCH_GAME_AUDIO_FILTER_ALLPASS
} TilefinchGameAudioFilterType;

/* BiquadFilterNode parameter order in programs. */
enum {
    TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY = 0,
    TILEFINCH_GAME_AUDIO_FILTER_DETUNE,
    TILEFINCH_GAME_AUDIO_FILTER_Q,
    TILEFINCH_GAME_AUDIO_FILTER_GAIN
};

typedef struct TilefinchGameAudio TilefinchGameAudio;
/* Reference-counted float32 sample storage shared by the engine and the
   ArrayBuffers behind AudioBuffer.getChannelData. */
typedef struct TilefinchGameAudioStorage TilefinchGameAudioStorage;

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
typedef struct {
    uint32_t blocks;
    /* Even runs: four program voices (gain ramp + swept lowpass each); odd
       runs: four integer-path oscillators. PSP CPU microseconds. */
    uint32_t mix_cpu_us[6], mix_max_us[6];
    /* Wall time between the CPU reads: PPSSPP reports no thread CPU. */
    uint32_t mix_wall_us[6];
} TilefinchGameAudioProgramProbe;
bool tilefinch_game_audio_validation_program_probe(TilefinchGameAudioProgramProbe *out);
typedef struct {
    uint32_t blocks;
    /* Even runs: a fed microphone stream voice and a 1 kHz oscillator, each
       into an analyser; odd runs: the same without analysers. */
    uint32_t mix_cpu_us[6], mix_max_us[6], mix_wall_us[6];
    /* One getFloatFrequencyData's work at fftSize 256, 1024 and 2048. */
    uint32_t fft_cpu_us[3], fft_wall_us[3], peak_bin[3];
} TilefinchGameAudioAnalyserProbe;
bool tilefinch_game_audio_validation_analyser_probe(TilefinchGameAudioAnalyserProbe *out);
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
/*
 * Automation lanes: one AudioParam's specification timeline
 * (game_audio_timeline.h), compiled to segments the mixer evaluates per
 * sample without JavaScript. Times are AudioContext seconds; "now" is the
 * context's currentTime. Handles are generation tagged; 0 is never a lane.
 */
bool tilefinch_game_audio_lane_create(TilefinchGameAudio *audio,
                                      double intrinsic, double minimum,
                                      double maximum, uint32_t *lane);
/* The mixer stops reading a released lane before its memory is reused. */
void tilefinch_game_audio_lane_release(TilefinchGameAudio *audio, uint32_t lane);
/* INVALID_STATE for a stale handle; otherwise the timeline's status. */
TilefinchAudioTimelineStatus tilefinch_game_audio_lane_insert(
    TilefinchGameAudio *audio, uint32_t lane, double now,
    TilefinchAudioEventKind kind, double value, double time, double aux,
    const float *curve, size_t count);
TilefinchAudioTimelineStatus tilefinch_game_audio_lane_cancel(
    TilefinchGameAudio *audio, uint32_t lane, double now, double time,
    bool hold);
TilefinchAudioTimelineStatus tilefinch_game_audio_lane_set_value(
    TilefinchGameAudio *audio, uint32_t lane, double now, double value);
bool tilefinch_game_audio_lane_value(TilefinchGameAudio *audio, uint32_t lane,
                                     double time, double *value);
/* Whether the lane's value is fixed from now on (game_audio_timeline.h,
   tilefinch_audio_timeline_settled), so its owner can return it and use a
   static value; anchor restarts a new lane from that value's event. */
bool tilefinch_game_audio_lane_settled(TilefinchGameAudio *audio, uint32_t lane,
                                       double now, double *value, double *anchor,
                                       bool *anchored);
bool tilefinch_game_audio_lane_anchor(TilefinchGameAudio *audio, uint32_t lane,
                                      double time, double value);

/* The biquad coefficients {b0, b1, b2, a1, a2} (normalized: a0 = 1) the
   mixer runs for a filter of TYPE (TilefinchGameAudioFilterType) at these
   values (Hz, cents, Q, dB) and the 44.1 kHz output rate. */
void tilefinch_game_audio_filter_coefficients(uint8_t type, float frequency,
                                              float detune, float q, float gain,
                                              float out[5]);

/*
 * A voice's parameter program: what its source passes through on the way to
 * the destination. Unautomated gains and the panner fold into the static
 * left/right gains; each automated parameter names its lane (0 = none, use
 * the static value). pitch is the oscillator frequency in Hz, or a buffer
 * source's playbackRate; detune is in cents.
 */
typedef struct {
    double gain_left, gain_right;
    uint32_t gain_lanes[TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES];
    uint8_t pre_filter; /* bit i: gain_lanes[i] precedes the filter */
    double pitch, detune;
    uint32_t pitch_lane, detune_lane;
    uint8_t filter;     /* TilefinchGameAudioFilterType */
    double filter_values[4];
    uint32_t filter_lanes[4];
    /* The first AnalyserNode on the path (0 = none) hears the source after
       the static gains (tap_gain), the automated gains (tap_lanes, bit i:
       gain_lanes[i]) and, when tap_after_filter, the filter before it. */
    uint32_t analyser;
    double tap_gain;
    uint8_t tap_lanes;
    bool tap_after_filter;
    /* Nothing reaches the destination (a microphone source whose path does
       not end there); analysers still hear it. */
    bool muted;
    /* The source itself is silent (a microphone source whose track is
       disabled): zeros enter the path, so a filter or analyser on it
       hears silence and a filter rings out as it would on a quiet input. */
    bool source_silent;
} TilefinchGameAudioProgram;

typedef struct {
    uint32_t buffer;     /* buffer handle, or 0 for an oscillator */
    uint8_t oscillator;  /* TilefinchGameAudioOscillatorType when buffer == 0 */
    double offset, duration;
    bool loop;
    double loop_start, loop_end;
    double now, when;    /* context seconds */
} TilefinchGameAudioVoiceStart;

/* A program without lanes or a filter, over integer PCM or an oscillator,
   runs on the integer mixer paths; anything else on the float program
   path. A voice that later gains automation switches paths once. */
bool tilefinch_game_audio_start_program(
    TilefinchGameAudio *audio, const TilefinchGameAudioVoiceStart *start,
    const TilefinchGameAudioProgram *program, uint32_t *voice_handle);
bool tilefinch_game_audio_set_program(
    TilefinchGameAudio *audio, uint32_t voice_handle,
    const TilefinchGameAudioProgram *program);

/*
 * Microphone stream voices (MediaStreamAudioSourceNode). The browser thread
 * writes capture samples (mono, 44.1 kHz, in [-1, 1)) into the engine's
 * stream ring; every stream voice reads it from where it started. Stream
 * voices reach the output only while stream_audible is set (the PSP's
 * headphones are in: no speaker feedback) and their program is unmuted.
 */
bool tilefinch_game_audio_start_stream(
    TilefinchGameAudio *audio, double now,
    const TilefinchGameAudioProgram *program, uint32_t *voice_handle);
void tilefinch_game_audio_stream_write(TilefinchGameAudio *audio,
                                       const float *samples, size_t count);
void tilefinch_game_audio_stream_audible(TilefinchGameAudio *audio, bool audible);
/* Clears the stream ring once no stream voice plays (false: one still does). */
bool tilefinch_game_audio_stream_reset(TilefinchGameAudio *audio);

/*
 * AnalyserNode histories. read copies the newest COUNT (at most
 * TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX) input samples, oldest first, and
 * *STAMP, which changes as the output advances; an analyser that no
 * playing voice feeds reads silence.
 */
bool tilefinch_game_audio_analyser_create(TilefinchGameAudio *audio, uint32_t *handle);
void tilefinch_game_audio_analyser_release(TilefinchGameAudio *audio, uint32_t handle);
bool tilefinch_game_audio_analyser_read(TilefinchGameAudio *audio, uint32_t handle,
                                        float *out, size_t count, uint32_t *stamp);
/* The AnalyserNode frequency analysis of the Web Audio specification over
   N (a power of two, 32 to TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX) time-domain
   samples: Blackman window, FFT, magnitude / N, then smoothing over time
   into SPECTRUM (N / 2 values, kept between calls). SCRATCH holds 2N
   floats; TIME is overwritten. */
void tilefinch_game_audio_spectrum(float *time, size_t n, float smoothing,
                                   float *spectrum, float *scratch);

/*
 * Float32 AudioBuffers (createBuffer, or a decoded buffer once its samples
 * are read). Planar storage counts 4 bytes per sample against the PCM pool.
 * channel_data returns the live plane, and a reference the caller must drop
 * with tilefinch_game_audio_storage_release; writes are heard when the mixer
 * next reaches those frames.
 */
bool tilefinch_game_audio_create_buffer(
    TilefinchGameAudio *audio, uint32_t channels, uint32_t frames,
    uint32_t sample_rate, TilefinchGameAudioBufferInfo *info);
float *tilefinch_game_audio_channel_data(
    TilefinchGameAudio *audio, uint32_t handle, uint32_t channel,
    TilefinchGameAudioStorage **storage);
void tilefinch_game_audio_storage_release(TilefinchGameAudioStorage *storage);
/* copyFromChannel without promoting integer PCM to float. */
bool tilefinch_game_audio_copy_from_channel(
    TilefinchGameAudio *audio, uint32_t handle, uint32_t channel,
    uint32_t start, float *out, size_t count);
/* The buffer's slot returns once no voice plays it. */
void tilefinch_game_audio_buffer_release(TilefinchGameAudio *audio,
                                         uint32_t handle);
size_t tilefinch_game_audio_pcm_bytes(const TilefinchGameAudio *audio);

#if !defined(PSP) || defined(TILEFINCH_PSP_VALIDATION_LOG)
/* Fixed validation renders (no output device): host tests dump them as WAV
   and PSP validation runs log their envelopes. frames is a multiple of the
   512-frame block. Absent from shipping PSP builds. */
enum {
    TILEFINCH_GAME_AUDIO_SCENARIO_BEEP,  /* 880 Hz sine: linear attack, exp decay */
    TILEFINCH_GAME_AUDIO_SCENARIO_NOISE, /* createBuffer noise, setTargetAtTime release */
    TILEFINCH_GAME_AUDIO_SCENARIO_SWEEP, /* sawtooth under a swept Q 8 lowpass */
    TILEFINCH_GAME_AUDIO_SCENARIO_FOUR,  /* four ramped, filtered oscillators */
    TILEFINCH_GAME_AUDIO_SCENARIOS
};
bool tilefinch_game_audio_validation_scenario(Budget *budget, unsigned scenario,
                                              int16_t *stereo, size_t frames);
/* Left-channel RMS of each 2048-frame window, rounded. */
bool tilefinch_game_audio_validation_scenario_envelope(Budget *budget, unsigned scenario,
                                                       uint32_t *rms, size_t windows);
#endif

/* Deterministic host/test seam and the PSP worker's one bounded mixer unit. */
bool tilefinch_game_audio_mix(TilefinchGameAudio *audio, int16_t *stereo,
                              size_t frames);

#endif
