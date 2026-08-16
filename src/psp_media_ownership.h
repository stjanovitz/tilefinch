#ifndef TILEFINCH_PSP_MEDIA_OWNERSHIP_H
#define TILEFINCH_PSP_MEDIA_OWNERSHIP_H

#include <stdatomic.h>
#include <stdbool.h>

/*
 * One-entry prepared codec-job mailbox.
 *
 * The browser publishes EMPTY -> READY. At active-job completion the worker
 * either claims READY or closes EMPTY before publishing DONE, preventing a
 * preparation from being stranded behind a worker which went back to sleep.
 * The collector reopens CLOSED after consuming DONE. Cancellation claims a
 * READY entry before discarding its separately-owned payload.
 *
 * Keep the atomics here rather than in the PSP-only backend so host tests can
 * exercise the exact release/acquire protocol under randomized interleavings.
 */
typedef enum {
    PSP_MEDIA_CODEC_PREPARED_EMPTY = 0,
    PSP_MEDIA_CODEC_PREPARED_READY = 1,
    PSP_MEDIA_CODEC_PREPARED_CLAIMED = 2,
    PSP_MEDIA_CODEC_PREPARED_CLOSED = 3
} PspMediaCodecPreparedState;

typedef enum {
    PSP_MEDIA_PREPARED_WORKER_UNAVAILABLE = 0,
    PSP_MEDIA_PREPARED_WORKER_CLOSED,
    PSP_MEDIA_PREPARED_WORKER_CLAIMED
} PspMediaPreparedWorkerResult;

static inline bool psp_media_prepared_try_publish(atomic_int *state)
{
    if (state == NULL) return false;
    int expected = PSP_MEDIA_CODEC_PREPARED_EMPTY;
    return atomic_compare_exchange_strong_explicit(
        state, &expected, PSP_MEDIA_CODEC_PREPARED_READY,
        memory_order_release, memory_order_acquire);
}

static inline PspMediaPreparedWorkerResult
psp_media_prepared_worker_take_or_close(
    atomic_int *state, bool completion_slot_empty)
{
    if (state == NULL) return PSP_MEDIA_PREPARED_WORKER_UNAVAILABLE;
    for (;;) {
        int observed = atomic_load_explicit(state, memory_order_acquire);
        if (observed == PSP_MEDIA_CODEC_PREPARED_EMPTY) {
            int expected = PSP_MEDIA_CODEC_PREPARED_EMPTY;
            if (!atomic_compare_exchange_weak_explicit(
                    state, &expected, PSP_MEDIA_CODEC_PREPARED_CLOSED,
                    memory_order_acq_rel, memory_order_acquire)) continue;
            return PSP_MEDIA_PREPARED_WORKER_CLOSED;
        }
        if (observed != PSP_MEDIA_CODEC_PREPARED_READY
            || !completion_slot_empty)
            return PSP_MEDIA_PREPARED_WORKER_UNAVAILABLE;
        int expected = PSP_MEDIA_CODEC_PREPARED_READY;
        if (!atomic_compare_exchange_weak_explicit(
                state, &expected, PSP_MEDIA_CODEC_PREPARED_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) continue;
        return PSP_MEDIA_PREPARED_WORKER_CLAIMED;
    }
}

static inline bool psp_media_prepared_try_cancel(atomic_int *state)
{
    if (state == NULL) return false;
    int expected = PSP_MEDIA_CODEC_PREPARED_READY;
    return atomic_compare_exchange_strong_explicit(
        state, &expected, PSP_MEDIA_CODEC_PREPARED_CLAIMED,
        memory_order_acq_rel, memory_order_acquire);
}

static inline bool psp_media_prepared_release_claimed(atomic_int *state)
{
    if (state == NULL) return false;
    int expected = PSP_MEDIA_CODEC_PREPARED_CLAIMED;
    return atomic_compare_exchange_strong_explicit(
        state, &expected, PSP_MEDIA_CODEC_PREPARED_EMPTY,
        memory_order_release, memory_order_acquire);
}

static inline bool psp_media_prepared_reopen_closed(atomic_int *state)
{
    if (state == NULL) return false;
    int expected = PSP_MEDIA_CODEC_PREPARED_CLOSED;
    return atomic_compare_exchange_strong_explicit(
        state, &expected, PSP_MEDIA_CODEC_PREPARED_EMPTY,
        memory_order_acq_rel, memory_order_acquire);
}

#endif
