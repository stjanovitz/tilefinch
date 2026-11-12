/* A stand-in for the PSP presentation fence (AU-05). The loading
 * supervisor draws Library rows from another thread, and each row borrows
 * an item below the count the list was built with: its slot (id), title and
 * address. The probe compares those slots at every fence call and counts
 * the changes made while the fence was not held. Appends land beyond the
 * built rows and are not changes. Test-only.
 */
#ifndef TILEFINCH_TEST_OFFLINE_LIBRARY_FENCE_PROBE_H
#define TILEFINCH_TEST_OFFLINE_LIBRARY_FENCE_PROBE_H

#include "tilefinch/offline_library.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static struct {
    const OfflineLibrary *library;
    /* The rows of the last look: slots below this count, and their digest. */
    size_t rows;
    uint32_t seen;
    bool held;
    unsigned acquires;
    unsigned unfenced;
    /* Library work (a compile step) reached while the fence was held. */
    unsigned held_across_work;
} fence_probe;

static uint32_t fence_probe_mix(uint32_t hash, const void *data,
                                size_t length)
{
    const unsigned char *bytes = data;
    for (size_t at = 0; at < length; at++) {
        hash ^= bytes[at];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint32_t fence_probe_digest(const OfflineLibrary *library,
                                   size_t rows)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t at = 0; at < rows && at < OFFLINE_LIBRARY_ITEM_LIMIT; at++) {
        const OfflineLibraryItem *item = &library->items[at];
        hash = fence_probe_mix(hash, &item->id, sizeof(item->id));
        hash = fence_probe_mix(hash, item->title, strlen(item->title));
        hash = fence_probe_mix(hash, item->source_url,
                               strlen(item->source_url));
    }
    return hash;
}

/* The rows of the current list become the baseline. */
static void fence_probe_rebase(void)
{
    fence_probe.rows = fence_probe.library->count;
    fence_probe.seen = fence_probe_digest(fence_probe.library,
                                          fence_probe.rows);
}

/* A change to the last look's rows counts as unfenced unless the fence is
   held now; either way the current list becomes the baseline. */
static void fence_probe_observe(void)
{
    uint32_t now = fence_probe_digest(fence_probe.library, fence_probe.rows);
    if (!fence_probe.held && now != fence_probe.seen) fence_probe.unfenced++;
    fence_probe_rebase();
}

static bool fence_probe_acquire(void)
{
    fence_probe_observe();
    fence_probe.held = true;
    fence_probe.acquires++;
    return true;
}

static void fence_probe_release(bool held)
{
    if (!held || !fence_probe.held) fence_probe.unfenced++;
    fence_probe_rebase();
    fence_probe.held = false;
}

static void fence_probe_install(OfflineLibrary *library)
{
    memset(&fence_probe, 0, sizeof(fence_probe));
    fence_probe.library = library;
    fence_probe_rebase();
    library->fence_acquire = fence_probe_acquire;
    library->fence_release = fence_probe_release;
}

/* After an operation: every row-visible change since the last check was
   made under the fence, and the fence is not left held. */
static bool fence_probe_clean(void)
{
    fence_probe_observe();
    return fence_probe.unfenced == 0 && !fence_probe.held
        && fence_probe.held_across_work == 0;
}

#endif
