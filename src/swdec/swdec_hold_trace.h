#ifndef SWDEC_HOLD_TRACE_H
#define SWDEC_HOLD_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Per-picture records behind the steady window's longest holds (validation
 * builds). Every video unit gets a record when it is queued, keyed by its
 * presentation time; the browser thread fills in when it was dispatched,
 * decoded (start and end, from the worker's job), ready, taken and shown,
 * or how it was lost (a decoded unit with no ready time gave no picture).
 * When a hold is among the window's SWDEC_HOLD_KEEP
 * longest, the records of the pictures around the one that ended it are
 * copied out, so the report shows those pictures, not an average.
 *
 * Times are the low 32 bits of the system clock in microseconds; only
 * differences are read, which wrap correctly.
 */
#define SWDEC_HOLD_RING 128u
#define SWDEC_HOLD_KEEP 6u
#define SWDEC_HOLD_PICTURES 20u
/* The pictures copied around a hold: from this long before the picture
   that ended it to this long after. */
#define SWDEC_HOLD_BEFORE_US UINT64_C(500000)
#define SWDEC_HOLD_AFTER_US UINT64_C(130000)

typedef enum {
    SWDEC_HOLD_PENDING = 0,
    SWDEC_HOLD_SHOWN,
    SWDEC_HOLD_SUPERSEDED,   /* decoded, then a newer due picture won */
    SWDEC_HOLD_LATE,         /* dropped late before dispatch */
    SWDEC_HOLD_SKIPPED,      /* skipped to a keyframe */
    SWDEC_HOLD_FULL,         /* dropped with the queue full */
    SWDEC_HOLD_WORKER_LATE   /* dropped late by the worker */
} SwdecHoldFate;

typedef struct {
    uint64_t pts_us;
    uint32_t bytes;
    int32_t lead_us;         /* pts minus the clock when queued */
    uint32_t queued_us, dispatched_us, decode_start_us, decode_end_us;
    /* Same decode's worker/browser CPU; UINT32_MAX means unavailable.
       Sampling brackets bound their uncertainty, not time to subtract. */
    uint32_t decode_cpu_us, browser_cpu_us, clock_bracket_us;
    uint32_t ready_us, taken_us, shown_us;
    /* Last readiness poll that declined this picture, and the audio-led
       clocks at that poll and at the successful take. These distinguish
       decoder starvation from a due gate or a delayed presenter turn. */
    uint32_t declined_us;
    uint64_t declined_clock_us, taken_clock_us;
    /* The picture's output: when the decode that returned it ended, and
       that decode's unit pts relative to this picture's (ms; 0 when the
       picture is its own unit's output, positive after reordering). */
    uint32_t output_us;
    int16_t output_by_ms;
    uint8_t depth;           /* units already queued when this one was */
    int8_t slice;            /* first slice's type (0 P, 1 B, 2 I), -1 */
    uint8_t keyframe, reference, fate, used;
} SwdecHoldPic;

typedef struct {
    uint32_t hold_us;
    uint64_t pts_us;         /* the picture that ended the hold */
    uint32_t shown_us;
    uint32_t take_gap_us;    /* longest gap between presenter takes */
    uint32_t gap_wait_us;    /* explicit owner sleep within that same gap */
    uint32_t repeat_age_us;  /* last redraw of the old picture, or zero */
    uint32_t ready;          /* pictures ready when the hold ended */
    uint32_t queued;         /* units queued when the hold ended */
    unsigned count;
    SwdecHoldPic pictures[SWDEC_HOLD_PICTURES];
} SwdecHold;

typedef struct {
    SwdecHoldPic ring[SWDEC_HOLD_RING];
    unsigned next;
    SwdecHold holds[SWDEC_HOLD_KEEP];
} SwdecHoldTrace;

static inline SwdecHoldPic *swdec_hold_add(SwdecHoldTrace *trace,
                                           uint64_t pts_us)
{
    SwdecHoldPic *picture = &trace->ring[trace->next];
    trace->next = (trace->next + 1u) % SWDEC_HOLD_RING;
    *picture = (SwdecHoldPic) {.pts_us = pts_us, .slice = -1, .used = 1};
    return picture;
}

/* The newest record of the picture at pts_us, or NULL. */
static inline SwdecHoldPic *swdec_hold_find(SwdecHoldTrace *trace,
                                            uint64_t pts_us)
{
    for (unsigned i = 1; i <= SWDEC_HOLD_RING; i++) {
        SwdecHoldPic *picture = &trace->ring[
            (trace->next + SWDEC_HOLD_RING - i) % SWDEC_HOLD_RING];
        if (picture->used && picture->pts_us == pts_us) return picture;
    }
    return NULL;
}

/* A hold of hold_us ended by the picture at pts_us: kept when it is among
   the longest so far, with the records around it in presentation order. */
static inline void swdec_hold_capture(SwdecHoldTrace *trace,
                                      const SwdecHold *hold)
{
    unsigned at = SWDEC_HOLD_KEEP;
    for (unsigned i = 0; i < SWDEC_HOLD_KEEP; i++) {
        if (hold->hold_us > trace->holds[i].hold_us) { at = i; break; }
    }
    if (at == SWDEC_HOLD_KEEP) return;
    for (unsigned j = SWDEC_HOLD_KEEP - 1u; j > at; j--)
        trace->holds[j] = trace->holds[j - 1u];
    SwdecHold *kept = &trace->holds[at];
    *kept = *hold;
    kept->count = 0;
    uint64_t from = hold->pts_us > SWDEC_HOLD_BEFORE_US
        ? hold->pts_us - SWDEC_HOLD_BEFORE_US : 0;
    uint64_t to = hold->pts_us + SWDEC_HOLD_AFTER_US;
    for (unsigned i = 0; i < SWDEC_HOLD_RING; i++) {
        const SwdecHoldPic *picture = &trace->ring[i];
        if (!picture->used || picture->pts_us < from || picture->pts_us > to)
            continue;
        unsigned slot = kept->count;
        if (slot == SWDEC_HOLD_PICTURES) {
            /* Full: keep the pictures nearest the hold. */
            unsigned far = 0;
            uint64_t far_by = 0;
            for (unsigned k = 0; k < SWDEC_HOLD_PICTURES; k++) {
                uint64_t p = kept->pictures[k].pts_us;
                uint64_t by = p > hold->pts_us ? p - hold->pts_us
                                               : hold->pts_us - p;
                if (by >= far_by) { far_by = by; far = k; }
            }
            uint64_t by = picture->pts_us > hold->pts_us
                ? picture->pts_us - hold->pts_us
                : hold->pts_us - picture->pts_us;
            if (by >= far_by) continue;
            slot = far;
        } else {
            kept->count++;
        }
        kept->pictures[slot] = *picture;
    }
    for (unsigned i = 1; i < kept->count; i++) {
        SwdecHoldPic moving = kept->pictures[i];
        unsigned j = i;
        while (j > 0 && kept->pictures[j - 1u].pts_us > moving.pts_us) {
            kept->pictures[j] = kept->pictures[j - 1u];
            j--;
        }
        kept->pictures[j] = moving;
    }
}

/* One unsigned Exp-Golomb value from a NAL payload (emulation prevention
   bytes are not removed: only the first few bits of a slice are read). */
static inline bool swdec_hold_ue(const unsigned char *bytes, size_t length,
                                 size_t *bit, uint32_t *value)
{
    unsigned zeros = 0;
    for (;;) {
        if (*bit >= length * 8u || zeros > 31u) return false;
        unsigned b = (bytes[*bit / 8u] >> (7u - *bit % 8u)) & 1u;
        (*bit)++;
        if (b != 0) break;
        zeros++;
    }
    uint32_t suffix = 0;
    for (unsigned i = 0; i < zeros; i++) {
        if (*bit >= length * 8u) return false;
        suffix = (suffix << 1)
            | ((bytes[*bit / 8u] >> (7u - *bit % 8u)) & 1u);
        (*bit)++;
    }
    *value = (UINT32_C(1) << zeros) - 1u + suffix;
    return true;
}

/* slice_type (mod 5: 0 P, 1 B, 2 I, 3 SP, 4 SI) of the slice whose NAL
   payload, after its header byte, starts at slice; -1 when unreadable. */
static inline int swdec_hold_slice_type(const unsigned char *slice,
                                        size_t length)
{
    size_t bit = 0;
    uint32_t first_mb = 0, type = 0;
    if (!swdec_hold_ue(slice, length, &bit, &first_mb)
        || !swdec_hold_ue(slice, length, &bit, &type) || type > 9u)
        return -1;
    return (int) (type % 5u);
}

#endif
