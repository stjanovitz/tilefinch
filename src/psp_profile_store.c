#include "tilefinch/psp_profile_store.h"

#include <string.h>

void psp_profile_store_init(
    PspProfileStore *store, BrowserProfile *profile, const char *path)
{
    if (store == NULL) return;
    memset(store, 0, sizeof(*store));
    store->profile = profile;
    store->path = path;
}

void psp_profile_store_mark_dirty(
    PspProfileStore *store, uint64_t now_us)
{
    if (store == NULL) return;
    /* Debounce from the latest edit. Rapid menu changes should become one
       Memory Stick transaction, not one write while the user is still
       stepping through values and another after they finish. */
    store->dirty_since_us = now_us;
    if (!store->dirty) store->first_dirty_us = now_us;
    store->dirty = true;
}

static void profile_store_clean(PspProfileStore *store)
{
    store->dirty = false;
    store->dirty_since_us = 0;
    store->first_dirty_us = 0;
    store->retry_at_us = 0;
    store->retry_delay_us = 0;
}

bool psp_profile_store_flush(PspProfileStore *store)
{
    if (store == NULL || store->profile == NULL || store->path == NULL)
        return false;
    if (!store->dirty) return true;
    /* Many edits change nothing durable (a value stepped and stepped back,
       a menu reopened). Formatting in memory is far cheaper than a Memory
       Stick create, write and three renames. */
    BrowserProfileFingerprint current;
    if (store->saved_known
        && browser_profile_fingerprint(store->profile, &current)
        && current.hash == store->saved.hash
        && current.length == store->saved.length) {
        profile_store_clean(store);
        return true;
    }
    BrowserProfileFingerprint written;
    if (!browser_profile_save_fingerprinted(
            store->profile, store->path, &written)) return false;
    store->saved = written;
    store->saved_known = true;
    profile_store_clean(store);
    return true;
}

bool psp_profile_store_flush_due(
    PspProfileStore *store, uint64_t now_us, uint64_t delay_us, bool busy,
    bool *attempted)
{
    if (attempted != NULL) *attempted = false;
    if (store == NULL || !store->dirty
        || now_us < store->dirty_since_us
        || now_us - store->dirty_since_us < delay_us
        || now_us < store->retry_at_us)
        return true;
    if (busy && now_us >= store->first_dirty_us
        && now_us - store->first_dirty_us
               < PSP_PROFILE_STORE_MAXIMUM_HOLD_US)
        return true;
    if (attempted != NULL) *attempted = true;
    if (psp_profile_store_flush(store)) return true;
    /* A full, removed or write-protected stick fails every attempt; each
       one writes until it fails. Back off instead of retrying every
       debounce period for the rest of the session. */
    uint64_t base = delay_us > UINT64_MAX / 2u ? UINT64_MAX : delay_us * 2u;
    uint64_t next = store->retry_delay_us == 0 ? base
        : store->retry_delay_us > UINT64_MAX / 2u ? UINT64_MAX
        : store->retry_delay_us * 2u;
    if (next > PSP_PROFILE_STORE_MAXIMUM_RETRY_US)
        next = PSP_PROFILE_STORE_MAXIMUM_RETRY_US;
    if (next == 0) next = 1u;
    store->retry_delay_us = next;
    store->retry_at_us = now_us > UINT64_MAX - next ? UINT64_MAX
                                                    : now_us + next;
    return false;
}
