#ifndef TILEFINCH_SWDEC_JOB_RING_H
#define TILEFINCH_SWDEC_JOB_RING_H

#include <stdatomic.h>
#include <stdbool.h>

/*
 * The hand-off between the browser thread, which prepares access units for
 * the software decoder, and the decode worker, which runs them.
 *
 * Jobs run strictly in the order they were published (the decoder is
 * stateful). With depth 2 the browser can publish the next unit while the
 * worker is still decoding the current one, so the worker starts it the
 * moment it finishes instead of idling until the browser loop next runs.
 *
 * Ownership: publish/collect run on the browser thread only; begin/finish on
 * the worker only. Each entry's state is the only shared word; the payload
 * an entry indexes is written before READY is released and read after it is
 * acquired, and the worker's results are released by DONE.
 */
#define SWDEC_JOB_RING_MAX 2u

enum {
    SWDEC_JOB_IDLE = 0,
    SWDEC_JOB_READY,
    SWDEC_JOB_RUNNING,
    SWDEC_JOB_DONE
};

/* swdec_job_ring_collect results other than an entry index. */
#define SWDEC_JOB_RING_EMPTY (-1)
#define SWDEC_JOB_RING_BUSY (-2)

typedef struct {
    atomic_int state[SWDEC_JOB_RING_MAX];
    unsigned depth;        /* 1 or 2, fixed after init */
    unsigned published;    /* browser thread */
    unsigned collected;    /* browser thread */
    unsigned started;      /* worker thread */
} SwdecJobRing;

static inline void swdec_job_ring_init(SwdecJobRing *ring, unsigned depth)
{
    ring->depth = depth == 0u ? 1u
        : depth > SWDEC_JOB_RING_MAX ? SWDEC_JOB_RING_MAX : depth;
    for (unsigned i = 0; i < SWDEC_JOB_RING_MAX; i++)
        atomic_init(&ring->state[i], SWDEC_JOB_IDLE);
    ring->published = 0;
    ring->collected = 0;
    ring->started = 0;
}

/* Browser: jobs published and not yet collected. */
static inline unsigned swdec_job_ring_in_flight(const SwdecJobRing *ring)
{
    return ring->published - ring->collected;
}

/* Browser: the entry the next job must be written into, or -1 when every
   entry is still owned by the worker or awaits collection. */
static inline int swdec_job_ring_reserve(const SwdecJobRing *ring)
{
    if (swdec_job_ring_in_flight(ring) >= ring->depth) return -1;
    unsigned index = ring->published % ring->depth;
    return atomic_load_explicit(&ring->state[index], memory_order_acquire)
            == SWDEC_JOB_IDLE ? (int) index : -1;
}

/* Browser: hand the reserved entry (its payload already written) over. */
static inline void swdec_job_ring_publish(SwdecJobRing *ring, int index)
{
    atomic_store_explicit(
        &ring->state[index], SWDEC_JOB_READY, memory_order_release);
    ring->published++;
}

/* Browser: withdraw the entry just published when the worker could not be
   woken. Only valid while it is still READY (the worker never saw it). */
static inline bool swdec_job_ring_unpublish(SwdecJobRing *ring, int index)
{
    int expected = SWDEC_JOB_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &ring->state[index], &expected, SWDEC_JOB_IDLE,
            memory_order_acq_rel, memory_order_acquire)) return false;
    ring->published--;
    return true;
}

/* Worker: claim the next job in publication order, or -1 if it is not
   published yet. */
static inline int swdec_job_ring_begin(SwdecJobRing *ring)
{
    unsigned index = ring->started % ring->depth;
    int expected = SWDEC_JOB_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &ring->state[index], &expected, SWDEC_JOB_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) return -1;
    ring->started++;
    return (int) index;
}

/* Worker: release the job's results to the browser. */
static inline void swdec_job_ring_finish(SwdecJobRing *ring, int index)
{
    atomic_store_explicit(
        &ring->state[index], SWDEC_JOB_DONE, memory_order_release);
}

/* Browser: the oldest finished job's entry (now IDLE again, its results
   readable until it is reserved), SWDEC_JOB_RING_BUSY when the oldest job is
   still queued or running, or SWDEC_JOB_RING_EMPTY when nothing is in
   flight. */
static inline int swdec_job_ring_collect(SwdecJobRing *ring)
{
    if (swdec_job_ring_in_flight(ring) == 0) return SWDEC_JOB_RING_EMPTY;
    unsigned index = ring->collected % ring->depth;
    if (atomic_load_explicit(&ring->state[index], memory_order_acquire)
        != SWDEC_JOB_DONE) return SWDEC_JOB_RING_BUSY;
    atomic_store_explicit(
        &ring->state[index], SWDEC_JOB_IDLE, memory_order_relaxed);
    ring->collected++;
    return (int) index;
}

#endif
