#ifndef TILEFINCH_BACKGROUND_SLOT_POLICY_H
#define TILEFINCH_BACKGROUND_SLOT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/fetch.h"

#define FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS 2u

static inline unsigned fetch_background_admission_slot_limit(
    bool foreground_media, bool media_priority_active)
{
    return media_priority_active && !foreground_media
        ? FETCH_BACKGROUND_REQUEST_LIMIT
              - FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS
        : FETCH_BACKGROUND_REQUEST_LIMIT;
}

static inline uint32_t fetch_background_generation_next(uint32_t current)
{
    uint32_t next = current + 1u;
    return next == 0 ? 1u : next;
}

static inline unsigned fetch_background_request_id_slot(uint64_t request_id)
{
    unsigned encoded = (unsigned) (request_id & UINT64_C(0xff));
    return encoded == 0 ? FETCH_BACKGROUND_REQUEST_LIMIT : encoded - 1u;
}

static inline uint32_t fetch_background_request_id_generation(
    uint64_t request_id)
{
    return (uint32_t) (request_id >> 8);
}

static inline uint64_t fetch_background_request_id_make(
    unsigned slot, uint32_t generation)
{
    return ((uint64_t) generation << 8) | (uint64_t) (slot + 1u);
}

#endif
