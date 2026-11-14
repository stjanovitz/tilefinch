/* Program voices rendered by the real mixer: automation lanes evaluated per
   sample without browser ticks, float32 buffers, BiquadFilterNode, voice
   lifetimes, and the lane and buffer bounds. Assertions are on samples. */
#include "tilefinch/budget.h"
#include "tilefinch/game_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        exit(1);                                                             \
    }                                                                        \
} while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RATE 44100.0
#define BLOCK TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES

typedef struct {
    Budget budget;
    TilefinchGameAudio *audio;
    int16_t pcm[BLOCK * 2u];
    double now; /* context seconds at the next block */
} Engine;

static void open_engine(Engine *e)
{
    memset(e, 0, sizeof(*e));
    budget_init(&e->budget, 4u * 1024u * 1024u);
    e->audio = tilefinch_game_audio_create(&e->budget);
    CHECK(e->audio != NULL);
    CHECK(tilefinch_game_audio_resume(e->audio));
}

static void close_engine(Engine *e)
{
    tilefinch_game_audio_destroy(e->audio);
    CHECK(e->budget.current == 0u);
}

static void mix(Engine *e)
{
    size_t allocations = e->budget.allocation_count;
    CHECK(tilefinch_game_audio_mix(e->audio, e->pcm, BLOCK));
    CHECK(e->budget.allocation_count == allocations);
    e->now += BLOCK / RATE;
}

static uint32_t lane(Engine *e, double intrinsic, double low, double high)
{
    uint32_t handle = 0;
    CHECK(tilefinch_game_audio_lane_create(e->audio, intrinsic, low, high, &handle));
    CHECK(handle != 0u);
    return handle;
}

static void event(Engine *e, uint32_t handle, TilefinchAudioEventKind kind,
                  double value, double time, double aux)
{
    CHECK(tilefinch_game_audio_lane_insert(e->audio, handle, e->now, kind, value,
                                          time, aux, NULL, 0)
          == TILEFINCH_AUDIO_TIMELINE_OK);
}

/* A looping float buffer holding one constant value. */
static uint32_t dc_buffer(Engine *e, float value, uint32_t frames)
{
    TilefinchGameAudioBufferInfo info;
    CHECK(tilefinch_game_audio_create_buffer(e->audio, 1, frames, 44100, &info));
    TilefinchGameAudioStorage *storage = NULL;
    float *plane = tilefinch_game_audio_channel_data(e->audio, info.handle, 0, &storage);
    CHECK(plane != NULL && storage != NULL);
    for (uint32_t at = 0; at < frames; at++) plane[at] = value;
    tilefinch_game_audio_storage_release(storage);
    return info.handle;
}

static TilefinchGameAudioProgram unity(void)
{
    TilefinchGameAudioProgram p = {.gain_left = 1, .gain_right = 1, .pitch = 1};
    return p;
}

static uint32_t start(Engine *e, uint32_t buffer, uint8_t oscillator, double when,
                      const TilefinchGameAudioProgram *program, bool loop)
{
    TilefinchGameAudioVoiceStart s = {.buffer = buffer, .oscillator = oscillator,
        .loop = loop, .now = e->now, .when = when};
    uint32_t voice = 0;
    CHECK(tilefinch_game_audio_start_program(e->audio, &s, program, &voice));
    return voice;
}

static void near_sample(int actual, double expected, double tolerance, const char *what,
                        size_t frame)
{
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr, "%s frame %zu: %d, expected %.2f\n", what, frame, actual, expected);
        exit(1);
    }
}

static void test_linear_ramp_per_sample(void)
{
    Engine e;
    open_engine(&e);
    uint32_t g = lane(&e, 1, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_SET, 0, 0, 0);
    event(&e, g, TILEFINCH_AUDIO_EVENT_LINEAR, 1, 1024 / RATE, 0);
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = g;
    uint32_t buffer = dc_buffer(&e, .5f, 4096);
    start(&e, buffer, 0, 0, &p, true);
    for (size_t block = 0; block < 3; block++) {
        mix(&e);
        for (size_t f = 0; f < BLOCK; f++) {
            size_t frame = block * BLOCK + f;
            double ramp = frame >= 1024 ? 1.0 : frame / 1024.0;
            near_sample(e.pcm[f * 2u], 16384.0 * ramp, 1.5, "linear", frame);
            CHECK(e.pcm[f * 2u] == e.pcm[f * 2u + 1u]);
        }
    }
    double value = 0;
    CHECK(tilefinch_game_audio_lane_value(e.audio, g, 512 / RATE, &value));
    CHECK(fabs(value - .5) < 1e-6);
    close_engine(&e);
}

static void test_exponential_beep(void)
{
    /* setValueAtTime(.3, 0); exponentialRampToValueAtTime(.01, .3) */
    Engine e;
    open_engine(&e);
    uint32_t g = lane(&e, 1, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_SET, .3, 0, 0);
    event(&e, g, TILEFINCH_AUDIO_EVENT_EXPONENTIAL, .01, .3, 0);
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = g;
    start(&e, dc_buffer(&e, 1.0f, 2048), 0, 0, &p, true);
    double end = .3 * RATE;
    for (size_t block = 0; block < 32; block++) {
        mix(&e);
        for (size_t f = 0; f < BLOCK; f += 37) {
            size_t frame = block * BLOCK + f;
            double gain = frame >= end ? .01 : .3 * pow(.01 / .3, frame / end);
            near_sample(e.pcm[f * 2u], 32768.0 * gain, 1.0 + 32768.0 * gain * 2e-4,
                        "exponential", frame);
        }
    }
    close_engine(&e);
}

static void test_target_and_curve(void)
{
    Engine e;
    open_engine(&e);
    uint32_t g = lane(&e, 0, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_TARGET, 1, 0, .01);
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = g;
    start(&e, dc_buffer(&e, .5f, 1024), 0, 0, &p, true);
    for (size_t block = 0; block < 4; block++) {
        mix(&e);
        for (size_t f = 0; f < BLOCK; f += 13) {
            size_t frame = block * BLOCK + f;
            near_sample(e.pcm[f * 2u], 16384.0 * (1 - exp(-(frame / RATE) / .01)), 2.0,
                        "target", frame);
        }
    }
    /* A value curve that starts in the next block, then holds its end. */
    static const float shape[] = {0, 1, .5};
    double begin = e.now + 256 / RATE;
    CHECK(tilefinch_game_audio_lane_insert(e.audio, g, e.now, TILEFINCH_AUDIO_EVENT_CURVE,
                                          0, begin, 1024 / RATE, shape, 3)
          == TILEFINCH_AUDIO_TIMELINE_OK);
    double elapsed = e.now;
    for (size_t block = 0; block < 4; block++) {
        mix(&e);
        for (size_t f = 0; f < BLOCK; f += 7) {
            double frame = block * BLOCK + f - 256.0;
            double expected = frame < 0
                ? 1 - exp(-(elapsed + (block * BLOCK + f) / RATE) / .01)
                : frame >= 1024 ? .5
                : frame < 512 ? frame / 512 : 1 - .5 * (frame - 512) / 512;
            near_sample(e.pcm[f * 2u], 16384.0 * expected, 3.0, "curve",
                        (size_t) (block * BLOCK + f));
        }
    }
    close_engine(&e);
}

/* setTargetAtTime with a time constant far below one frame: the first
   frame is still v0 and the next ones are the target, never the NaN of an
   out-of-range per-frame rate (which clamped to the minimum, silence). */
static void test_tiny_time_constant(void)
{
    Engine e;
    open_engine(&e);
    uint32_t g = lane(&e, .8, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_SET, .8, 0, 0);
    event(&e, g, TILEFINCH_AUDIO_EVENT_TARGET, .5, 0, 1e-45);
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = g;
    start(&e, dc_buffer(&e, 1.0f, 1024), 0, 0, &p, true);
    mix(&e);
    near_sample(e.pcm[0], 32767.0 * .8, 2.0, "tiny target start", 0);
    for (size_t f = 1; f < BLOCK; f++)
        near_sample(e.pcm[f * 2u], 32768.0 * .5, 2.0, "tiny target", f);
    double value = 0;
    CHECK(tilefinch_game_audio_lane_value(e.audio, g, 256 / RATE, &value));
    CHECK(fabs(value - .5) < 1e-6);
    close_engine(&e);
}

static void test_cancel_and_hold_while_playing(void)
{
    Engine e;
    open_engine(&e);
    uint32_t g = lane(&e, 0, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_SET, 0, 0, 0);
    event(&e, g, TILEFINCH_AUDIO_EVENT_LINEAR, 1, 4096 / RATE, 0);
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = g;
    start(&e, dc_buffer(&e, .5f, 1024), 0, 0, &p, true);
    mix(&e);
    /* Hold at the start of the third block, ahead of the mixer. */
    CHECK(tilefinch_game_audio_lane_cancel(e.audio, g, e.now, 1024 / RATE, true)
          == TILEFINCH_AUDIO_TIMELINE_OK);
    mix(&e);
    near_sample(e.pcm[100 * 2u], 16384.0 * (612 / 4096.0), 1.5, "before hold", 612);
    mix(&e);
    mix(&e);
    for (size_t f = 0; f < BLOCK; f++)
        near_sample(e.pcm[f * 2u], 16384.0 * .25, 1.5, "held", 1536 + f);
    /* cancelScheduledValues with no events left keeps the held value. */
    CHECK(tilefinch_game_audio_lane_cancel(e.audio, g, e.now, e.now, false)
          == TILEFINCH_AUDIO_TIMELINE_OK);
    mix(&e);
    near_sample(e.pcm[0], 16384.0 * .25, 1.5, "after cancel", 0);
    close_engine(&e);
}

static void test_delayed_start_and_stop(void)
{
    Engine e;
    open_engine(&e);
    e.now = 3.25; /* a context that has run for a while */
    TilefinchGameAudioProgram p = unity();
    uint32_t g = lane(&e, .5, 0, 4);
    p.gain_lanes[0] = g;
    uint32_t voice = start(&e, dc_buffer(&e, .5f, 256), 0, e.now + 300 / RATE, &p, true);
    tilefinch_game_audio_stop(e.audio, voice, 700 / RATE);
    mix(&e);
    for (size_t f = 0; f < BLOCK; f++)
        CHECK(e.pcm[f * 2u] == (f < 300 ? 0 : 8192));
    mix(&e);
    for (size_t f = 0; f < BLOCK; f++)
        CHECK(e.pcm[f * 2u] == (f < 700 - 512 ? 8192 : 0));
    uint32_t ended = 0;
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    close_engine(&e);
}

static void test_oscillator_pitch_lane(void)
{
    Engine e;
    open_engine(&e);
    uint32_t f = lane(&e, 440, 1, 20000);
    event(&e, f, TILEFINCH_AUDIO_EVENT_SET, 200, 0, 0);
    event(&e, f, TILEFINCH_AUDIO_EVENT_EXPONENTIAL, 4000, 4096 / RATE, 0);
    TilefinchGameAudioProgram p = unity();
    p.pitch = 440;
    p.pitch_lane = f;
    start(&e, 0, TILEFINCH_GAME_AUDIO_OSCILLATOR_SQUARE, 0, &p, false);
    unsigned crossings[10] = {0};
    for (size_t block = 0; block < 10; block++) {
        mix(&e);
        for (size_t i = 1; i < BLOCK; i++)
            crossings[block] += (e.pcm[i * 2u] > 0) != (e.pcm[i * 2u - 2u] > 0);
    }
    /* Square wave: two crossings per period. 200 Hz is ~4.6 per block,
       4 kHz (held after the ramp) ~93; the sweep rises monotonically
       without browser ticks. */
    CHECK(crossings[0] <= 8u);
    for (size_t block = 1; block < 8; block++) CHECK(crossings[block] >= crossings[block - 1]);
    CHECK(crossings[9] >= 90u && crossings[9] <= 95u);
    close_engine(&e);
}

static double block_rms(const int16_t *pcm)
{
    double sum = 0;
    for (size_t f = 0; f < BLOCK; f++) sum += (double) pcm[f * 2u] * pcm[f * 2u];
    return sqrt(sum / BLOCK);
}

static uint32_t noise_buffer(Engine *e)
{
    TilefinchGameAudioBufferInfo info;
    CHECK(tilefinch_game_audio_create_buffer(e->audio, 1, 44100, 44100, &info));
    TilefinchGameAudioStorage *storage = NULL;
    float *plane = tilefinch_game_audio_channel_data(e->audio, info.handle, 0, &storage);
    uint32_t state = 0x1234567u;
    for (uint32_t at = 0; at < info.frames; at++) {
        state = state * 1664525u + 1013904223u;
        plane[at] = (float) ((int32_t) state) / 2147483648.0f * .5f;
    }
    tilefinch_game_audio_storage_release(storage);
    return info.handle;
}

static double filtered_rms(uint8_t type, double frequency, double q, uint32_t lane_handle,
                           size_t blocks, double *last)
{
    Engine e;
    open_engine(&e);
    uint32_t noise = noise_buffer(&e);
    uint32_t frequency_lane = 0;
    if (lane_handle) {
        frequency_lane = lane(&e, frequency, 0, 22050);
        event(&e, frequency_lane, TILEFINCH_AUDIO_EVENT_SET, 100, 0, 0);
        event(&e, frequency_lane, TILEFINCH_AUDIO_EVENT_EXPONENTIAL, 15000,
              blocks * BLOCK / RATE, 0);
    }
    TilefinchGameAudioProgram p = unity();
    p.filter = type;
    p.filter_values[TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY] = frequency;
    p.filter_values[TILEFINCH_GAME_AUDIO_FILTER_Q] = q;
    p.filter_lanes[TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY] = frequency_lane;
    start(&e, noise, 0, 0, &p, true);
    double first = 0;
    for (size_t block = 0; block < blocks; block++) {
        mix(&e);
        if (block == 1) first = block_rms(e.pcm);
        *last = block_rms(e.pcm);
    }
    close_engine(&e);
    return first;
}

static void test_biquad_filters(void)
{
    double last = 0;
    double open = filtered_rms(TILEFINCH_GAME_AUDIO_FILTER_ALLPASS, 1000, 1, 0, 4, &last);
    double low = filtered_rms(TILEFINCH_GAME_AUDIO_FILTER_LOWPASS, 300, 0, 0, 4, &last);
    double high = filtered_rms(TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS, 12000, 0, 0, 4, &last);
    double band = filtered_rms(TILEFINCH_GAME_AUDIO_FILTER_BANDPASS, 1000, 4, 0, 4, &last);
    /* White noise: power is proportional to bandwidth. */
    CHECK(open > 4000.0);
    CHECK(low < open * .2);
    CHECK(high < open * .8 && high > open * .3);
    CHECK(band < open * .2);
    /* An automated cutoff opens up over the run, sub-block by sub-block. */
    double swept = filtered_rms(TILEFINCH_GAME_AUDIO_FILTER_LOWPASS, 350, 0, 1, 12, &last);
    CHECK(swept < open * .2 && last > open * .7);
    /* A lowpass passes DC; a highpass removes it. */
    for (int pass = 0; pass < 2; pass++) {
        Engine e;
        open_engine(&e);
        TilefinchGameAudioProgram p = unity();
        p.filter = pass ? TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS : TILEFINCH_GAME_AUDIO_FILTER_LOWPASS;
        p.filter_values[TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY] = 200;
        start(&e, dc_buffer(&e, .5f, 512), 0, 0, &p, true);
        for (int block = 0; block < 20; block++) mix(&e);
        near_sample(e.pcm[1000 % BLOCK * 2u], pass ? 0 : 16384, 40, "dc", 0);
        close_engine(&e);
    }
}

/* The Web Audio specification's biquad coefficients (Filters
   characteristics), in double precision, normalized by a0, at the
   computed frequency f0 (frequency * 2^(detune / 1200), clamped to
   [0, Nyquist]). Where its formulas divide by zero (f0 = 0, f0 = Nyquist,
   Q = 0, or an alpha that overflows) the coefficients are their limits,
   the transfer functions browsers use: 0, 1, A^2 or -1. */
static void reference_biquad(uint8_t type, double frequency, double detune, double q,
                             double gain, double out[5])
{
    const double nyquist = 22050.0;
    double f0 = frequency * pow(2.0, detune / 1200.0);
    if (f0 < 0) f0 = 0;
    if (f0 > nyquist) f0 = nyquist;
    double a = pow(10.0, gain / 40.0);
    double k = 0.0;
    bool edge = f0 == 0.0 || f0 == nyquist;
    bool low = f0 == 0.0;
    switch (type) {
    case TILEFINCH_GAME_AUDIO_FILTER_LOWPASS: if (edge) k = low ? 0 : 1; break;
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS: if (edge) k = low ? 1 : 0; break;
    case TILEFINCH_GAME_AUDIO_FILTER_BANDPASS: if (edge) k = 0; else if (q <= 0) k = 1; break;
    case TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF: if (edge) k = low ? 1 : a * a; break;
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF: if (edge) k = low ? a * a : 1; break;
    case TILEFINCH_GAME_AUDIO_FILTER_PEAKING: if (edge) k = 1; else if (q <= 0) k = a * a; break;
    case TILEFINCH_GAME_AUDIO_FILTER_NOTCH: if (edge) k = 1; else if (q <= 0) k = 0; break;
    case TILEFINCH_GAME_AUDIO_FILTER_ALLPASS: if (edge) k = 1; else if (q <= 0) k = -1; break;
    default: edge = true; k = 1; break;
    }
    bool linear_q = type == TILEFINCH_GAME_AUDIO_FILTER_BANDPASS
        || type == TILEFINCH_GAME_AUDIO_FILTER_PEAKING
        || type == TILEFINCH_GAME_AUDIO_FILTER_NOTCH
        || type == TILEFINCH_GAME_AUDIO_FILTER_ALLPASS;
    if (edge || (linear_q && q <= 0)) {
        out[0] = k; out[1] = out[2] = out[3] = out[4] = 0;
        return;
    }
    double w0 = 2.0 * 3.14159265358979323846 * f0 / 44100.0;
    double cw = cos(w0), sw = sin(w0);
    double alpha_q = sw / (2.0 * q);
    double alpha_db = sw / (2.0 * pow(10.0, q / 20.0));
    double alpha_s = sw / 2.0 * sqrt(2.0);
    bool shelf = type == TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF
        || type == TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF;
    if (!shelf && (linear_q ? alpha_q : alpha_db) > 3.4e38) {
        /* Beyond single precision: the alpha -> infinity limit. */
        out[0] = type == TILEFINCH_GAME_AUDIO_FILTER_BANDPASS ? 1
            : type == TILEFINCH_GAME_AUDIO_FILTER_PEAKING ? a * a
            : type == TILEFINCH_GAME_AUDIO_FILTER_ALLPASS ? -1 : 0;
        out[1] = out[2] = out[3] = out[4] = 0;
        return;
    }
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case TILEFINCH_GAME_AUDIO_FILTER_LOWPASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0;
        a0 = 1 + alpha_db; a1 = -2 * cw; a2 = 1 - alpha_db; break;
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0;
        a0 = 1 + alpha_db; a1 = -2 * cw; a2 = 1 - alpha_db; break;
    case TILEFINCH_GAME_AUDIO_FILTER_BANDPASS:
        b0 = alpha_q; b1 = 0; b2 = -alpha_q;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q; break;
    case TILEFINCH_GAME_AUDIO_FILTER_NOTCH:
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q; break;
    case TILEFINCH_GAME_AUDIO_FILTER_ALLPASS:
        b0 = 1 - alpha_q; b1 = -2 * cw; b2 = 1 + alpha_q;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q; break;
    case TILEFINCH_GAME_AUDIO_FILTER_PEAKING:
        b0 = 1 + alpha_q * a; b1 = -2 * cw; b2 = 1 - alpha_q * a;
        a0 = 1 + alpha_q / a; a1 = -2 * cw; a2 = 1 - alpha_q / a; break;
    case TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF: {
        double root = 2 * sqrt(a) * alpha_s;
        b0 = a * ((a + 1) - (a - 1) * cw + root);
        b1 = 2 * a * ((a - 1) - (a + 1) * cw);
        b2 = a * ((a + 1) - (a - 1) * cw - root);
        a0 = (a + 1) + (a - 1) * cw + root;
        a1 = -2 * ((a - 1) + (a + 1) * cw);
        a2 = (a + 1) + (a - 1) * cw - root;
        break;
    }
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF: {
        double root = 2 * sqrt(a) * alpha_s;
        b0 = a * ((a + 1) + (a - 1) * cw + root);
        b1 = -2 * a * ((a - 1) + (a + 1) * cw);
        b2 = a * ((a + 1) + (a - 1) * cw - root);
        a0 = (a + 1) - (a - 1) * cw + root;
        a1 = 2 * ((a - 1) - (a + 1) * cw);
        a2 = (a + 1) - (a - 1) * cw - root;
        break;
    }
    default: break;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
    out[3] = a1 / a0; out[4] = a2 / a0;
}

static unsigned coefficient_mismatches(uint8_t type, double frequency, double detune, double q,
                                       double gain, const char *what)
{
    double want[5];
    float got[5];
    reference_biquad(type, frequency, detune, q, gain, want);
    tilefinch_game_audio_filter_coefficients(type, (float) frequency, (float) detune,
                                              (float) q, (float) gain, got);
    for (size_t at = 0; at < 5u; at++) {
        double tolerance = 3e-4 * (fabs(want[at]) > 1 ? fabs(want[at]) : 1);
        if (fabs(got[at] - want[at]) > tolerance) {
            fprintf(stderr, "%s: type %u f %g detune %g Q %g gain %g: k%zu %g, spec %g\n",
                    what, type, frequency, detune, q, gain, at, got[at], want[at]);
            return 1u;
        }
    }
    return 0u;
}

static void test_biquad_coefficients_and_limits(void)
{
    unsigned bad = 0;
    static const double frequencies[] = {10, 50, 350, 1000, 5000, 15000, 22000, 22049};
    static const double qs[] = {-12, 0.0001, 0.7071, 1, 10, 30};
    static const double gains[] = {-40, -6, 0, 6, 24};
    for (uint8_t type = TILEFINCH_GAME_AUDIO_FILTER_LOWPASS;
         type <= TILEFINCH_GAME_AUDIO_FILTER_ALLPASS; type++)
        for (size_t f = 0; f < sizeof(frequencies) / sizeof(frequencies[0]); f++)
            for (size_t q = 0; q < sizeof(qs) / sizeof(qs[0]); q++)
                for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++)
                    bad += coefficient_mismatches(type, frequencies[f], 0, qs[q], gains[g],
                                                  "interior");
    /* The endpoints: 0 Hz, the Nyquist frequency (also when detune
       carries the computed frequency past it), Q 0 or below for the types
       whose Q is linear, and an alpha too large for single precision. */
    for (uint8_t type = TILEFINCH_GAME_AUDIO_FILTER_LOWPASS;
         type <= TILEFINCH_GAME_AUDIO_FILTER_ALLPASS; type++) {
        for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
            bad += coefficient_mismatches(type, 0, 0, 1, gains[g], "0 Hz");
            bad += coefficient_mismatches(type, 0, 1200, 1, gains[g], "0 Hz detuned");
            bad += coefficient_mismatches(type, 22050, 0, 1, gains[g], "Nyquist");
            bad += coefficient_mismatches(type, 20000, 1200, 1, gains[g], "past Nyquist");
            bad += coefficient_mismatches(type, 1000, 0, 0, gains[g], "Q 0");
            bad += coefficient_mismatches(type, 1000, 0, -5, gains[g], "Q below 0");
            bad += coefficient_mismatches(type, 1000, 0, -1000, gains[g], "Q -1000");
            bad += coefficient_mismatches(type, 1000, 0, 1e-38, gains[g], "Q tiny");
        }
    }
    /* Between 0 and 10 Hz a single-precision biquad cannot place its
       cutoff (its coefficients round to those of other filters), so the
       mixer filters at 10 Hz there, as Game Profile v1.1 says. */
    for (uint8_t type = TILEFINCH_GAME_AUDIO_FILTER_LOWPASS;
         type <= TILEFINCH_GAME_AUDIO_FILTER_ALLPASS; type++) {
        float floor[5], below[5];
        tilefinch_game_audio_filter_coefficients(type, 10, 0, 1, 6, floor);
        tilefinch_game_audio_filter_coefficients(type, 0.5f, 0, 1, 6, below);
        if (memcmp(floor, below, sizeof(floor)) != 0) {
            fprintf(stderr, "type %u: 0.5 Hz does not filter as 10 Hz\n", type);
            bad++;
        }
    }
    if (bad) fprintf(stderr, "%u coefficient sets differ from the specification\n", bad);
    CHECK(bad == 0u);
    /* Rendered: a low-pass at 0 Hz passes nothing (as the specification's
       limit; it was a 10 Hz low-pass that let DC through), a high-pass at
       0 Hz everything; at the Nyquist frequency the reverse. */
    for (int which = 0; which < 4; which++) {
        Engine e;
        open_engine(&e);
        TilefinchGameAudioProgram p = unity();
        p.filter = which < 2 ? TILEFINCH_GAME_AUDIO_FILTER_LOWPASS
                             : TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS;
        p.filter_values[TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY] = which % 2 ? 22050 : 0;
        start(&e, dc_buffer(&e, .5f, 512), 0, 0, &p, true);
        for (int block = 0; block < 4; block++) mix(&e);
        bool passes = which == 1 || which == 2;
        for (size_t f = 0; f < BLOCK; f++)
            near_sample(e.pcm[f * 2u], passes ? 16384 : 0, passes ? 1 : 0, "filter endpoint",
                        f);
        close_engine(&e);
    }
}

static void test_mixes_with_integer_voices(void)
{
    /* An integer-path oscillator plus a program voice equal their sum. */
    int16_t alone[2][BLOCK * 2u];
    for (int which = 0; which < 3; which++) {
        Engine e;
        open_engine(&e);
        uint32_t voice = 0;
        if (which != 1)
            CHECK(tilefinch_game_audio_start_oscillator(
                e.audio, TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE, 330, .25, .125, 0, &voice));
        if (which != 0) {
            uint32_t g = lane(&e, .5, 0, 4);
            TilefinchGameAudioProgram p = unity();
            p.gain_lanes[0] = g;
            start(&e, dc_buffer(&e, .25f, 256), 0, 0, &p, true);
        }
        mix(&e);
        if (which < 2) memcpy(alone[which], e.pcm, sizeof(e.pcm));
        else
            for (size_t at = 0; at < BLOCK * 2u; at++)
                CHECK(e.pcm[at] == alone[0][at] + alone[1][at]);
        close_engine(&e);
    }
}

static void test_switch_to_program_path(void)
{
    Engine e;
    open_engine(&e);
    TilefinchGameAudioProgram p = unity();
    p.pitch = 440;
    uint32_t voice = start(&e, 0, TILEFINCH_GAME_AUDIO_OSCILLATOR_SQUARE, 0, &p, false);
    mix(&e);
    CHECK(abs(e.pcm[0]) > 30000);
    uint32_t g = lane(&e, 1, 0, 4);
    event(&e, g, TILEFINCH_AUDIO_EVENT_SET, 1, e.now, 0);
    event(&e, g, TILEFINCH_AUDIO_EVENT_LINEAR, 0, e.now + 512 / RATE, 0);
    p.gain_lanes[0] = g;
    CHECK(tilefinch_game_audio_set_program(e.audio, voice, &p));
    mix(&e);
    CHECK(abs(e.pcm[0]) > 30000 && abs(e.pcm[256 * 2u]) < 17000
          && abs(e.pcm[256 * 2u]) > 15000);
    mix(&e);
    for (size_t f = 0; f < BLOCK; f++) CHECK(e.pcm[f * 2u] == 0);
    close_engine(&e);
}

static void test_lane_lifetimes(void)
{
    Engine e;
    open_engine(&e);
    uint32_t lanes[TILEFINCH_GAME_AUDIO_LANE_LIMIT];
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_LANE_LIMIT; at++)
        lanes[at] = lane(&e, 1, 0, 4);
    uint32_t spare = 0;
    CHECK(!tilefinch_game_audio_lane_create(e.audio, 1, 0, 4, &spare));
    TilefinchGameAudioProgram p = unity();
    p.gain_lanes[0] = lanes[3];
    uint32_t voice = start(&e, dc_buffer(&e, .5f, 64), 0, 0, &p, true);
    mix(&e);
    /* Released while a voice reads it: the slot stays taken. */
    tilefinch_game_audio_lane_release(e.audio, lanes[3]);
    CHECK(tilefinch_game_audio_lane_insert(e.audio, lanes[3], e.now,
              TILEFINCH_AUDIO_EVENT_SET, 0, e.now, 0, NULL, 0)
          == TILEFINCH_AUDIO_TIMELINE_INVALID_STATE);
    CHECK(!tilefinch_game_audio_lane_create(e.audio, 1, 0, 4, &spare));
    mix(&e);
    CHECK(e.pcm[0] == 16384);
    tilefinch_game_audio_stop(e.audio, voice, 0);
    mix(&e);
    uint32_t ended = 0;
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    CHECK(tilefinch_game_audio_lane_create(e.audio, 1, 0, 4, &spare));
    CHECK(spare != lanes[3]);
    /* A released lane is not reused by a program. */
    p.gain_lanes[0] = lanes[3];
    TilefinchGameAudioVoiceStart s = {.oscillator = 1, .now = e.now, .when = e.now};
    p.pitch = 440;
    CHECK(!tilefinch_game_audio_start_program(e.audio, &s, &p, &voice));
    close_engine(&e);
}

static void test_buffers_and_pool(void)
{
    Engine e;
    open_engine(&e);
    TilefinchGameAudioBufferInfo info;
    /* 4 bytes per sample in the 512 KiB pool. */
    CHECK(!tilefinch_game_audio_create_buffer(e.audio, 1, 131073, 44100, &info));
    CHECK(!tilefinch_game_audio_create_buffer(e.audio, 3, 16, 44100, &info));
    CHECK(!tilefinch_game_audio_create_buffer(e.audio, 1, 16, 7999, &info));
    CHECK(tilefinch_game_audio_create_buffer(e.audio, 2, 65536, 22050, &info));
    CHECK(tilefinch_game_audio_pcm_bytes(e.audio) == 65536u * 8u);
    TilefinchGameAudioBufferInfo more;
    CHECK(!tilefinch_game_audio_create_buffer(e.audio, 1, 1, 44100, &more));
    tilefinch_game_audio_buffer_release(e.audio, info.handle);
    CHECK(tilefinch_game_audio_pcm_bytes(e.audio) == 0u);
    /* A view outlives its buffer's slot. */
    CHECK(tilefinch_game_audio_create_buffer(e.audio, 2, 4, 48000, &info));
    TilefinchGameAudioStorage *storage = NULL;
    float *right = tilefinch_game_audio_channel_data(e.audio, info.handle, 1, &storage);
    CHECK(right != NULL && tilefinch_game_audio_channel_data(
        e.audio, info.handle, 2, &storage) == NULL);
    right[3] = .75f;
    float copy[8] = {0};
    CHECK(tilefinch_game_audio_copy_from_channel(e.audio, info.handle, 1, 2, copy, 8));
    CHECK(copy[0] == 0 && copy[1] == .75f && copy[2] == 0);
    tilefinch_game_audio_buffer_release(e.audio, info.handle);
    right[0] = 1; /* still owned by the view */
    tilefinch_game_audio_storage_release(storage);
    /* A decoded WAV is promoted to float when its samples are read. */
    unsigned char wav[52] = "RIFF\x2c\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xac\0\0"
                            "\x88\x58\x01\0\x02\0\x10\0data\x08\0\0\0";
    int16_t samples[] = {16384, -16384, 8192, 0};
    memcpy(wav + 44, samples, sizeof(samples));
    CHECK(tilefinch_game_audio_decode_wav(e.audio, wav, sizeof(wav), &info));
    CHECK(tilefinch_game_audio_pcm_bytes(e.audio) == 8u);
    CHECK(tilefinch_game_audio_copy_from_channel(e.audio, info.handle, 0, 0, copy, 4));
    CHECK(copy[0] == .5f && copy[1] == -.5f && copy[2] == .25f);
    float *mono = tilefinch_game_audio_channel_data(e.audio, info.handle, 0, &storage);
    CHECK(mono != NULL && mono[1] == -.5f);
    CHECK(tilefinch_game_audio_pcm_bytes(e.audio) == 16u);
    /* Writes are heard: the promoted buffer plays its float plane. */
    mono[0] = mono[1] = mono[2] = mono[3] = .125f;
    TilefinchGameAudioProgram p = unity();
    start(&e, info.handle, 0, 0, &p, true);
    mix(&e);
    CHECK(e.pcm[0] == 4096 && e.pcm[BLOCK] == 4096);
    mono[2] = -.125f;
    mix(&e);
    CHECK(e.pcm[2 * 2u] == -4096);
    tilefinch_game_audio_storage_release(storage);
    close_engine(&e);
}

static void test_stereo_and_resampled_buffers(void)
{
    Engine e;
    open_engine(&e);
    TilefinchGameAudioBufferInfo info;
    CHECK(tilefinch_game_audio_create_buffer(e.audio, 2, 64, 22050, &info));
    TilefinchGameAudioStorage *a = NULL, *b = NULL;
    float *left = tilefinch_game_audio_channel_data(e.audio, info.handle, 0, &a);
    float *right = tilefinch_game_audio_channel_data(e.audio, info.handle, 1, &b);
    for (int at = 0; at < 64; at++) { left[at] = at / 64.0f; right[at] = -.25f; }
    tilefinch_game_audio_storage_release(a);
    tilefinch_game_audio_storage_release(b);
    TilefinchGameAudioProgram p = unity();
    p.gain_left = .5;
    uint32_t voice = start(&e, info.handle, 0, 0, &p, false);
    mix(&e);
    /* 22.05 kHz at 44.1 kHz: each source frame twice; ends after 128. */
    CHECK(e.pcm[0] == 0 && e.pcm[2 * 2u] == (int) (32768 * (1 / 64.0) * .5));
    CHECK(e.pcm[1] == -8192 && e.pcm[127 * 2u + 1u] == -8192 && e.pcm[128 * 2u + 1u] == 0);
    uint32_t ended = 0;
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    close_engine(&e);
}

/* Optional 16-bit stereo WAV of a validation render, for listening. */
static void write_wav(const char *directory, const char *name, const int16_t *pcm,
                      size_t frames)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.wav", directory, name);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    uint32_t data = (uint32_t) (frames * 4u);
    unsigned char header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0,
        4, 0, 16, 0, 'd', 'a', 't', 'a'};
    uint32_t riff = data + 36u;
    for (unsigned i = 0; i < 4u; i++) {
        header[4 + i] = (unsigned char) (riff >> (8u * i));
        header[40 + i] = (unsigned char) (data >> (8u * i));
    }
    CHECK(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    for (size_t at = 0; at < frames * 2u; at++) {
        unsigned char bytes[2] = {(unsigned char) (pcm[at] & 0xff),
                                  (unsigned char) ((uint16_t) pcm[at] >> 8)};
        CHECK(fwrite(bytes, 1, 2, file) == 2u);
    }
    CHECK(fclose(file) == 0);
}

/* The PSP validation scenarios ("audio-program-probe"): envelope shapes,
   and the envelope helper agrees with the full render. Set
   TILEFINCH_GAME_AUDIO_WAV_DIR to write each render as a WAV. */
static void test_validation_scenarios(void)
{
    enum { WINDOWS = 12, FRAMES = WINDOWS * 4 * BLOCK };
    static int16_t pcm[FRAMES * 2u];
    static const char *const names[TILEFINCH_GAME_AUDIO_SCENARIOS] = {
        "beep", "noise", "sweep", "four"};
    const char *directory = getenv("TILEFINCH_GAME_AUDIO_WAV_DIR");
    for (unsigned scenario = 0; scenario < TILEFINCH_GAME_AUDIO_SCENARIOS; scenario++) {
        Budget budget;
        budget_init(&budget, 512u * 1024u);
        CHECK(tilefinch_game_audio_validation_scenario(&budget, scenario, pcm, FRAMES));
        CHECK(budget.current == 0u);
        uint32_t rms[WINDOWS];
        CHECK(tilefinch_game_audio_validation_scenario_envelope(&budget, scenario, rms,
                                                                WINDOWS));
        CHECK(budget.current == 0u);
        printf("scenario %s rms=", names[scenario]);
        for (unsigned w = 0; w < WINDOWS; w++) {
            double sum = 0;
            for (size_t f = w * 4u * BLOCK; f < (w + 1u) * 4u * BLOCK; f++)
                sum += (double) pcm[f * 2u] * pcm[f * 2u];
            CHECK((uint32_t) (sqrt(sum / (4.0 * BLOCK)) + .5) == rms[w]);
            printf("%s%u", w ? "," : "", (unsigned) rms[w]);
        }
        printf("\n");
        if (directory != NULL) write_wav(directory, names[scenario], pcm, FRAMES);
        switch (scenario) {
        case TILEFINCH_GAME_AUDIO_SCENARIO_BEEP:
            /* Windows are 2048 frames (46.4 ms). Peak in the first, decaying,
               silent once stopped at 350 ms. */
            CHECK(rms[0] > 5000u && rms[1] < rms[0] && rms[3] < rms[1] && rms[5] < rms[3]);
            CHECK(rms[8] == 0u && rms[11] == 0u);
            break;
        case TILEFINCH_GAME_AUDIO_SCENARIO_NOISE:
            /* Uniform noise at 0.8 is about 15000 RMS, then a 50 ms release. */
            CHECK(rms[0] > 9000u && rms[1] < rms[0] && rms[3] * 4u < rms[1]);
            CHECK(rms[6] == 0u);
            break;
        case TILEFINCH_GAME_AUDIO_SCENARIO_SWEEP:
            /* Fully open, a 0.5-gain sawtooth is about 9460 RMS. */
            CHECK(rms[11] > 8000u && rms[11] < 11000u && rms[0] != rms[11]);
            break;
        default:
            /* Four voices ramping up together over 500 ms. */
            CHECK(rms[0] < rms[4] && rms[4] < rms[9] && rms[9] > 3000u);
            break;
        }
    }
}


static uint32_t analyser(Engine *e)
{
    uint32_t handle = 0;
    CHECK(tilefinch_game_audio_analyser_create(e->audio, &handle));
    CHECK(handle != 0u);
    return handle;
}

/* The newest COUNT analyser samples; the last BLOCK must all be VALUE. */
static void expect_history(Engine *e, uint32_t handle, size_t count, double value,
                           double tolerance, const char *what)
{
    static float history[TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX];
    uint32_t stamp = 0;
    CHECK(tilefinch_game_audio_analyser_read(e->audio, handle, history, count, &stamp));
    for (size_t at = count - BLOCK; at < count; at++)
        if (fabs(history[at] - value) > tolerance) {
            fprintf(stderr, "%s sample %zu: %f, expected %f\n", what, at, history[at], value);
            exit(1);
        }
}

static void test_analyser_taps(void)
{
    /* The analyser hears the signal where it sits on the chain: after the
       static and automated gains before it, before the ones after it. */
    for (int at_case = 0; at_case < 5; at_case++) {
        Engine e;
        open_engine(&e);
        uint32_t a = analyser(&e);
        TilefinchGameAudioProgram p = unity();
        p.analyser = a;
        p.tap_gain = 1;
        double heard = .5, played = 16384;
        if (at_case == 1) { p.tap_gain = .5; p.gain_left = p.gain_right = .25; heard = .25; played = 4096; }
        if (at_case == 2) { p.muted = true; played = 0; }
        if (at_case >= 3) {
            uint32_t g = lane(&e, .5, 0, 4);
            event(&e, g, TILEFINCH_AUDIO_EVENT_SET, .5, 0, 0);
            p.gain_lanes[0] = g;
            p.tap_lanes = at_case == 3 ? 1u : 0u;
            heard = at_case == 3 ? .25 : .5;
            played = 8192;
        }
        start(&e, dc_buffer(&e, .5f, 4096), 0, 0, &p, true);
        mix(&e);
        expect_history(&e, a, 512, heard, 1e-6, "tap");
        near_sample(e.pcm[100], played, 1.5, "tap output", 50);
        /* The history keeps the newest samples, older ones first. */
        static float older[2048];
        uint32_t stamp = 0;
        CHECK(tilefinch_game_audio_analyser_read(e.audio, a, older, 2048, &stamp));
        CHECK(older[0] == 0.0f && older[1535] == 0.0f);
        CHECK(fabsf(older[1536] - (float) heard) < 1e-6f);
        close_engine(&e);
    }
    /* Before or after a highpass that removes DC. */
    for (int after = 0; after < 2; after++) {
        Engine e;
        open_engine(&e);
        uint32_t a = analyser(&e);
        TilefinchGameAudioProgram p = unity();
        p.filter = TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS;
        p.filter_values[TILEFINCH_GAME_AUDIO_FILTER_FREQUENCY] = 200;
        p.analyser = a;
        p.tap_gain = 1;
        p.tap_after_filter = after != 0;
        start(&e, dc_buffer(&e, .5f, 512), 0, 0, &p, true);
        for (int block = 0; block < 20; block++) mix(&e);
        expect_history(&e, a, 512, after ? 0 : .5, after ? .005 : 1e-6, "filter tap");
        near_sample(e.pcm[200], 0, 40, "highpassed", 100);
        close_engine(&e);
    }
}

static void test_analyser_lifetimes(void)
{
    Engine e;
    open_engine(&e);
    uint32_t a = analyser(&e), b = analyser(&e), c = 0;
    CHECK(!tilefinch_game_audio_analyser_create(e.audio, &c));
    float out[2048];
    uint32_t stamp = 0;
    CHECK(!tilefinch_game_audio_analyser_read(e.audio, a, out, 4096, &stamp));
    /* A stereo source is heard as the mean of its channels. */
    TilefinchGameAudioBufferInfo info;
    CHECK(tilefinch_game_audio_create_buffer(e.audio, 2, 512, 44100, &info));
    TilefinchGameAudioStorage *l = NULL, *r = NULL;
    float *left = tilefinch_game_audio_channel_data(e.audio, info.handle, 0, &l);
    float *right = tilefinch_game_audio_channel_data(e.audio, info.handle, 1, &r);
    for (int at = 0; at < 512; at++) { left[at] = .5f; right[at] = -.25f; }
    tilefinch_game_audio_storage_release(l);
    tilefinch_game_audio_storage_release(r);
    TilefinchGameAudioProgram p = unity();
    p.analyser = b;
    p.tap_gain = 1;
    uint32_t voice = start(&e, info.handle, 0, 0, &p, true);
    mix(&e);
    expect_history(&e, b, 512, .125, 1e-6, "stereo tap");
    /* Released while a voice feeds it: the slot stays taken. */
    tilefinch_game_audio_analyser_release(e.audio, b);
    CHECK(!tilefinch_game_audio_analyser_read(e.audio, b, out, 512, &stamp));
    CHECK(!tilefinch_game_audio_analyser_create(e.audio, &c));
    mix(&e);
    tilefinch_game_audio_stop(e.audio, voice, 0);
    mix(&e);
    uint32_t ended = 0;
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    CHECK(tilefinch_game_audio_analyser_create(e.audio, &c) && c != b);
    /* No voice feeds it: it reads silence, its history gone. */
    p.analyser = a;
    voice = start(&e, dc_buffer(&e, .5f, 64), 0, 0, &p, true);
    mix(&e);
    expect_history(&e, a, 512, .5, 1e-6, "fed");
    tilefinch_game_audio_stop(e.audio, voice, 0);
    mix(&e);
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    expect_history(&e, a, 2048, 0, 0, "unfed");
    /* A program cannot name a released analyser. */
    tilefinch_game_audio_analyser_release(e.audio, a);
    TilefinchGameAudioVoiceStart s = {.oscillator = 1, .now = e.now, .when = e.now};
    p.pitch = 440;
    CHECK(!tilefinch_game_audio_start_program(e.audio, &s, &p, &voice));
    close_engine(&e);
}

static void test_spectrum(void)
{
    /* Against a double-precision DFT of the Blackman-windowed input. */
    enum { N = 2048 };
    static float time[N], spectrum[N / 2], scratch[2 * N];
    static double reference[N / 2];
    for (size_t i = 0; i < N; i++)
        time[i] = (float) (.8 * sin(2 * M_PI * 100.5 * i / N) + .1 * cos(2 * M_PI * 17 * i / N));
    double peak = 0;
    for (size_t k = 0; k < N / 2; k++) {
        double re = 0, im = 0;
        for (size_t i = 0; i < N; i++) {
            double x = 2 * M_PI * i / N;
            double w = .42 - .5 * cos(x) + .08 * cos(2 * x);
            re += time[i] * w * cos(x * k);
            im -= time[i] * w * sin(x * k);
        }
        reference[k] = sqrt(re * re + im * im) / N;
        if (reference[k] > peak) peak = reference[k];
    }
    memset(spectrum, 0, sizeof(spectrum));
    tilefinch_game_audio_spectrum(time, N, 0, spectrum, scratch);
    for (size_t k = 0; k < N / 2; k++) CHECK(fabs(spectrum[k] - reference[k]) < peak * 1e-4);
    CHECK(fabs(spectrum[17] - .1 * .42 / 2) < 1e-4);
    /* Smoothing over time: silence halves a 0.5-smoothed spectrum. */
    float held = spectrum[100];
    memset(time, 0, sizeof(time));
    tilefinch_game_audio_spectrum(time, N, .5f, spectrum, scratch);
    CHECK(fabsf(spectrum[100] - held * .5f) < 1e-7f);
    /* Small sizes work; a size outside the range leaves the spectrum. */
    for (size_t i = 0; i < 32; i++) time[i] = (float) sin(2 * M_PI * 4 * i / 32);
    tilefinch_game_audio_spectrum(time, 32, 0, spectrum, scratch);
    CHECK(spectrum[4] > .2f && spectrum[4] < .22f && spectrum[10] < 1e-5f);
    float kept = spectrum[4];
    tilefinch_game_audio_spectrum(time, 4096, 0, spectrum, scratch);
    tilefinch_game_audio_spectrum(time, 48, 0, spectrum, scratch);
    CHECK(spectrum[4] == kept);
}

static void stream_fill(Engine *e, float value, size_t count)
{
    static float samples[TILEFINCH_GAME_AUDIO_STREAM_SAMPLES];
    for (size_t at = 0; at < count; at++) samples[at] = value;
    tilefinch_game_audio_stream_write(e->audio, samples, count);
}

static void test_stream_voices(void)
{
    Engine e;
    open_engine(&e);
    /* No stream voice: writes go nowhere. */
    stream_fill(&e, .25f, 1024);
    uint32_t a = analyser(&e);
    TilefinchGameAudioProgram p = unity();
    p.analyser = a;
    p.tap_gain = 1;
    uint32_t voice = 0;
    CHECK(tilefinch_game_audio_start_stream(e.audio, e.now, &p, &voice));
    /* Only what arrives after the start plays, after a prefill. */
    stream_fill(&e, .25f, 512);
    mix(&e);
    expect_history(&e, a, 512, 0, 0, "prefill");
    stream_fill(&e, .25f, 1024);
    mix(&e);
    expect_history(&e, a, 512, .25, 1e-4, "stream");
    /* Inaudible until the output is private (headphones). */
    for (size_t f = 0; f < BLOCK * 2u; f++) CHECK(e.pcm[f] == 0);
    tilefinch_game_audio_stream_audible(e.audio, true);
    mix(&e);
    near_sample(e.pcm[0], 8192, 1, "audible stream", 0);
    /* Underrun: what is left plays, then silence until a prefill. */
    mix(&e);
    near_sample(e.pcm[1022], 8192, 1, "drained", 511);
    mix(&e);
    near_sample(e.pcm[0], 0, 0, "underrun", 0);
    stream_fill(&e, .5f, 600);
    mix(&e);
    near_sample(e.pcm[0], 0, 0, "reprime", 0);
    stream_fill(&e, .5f, 600);
    mix(&e);
    near_sample(e.pcm[0], 16384, 1, "reprimed", 0);
    /* A voice too far behind skips to the newest samples. */
    stream_fill(&e, .125f, 4096);
    stream_fill(&e, -.5f, 4096);
    mix(&e);
    near_sample(e.pcm[0], -16384, 1, "caught up", 0);
    near_sample(e.pcm[1022], -16384, 1, "caught up", 511);
    /* Muted (not reaching the destination), the analyser still hears it. */
    p.muted = true;
    CHECK(tilefinch_game_audio_set_program(e.audio, voice, &p));
    mix(&e);
    for (size_t f = 0; f < BLOCK * 2u; f++) CHECK(e.pcm[f] == 0);
    expect_history(&e, a, 512, -.5, 1e-4, "muted stream");
    /* A stream voice ends only when stopped; then its ring may be cleared. */
    CHECK(!tilefinch_game_audio_stream_reset(e.audio));
    tilefinch_game_audio_stop(e.audio, voice, 0);
    mix(&e);
    uint32_t ended = 0;
    CHECK(tilefinch_game_audio_take_completion(e.audio, &ended) && ended == voice);
    CHECK(tilefinch_game_audio_stream_reset(e.audio));
    CHECK(tilefinch_game_audio_start_stream(e.audio, e.now, &p, &voice));
    stream_fill(&e, .5f, 1024);
    p.muted = false;
    CHECK(tilefinch_game_audio_set_program(e.audio, voice, &p));
    mix(&e);
    near_sample(e.pcm[0], 16384, 1, "restarted", 0);
    close_engine(&e);
}

int main(void)
{
    test_linear_ramp_per_sample();
    test_exponential_beep();
    test_target_and_curve();
    test_tiny_time_constant();
    test_cancel_and_hold_while_playing();
    test_delayed_start_and_stop();
    test_oscillator_pitch_lane();
    test_biquad_filters();
    test_biquad_coefficients_and_limits();
    test_mixes_with_integer_voices();
    test_switch_to_program_path();
    test_lane_lifetimes();
    test_buffers_and_pool();
    test_stereo_and_resampled_buffers();
    test_validation_scenarios();
    test_analyser_taps();
    test_analyser_lifetimes();
    test_spectrum();
    test_stream_voices();
    puts("game audio programs: lanes, float buffers and filters render per sample");
    return 0;
}
