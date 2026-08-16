#ifndef TILEFINCH_CANCELLATION_H
#define TILEFINCH_CANCELLATION_H

#include <stdbool.h>
#include <stdatomic.h>

/*
 * An operation-scoped, cross-thread cancellation latch.
 *
 * Cancellation is monotonic for the lifetime of one operation: initialize
 * before publishing the operation, request from any thread, and destroy or
 * reinitialize only after every consumer has stopped using it.  This is the
 * native engine equivalent of an AbortSignal.  It deliberately carries no
 * callback list or allocation; adapters poll it at bounded work boundaries.
 */
typedef struct {
    atomic_bool requested;
} TilefinchCancellation;

static inline void tilefinch_cancellation_init(TilefinchCancellation *cancellation)
{
    if (cancellation == NULL) return;
    atomic_init(&cancellation->requested, false);
}

static inline void tilefinch_cancellation_request(
    TilefinchCancellation *cancellation)
{
    if (cancellation == NULL) return;
    atomic_store_explicit(
        &cancellation->requested, true, memory_order_release);
}

static inline bool tilefinch_cancellation_requested(
    const TilefinchCancellation *cancellation)
{
    return cancellation != NULL
        && atomic_load_explicit(
               &cancellation->requested, memory_order_acquire);
}

#endif
