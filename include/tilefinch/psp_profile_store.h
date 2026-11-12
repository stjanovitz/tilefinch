#ifndef TILEFINCH_PSP_PROFILE_STORE_H
#define TILEFINCH_PSP_PROFILE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/browser_profile.h"

/* A busy frame (input, loading, a pending render) holds a due save, but
   never longer than this after the first unsaved edit. */
#define PSP_PROFILE_STORE_MAXIMUM_HOLD_US UINT64_C(10000000)
/* A failed deferred save is retried after twice the debounce, doubling up
   to this. psp_profile_store_flush (suspend, exit, explicit saves) always
   tries at once. */
#define PSP_PROFILE_STORE_MAXIMUM_RETRY_US UINT64_C(60000000)

typedef struct {
    BrowserProfile *profile;
    const char *path;
    /* The latest edit, which the debounce runs from. */
    uint64_t dirty_since_us;
    /* The first edit not yet saved, which bounds how long busy frames may
       hold the save. */
    uint64_t first_dirty_us;
    /* What the file holds since this store's last successful save; a flush
       that would write the same bytes skips the Memory Stick. */
    BrowserProfileFingerprint saved;
    bool saved_known;
    /* After a failed deferred save: no retry before retry_at_us. */
    uint64_t retry_at_us;
    uint64_t retry_delay_us;
    bool dirty;
} PspProfileStore;

void psp_profile_store_init(
    PspProfileStore *store, BrowserProfile *profile, const char *path);

void psp_profile_store_mark_dirty(
    PspProfileStore *store, uint64_t now_us);

/* Saves a dirty profile now, unless it would write what the file already
   holds. */
bool psp_profile_store_flush(PspProfileStore *store);

/* Saves once `delay_us` passed since the latest edit, and, while `busy`,
   only once PSP_PROFILE_STORE_MAXIMUM_HOLD_US passed since the first
   unsaved edit. *attempted reports whether a save was tried. */
bool psp_profile_store_flush_due(
    PspProfileStore *store, uint64_t now_us, uint64_t delay_us, bool busy,
    bool *attempted);

#endif
