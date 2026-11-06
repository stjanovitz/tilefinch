#ifndef TILEFINCH_MEDIA_BACKEND_PSP_ALLOCATION_H
#define TILEFINCH_MEDIA_BACKEND_PSP_ALLOCATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Order is the stable failure-mask contract, not allocation order. Audio
   sub-buffers are separate so an apparently successful video allocation
   cannot conceal the actual refusal. */
enum {
    PSP_MEDIA_ALLOC_MPEG, PSP_MEDIA_ALLOC_DDR,
    PSP_MEDIA_ALLOC_SURFACE0, PSP_MEDIA_ALLOC_SURFACE1,
    PSP_MEDIA_ALLOC_PACKET, PSP_MEDIA_ALLOC_AU, PSP_MEDIA_ALLOC_SETS,
    PSP_MEDIA_ALLOC_AUDIO_CODEC, PSP_MEDIA_ALLOC_PCM, PSP_MEDIA_ALLOC_QUEUE,
    PSP_MEDIA_ALLOC_AUDIO_STAGING, PSP_MEDIA_ALLOC_AUDIO_PENDING,
    PSP_MEDIA_ALLOC_COUNT
};
#define PSP_MEDIA_ALLOC_POOL_ABSENT 1u
#define PSP_MEDIA_ALLOC_POOL_CLAIMED 2u
#define PSP_MEDIA_ALLOC_POOL_POISONED 4u
#define PSP_MEDIA_ALLOC_POOL_HIGH 8u

typedef struct {
    uint32_t missing_mask;
    size_t request_bytes, request_alignment;
    int allocation_errno;
    size_t heap_lower_bound, heap_arena, heap_free, heap_top;
    size_t pool_bytes, pool_used, budget_remaining;
    unsigned pool_flags, width, height;
    uint32_t devkit;
    int system_free, system_largest;
} PspMediaAllocationFailure;

static inline unsigned psp_media_allocation_missing(
    bool video, bool audio, const void *const buffers[PSP_MEDIA_ALLOC_COUNT])
{
    unsigned missing = 0;
    for (unsigned i = 0; i < PSP_MEDIA_ALLOC_COUNT; i++) {
        bool required = i == PSP_MEDIA_ALLOC_PACKET
            || (i < PSP_MEDIA_ALLOC_AUDIO_CODEC ? video : audio);
        if (required && buffers[i] == NULL) missing |= 1u << i;
    }
    return missing;
}

static inline bool psp_media_allocation_format(
    const PspMediaAllocationFailure *failure, char *output, size_t size)
{
    if (output == NULL || size == 0) return false;
    output[0] = '\0';
    if (failure == NULL || failure->missing_mask == 0) return false;
    /* Fixed numeric fields only: never URLs, buffer contents, addresses,
       cookies, device/network names or user-controlled strings. */
    int written = snprintf(output, size,
        "media-allocation-v1 missing=0x%03x request=%zu alignment=%zu errno=%d\n"
        "media-allocation-heap lower-bound=%zu arena=%zu free=%zu top=%zu\n"
        "media-allocation-pool bytes=%zu used=%zu flags=0x%x\n"
        "media-allocation-context devkit=0x%08x size=%ux%u budget=%zu "
        "system-free=%d system-largest=%d\n",
        (unsigned) failure->missing_mask, failure->request_bytes,
        failure->request_alignment, failure->allocation_errno,
        failure->heap_lower_bound, failure->heap_arena,
        failure->heap_free, failure->heap_top,
        failure->pool_bytes, failure->pool_used, failure->pool_flags,
        (unsigned) failure->devkit, failure->width, failure->height,
        failure->budget_remaining, failure->system_free, failure->system_largest);
    if (written < 0 || (size_t) written >= size) {
        output[0] = '\0';
        return false;
    }
    return true;
}

#endif
