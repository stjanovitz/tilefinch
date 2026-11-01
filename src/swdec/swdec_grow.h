#ifndef TILEFINCH_SWDEC_GROW_H
#define TILEFINCH_SWDEC_GROW_H

#include <stdbool.h>
#include <stddef.h>

#include "swdec_bounds.h"
#include "tilefinch/budget.h"

/* Growing the software decoder's video buffers on the browser thread once a
 * unit arrives that the size planned at open cannot hold (a later fragment
 * of a fragmented MP4: SWDEC_SAMPLE_MAX_BYTES). Both buffers are plain
 * Budget blocks that nothing else reads while they grow: a job's packet is
 * grown only while the job is idle, and the video queue is the browser
 * thread's alone. A refused growth leaves the buffer, its contents and its
 * capacity exactly as they were, so the caller can fail cleanly and free it
 * as usual. */
typedef enum {
    SWDEC_GROW_FITS = 0,    /* already big enough, untouched */
    SWDEC_GROW_GROWN,       /* reallocated; contents kept */
    SWDEC_GROW_OVER_BOUND,  /* the unit is past what any buffer may hold */
    SWDEC_GROW_REFUSED      /* the Budget refused even the least size */
} SwdecGrowResult;

/* A job's decoder packet for a unit needing `needed` bytes: half again as
 * big as it is (swdec_packet_regrow), and when the Budget refuses that,
 * exactly `needed`. */
static inline SwdecGrowResult swdec_packet_grow(
    Budget *budget, unsigned char **packet, size_t *capacity,
    size_t needed, size_t ceiling)
{
    if (packet == NULL || capacity == NULL) return SWDEC_GROW_REFUSED;
    size_t next = swdec_packet_regrow(*capacity, needed, ceiling);
    if (next == 0) return SWDEC_GROW_OVER_BOUND;
    if (next == *capacity) return SWDEC_GROW_FITS;
    for (;;) {
        unsigned char *grown = budget_realloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, *packet, next);
        if (grown != NULL) {
            *packet = grown;
            *capacity = next;
            return SWDEC_GROW_GROWN;
        }
        if (next == needed) return SWDEC_GROW_REFUSED;
        next = needed;
    }
}

/* The video queue for a `length`-byte unit: planned as at open but for this
 * unit (swdec_au_queue_plan), smaller when refused, down to the least that
 * holds it. Queued units keep their offsets (the reallocation copies the
 * bytes as they lie) and the ring only gains free space past them. */
static inline SwdecGrowResult swdec_au_queue_grow(
    Budget *budget, unsigned char **bytes, size_t *capacity,
    size_t length, bool have_audio)
{
    if (bytes == NULL || capacity == NULL) return SWDEC_GROW_REFUSED;
    if (!swdec_au_queue_needs_growth(*capacity, length))
        return SWDEC_GROW_FITS;
    size_t want = 0, floor = 0;
    if (length > SWDEC_SAMPLE_MAX_BYTES
        || !swdec_au_queue_plan(length, have_audio, &want, &floor))
        return SWDEC_GROW_OVER_BOUND;
    for (; want != 0; want = swdec_au_queue_step_down(want, floor)) {
        unsigned char *grown = budget_realloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, *bytes, want);
        if (grown != NULL) {
            *bytes = grown;
            *capacity = want;
            return SWDEC_GROW_GROWN;
        }
    }
    return SWDEC_GROW_REFUSED;
}

#endif
