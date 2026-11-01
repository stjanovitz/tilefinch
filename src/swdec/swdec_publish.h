#ifndef TILEFINCH_SWDEC_PUBLISH_H
#define TILEFINCH_SWDEC_PUBLISH_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * A 64-bit value one thread publishes and another reads, on a 32-bit CPU
 * without a 64-bit atomic store (timestamps in microseconds outgrow 32 bits
 * after 71 minutes).
 *
 * The PSP has one core and no time slicing between priorities. A sequence
 * lock whose reader spins while a publication is half written deadlocks
 * when the reader outranks the writer: the audio thread (0x18) reading what
 * the browser thread (0x20) publishes would spin forever on a browser
 * preempted between the two halves. So the reader here never waits for a
 * writer. The value is double-buffered: a publication writes the slot
 * readers are not using and then flips the sequence, so a writer paused
 * anywhere inside a publication leaves the previous value whole and
 * readable. A read is retried only when a publication COMPLETED during it,
 * which needs the writer to make progress; after a bounded number of such
 * retries the reader returns the caller's fallback instead.
 *
 * One writer at a time. A second thread may publish only when it is ordered
 * after the first by other synchronisation (the seek fence hands the audio
 * cursor to the browser thread that way).
 */
#define SWDEC_PUBLISH_READ_ATTEMPTS 4u

typedef struct {
    atomic_uint sequence;               /* completed publications */
    atomic_uint low[2];
    atomic_uint high[2];
} SwdecPublishedU64;

static inline void swdec_published_init(SwdecPublishedU64 *published,
                                        uint64_t value)
{
    atomic_init(&published->sequence, 0u);
    for (unsigned i = 0; i < 2u; i++) {
        atomic_init(&published->low[i], (unsigned) value);
        atomic_init(&published->high[i], (unsigned) (value >> 32));
    }
}

/* Writer, step 1: the slot this publication fills. The fence orders the
   previous publication's sequence store before this slot's writes, so a
   reader that sees any of them also sees that the slot moved on. */
static inline unsigned swdec_published_begin(SwdecPublishedU64 *published)
{
    unsigned sequence = atomic_load_explicit(
        &published->sequence, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    return (sequence + 1u) & 1u;
}

/* Writer, step 2 (the two halves may be interrupted between). */
static inline void swdec_published_store(
    SwdecPublishedU64 *published, unsigned slot, uint64_t value)
{
    atomic_store_explicit(&published->low[slot & 1u], (unsigned) value,
                          memory_order_relaxed);
    atomic_store_explicit(&published->high[slot & 1u],
                          (unsigned) (value >> 32), memory_order_relaxed);
}

/* Writer, step 3: make the slot current. */
static inline void swdec_published_commit(SwdecPublishedU64 *published)
{
    atomic_fetch_add_explicit(&published->sequence, 1u, memory_order_release);
}

static inline void swdec_published_write(SwdecPublishedU64 *published,
                                         uint64_t value)
{
    unsigned slot = swdec_published_begin(published);
    swdec_published_store(published, slot, value);
    swdec_published_commit(published);
}

/* Reader: one attempt. False only when a publication completed meanwhile. */
static inline bool swdec_published_try_read(
    const SwdecPublishedU64 *published, uint64_t *value)
{
    unsigned before = atomic_load_explicit(
        &published->sequence, memory_order_acquire);
    unsigned slot = before & 1u;
    uint64_t low = atomic_load_explicit(
        &published->low[slot], memory_order_relaxed);
    uint64_t high = atomic_load_explicit(
        &published->high[slot], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    unsigned after = atomic_load_explicit(
        &published->sequence, memory_order_relaxed);
    if (before != after) return false;
    *value = (high << 32) | low;
    return true;
}

/* Reader: bounded; `fallback` (normally the caller's last good value) when
   the writer kept completing publications through every attempt. */
static inline uint64_t swdec_published_read(
    const SwdecPublishedU64 *published, uint64_t fallback)
{
    uint64_t value;
    for (unsigned attempt = 0; attempt < SWDEC_PUBLISH_READ_ATTEMPTS;
         attempt++) {
        if (swdec_published_try_read(published, &value)) return value;
    }
    return fallback;
}

#endif
