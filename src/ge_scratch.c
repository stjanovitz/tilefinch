#include "tilefinch/ge_scratch.h"

#include <stdlib.h>

#if defined(__PSP__)
#include <malloc.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspkernel.h>
#pragma GCC diagnostic pop
#endif

#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
#include "tilefinch/psp_log.h"
/* Validation only: one line per allocation or release, never per frame. */
#define GE_SCRATCH_LOG(event, bytes) psp_log_printf( \
    "tilefinch-ge-scratch: event=%s bytes=%u\n", (event), (unsigned) (bytes))
#else
#define GE_SCRATCH_LOG(event, bytes) ((void) 0)
#endif

static size_t ge_scratch_round(size_t bytes)
{
    if (bytes == 0u || bytes > SIZE_MAX - (GE_SCRATCH_ALIGNMENT - 1u))
        return 0u;
    return (bytes + GE_SCRATCH_ALIGNMENT - 1u)
        & ~(size_t) (GE_SCRATCH_ALIGNMENT - 1u);
}

void *ge_scratch_heap_allocate(void *context, size_t bytes)
{
    (void) context;
    size_t rounded = ge_scratch_round(bytes);
    if (rounded == 0u) return NULL;
#if defined(__PSP__)
    void *block = memalign(GE_SCRATCH_ALIGNMENT, rounded);
    /* The heap's previous owner may have left dirty lines over these bytes;
       written back later, they would land on top of what a path writes
       through the uncached alias. The block owns whole lines, so dropping
       them touches no neighbour. */
    if (block != NULL) sceKernelDcacheWritebackInvalidateRange(
        block, (unsigned) rounded);
    return block;
#else
    void *block = NULL;
    return posix_memalign(&block, GE_SCRATCH_ALIGNMENT, rounded) == 0
        ? block : NULL;
#endif
}

void ge_scratch_heap_release(void *context, void *block, size_t bytes)
{
    (void) context; (void) bytes;
    free(block);
}

void *ge_scratch_acquire(GeScratch *scratch, const GeScratchOps *ops,
                         size_t bytes)
{
    if (scratch == NULL || ops == NULL || ops->allocate == NULL) return NULL;
    if (scratch->block != NULL) return scratch->block;
    if (scratch->refused) return NULL;
    size_t rounded = ge_scratch_round(bytes);
    void *block = rounded == 0u ? NULL
        : ops->allocate(ops->context, rounded);
    if (block == NULL) {
        scratch->failures++;
        scratch->refused = ops->hold_failure;
        GE_SCRATCH_LOG("allocation-failed", rounded);
        return NULL;
    }
    scratch->block = block;
    scratch->bytes = rounded;
    scratch->allocations++;
    GE_SCRATCH_LOG("allocated", rounded);
    return block;
}

bool ge_scratch_release(GeScratch *scratch, const GeScratchOps *ops)
{
    if (scratch == NULL) return true;
    scratch->refused = false;
    if (scratch->block == NULL) return true;
    if (ops == NULL || ops->release == NULL) return false;
    if (ops->busy != NULL && ops->busy(ops->context)) {
        scratch->deferred_releases++;
        GE_SCRATCH_LOG("release-deferred", scratch->bytes);
        return false;
    }
    GE_SCRATCH_LOG("released", scratch->bytes);
    ops->release(ops->context, scratch->block, scratch->bytes);
    scratch->block = NULL;
    scratch->bytes = 0u;
    scratch->releases++;
    return true;
}

bool ge_scratch_resident(const GeScratch *scratch)
{
    return scratch != NULL && scratch->block != NULL;
}
