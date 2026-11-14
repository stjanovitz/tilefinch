#ifndef TILEFINCH_GE_SCRATCH_H
#define TILEFINCH_GE_SCRATCH_H

/*
 * Graphics-engine scratch that is resident only while its path renders.
 *
 * The WebGL command list (64 KiB) and the GE canvas presenter's sprite
 * vertices (86 KiB) were static arrays, resident on every page. Each path
 * now holds a GeScratch instead:
 *
 * - Nothing is allocated before the path's first frame.
 * - ge_scratch_acquire() allocates the block on first use and afterwards
 *   returns the same block every frame without allocating.
 * - ge_scratch_release() frees it at the path's release point (its page is
 *   gone), and only once `busy` says the GE can no longer read it. A busy
 *   block stays held; the next release call tries again.
 *
 * The block comes from the C heap, outside the page Budget, as the static
 * array was: a page that renders pays the same memory it did, and the
 * Budget, script admission and the content ceiling see no change. A page
 * that never renders through the path gets the bytes back as heap headroom.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The PSP data cache line. Blocks are aligned to it and padded to a whole
   number of lines, so no other allocation shares a line with the block and a
   path may write it through the uncached alias. */
#define GE_SCRATCH_ALIGNMENT 64u

typedef struct {
    /* A GE_SCRATCH_ALIGNMENT-aligned block of `bytes` (a multiple of the
       alignment) with no dirty or valid CPU cache line over it, or NULL. */
    void *(*allocate)(void *context, size_t bytes);
    void (*release)(void *context, void *block, size_t bytes);
    /* Whether the GE may still read the block. NULL: the path's own fence
       has completed every submission before it returns. */
    bool (*busy)(void *context);
    void *context;
    /* A path asked every frame sets this: after a failed allocation it is
       refused without allocating until its next release point. */
    bool hold_failure;
} GeScratchOps;

typedef struct {
    void *block;
    size_t bytes;
    bool refused;
    uint32_t allocations, failures, releases, deferred_releases;
} GeScratch;

/* The C heap: memalign plus, on the PSP, a writeback-invalidate of the
   block's lines. Pair with ge_scratch_heap_release. */
void *ge_scratch_heap_allocate(void *context, size_t bytes);
void ge_scratch_heap_release(void *context, void *block, size_t bytes);

/* `bytes` is rounded up to whole cache lines. Returns NULL when the block
   cannot be allocated (or a held failure refuses it); the caller then
   refuses the frame as it does for any other allocation failure. A caller
   must not change `bytes` between release points. */
void *ge_scratch_acquire(GeScratch *scratch, const GeScratchOps *ops,
                         size_t bytes);
/* True when nothing is held afterwards. Clears a held failure. */
bool ge_scratch_release(GeScratch *scratch, const GeScratchOps *ops);
bool ge_scratch_resident(const GeScratch *scratch);

#endif
