#ifndef TILEFINCH_NAVIGATION_STAGING_POLICY_H
#define TILEFINCH_NAVIGATION_STAGING_POLICY_H

#include <stddef.h>

/*
 * Capacity for a navigation's staged response body (bytes received but not
 * yet parsed) that must hold `needed` bytes. A low-memory replacement --
 * every embed hop to a player page -- stages the whole document before the
 * page being left is retired, so this buffer reaches the document's full
 * size while the old page is still resident.
 *
 * Small buffers double. From NAVIGATION_STAGING_LINEAR_BYTES on, the buffer
 * grows by a quarter: doubling a 1 MiB buffer for a 1.14 MiB document asked
 * the page budget for a whole extra MiB at its tightest moment, the request
 * failed, and the navigation ended as "stream consumer rejected response
 * body". The result is never below `needed` nor above `maximum` (0 when
 * `needed` exceeds `maximum`).
 */
#define NAVIGATION_STAGING_INITIAL_BYTES 4096u
#define NAVIGATION_STAGING_LINEAR_BYTES (256u * 1024u)

static inline size_t navigation_staging_capacity(
    size_t current, size_t needed, size_t maximum)
{
    if (needed > maximum) return 0;
    if (needed <= current) return current;
    size_t capacity = current < NAVIGATION_STAGING_INITIAL_BYTES
        ? NAVIGATION_STAGING_INITIAL_BYTES : current;
    while (capacity < needed) {
        size_t step = capacity < NAVIGATION_STAGING_LINEAR_BYTES
            ? capacity : capacity / 4u;
        if (capacity >= maximum || step > maximum - capacity) {
            capacity = maximum;
            break;
        }
        capacity += step;
    }
    if (capacity < needed) capacity = needed;
    return capacity > maximum ? maximum : capacity;
}

#endif
