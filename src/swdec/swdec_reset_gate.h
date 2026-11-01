#ifndef TILEFINCH_SWDEC_RESET_GATE_H
#define TILEFINCH_SWDEC_RESET_GATE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A seek's decoder reset, completed on later pumps instead of waiting on the
 * browser thread.
 *
 * The reset has to wait for the decode worker's in-flight units, for every
 * borrowed surface to come back, for the audio thread to stop reading the
 * PCM ring (swdec_audio_fence.h) and for the Media Engine to finish the
 * audio it was given. Waiting for those on the browser thread held input
 * for as long as a slow decode took. Instead the seek requests the reset and
 * returns; each later submit, drain or advance polls it and performs it once
 * everything is quiet. A seek while one is pending folds into it (one reset
 * serves both), and close tears the pending state down as it would a
 * playing one.
 *
 * Browser thread only.
 */
typedef enum {
    SWDEC_RESET_IDLE = 0,    /* nothing requested */
    SWDEC_RESET_WAIT,        /* requested; something is still in flight */
    SWDEC_RESET_READY,       /* everything quiet: perform it now */
    SWDEC_RESET_TIMEOUT      /* waited past the deadline: fail playback */
} SwdecResetVerdict;

typedef struct {
    bool pending;
    uint32_t requested_us;   /* wrapping microsecond clock */
    uint32_t deadline_us;    /* how long the inputs may stay busy */
    unsigned coalesced;      /* requests folded into the pending one */
} SwdecResetGate;

typedef struct {
    bool jobs_idle;          /* the decode worker holds no unit */
    bool surfaces_idle;      /* no decoded surface is borrowed */
    bool audio_parked;       /* the audio thread reads no PCM block */
    bool media_engine_idle;  /* every submitted audio unit is decoded */
} SwdecResetInputs;

static inline void swdec_reset_gate_init(SwdecResetGate *gate,
                                         uint32_t deadline_us)
{
    gate->pending = false;
    gate->requested_us = 0;
    gate->deadline_us = deadline_us;
    gate->coalesced = 0;
}

/* A seek: never waits. True when this request opened a new pending reset,
   false when it folded into one already pending. */
static inline bool swdec_reset_gate_request(SwdecResetGate *gate,
                                            uint32_t now_us)
{
    if (gate->pending) {
        gate->coalesced++;
        return false;
    }
    gate->pending = true;
    gate->requested_us = now_us;
    gate->coalesced = 0;
    return true;
}

static inline SwdecResetVerdict swdec_reset_gate_poll(
    const SwdecResetGate *gate, SwdecResetInputs inputs, uint32_t now_us)
{
    if (!gate->pending) return SWDEC_RESET_IDLE;
    if (inputs.jobs_idle && inputs.surfaces_idle && inputs.audio_parked
        && inputs.media_engine_idle) return SWDEC_RESET_READY;
    return (uint32_t) (now_us - gate->requested_us) >= gate->deadline_us
        ? SWDEC_RESET_TIMEOUT : SWDEC_RESET_WAIT;
}

/* The reset was performed (or abandoned with the backend). */
static inline void swdec_reset_gate_complete(SwdecResetGate *gate)
{
    gate->pending = false;
    gate->coalesced = 0;
}

#endif
