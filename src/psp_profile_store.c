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
    store->dirty = true;
}

bool psp_profile_store_flush(PspProfileStore *store)
{
    if (store == NULL || store->profile == NULL || store->path == NULL)
        return false;
    if (!store->dirty) return true;
    if (!browser_profile_save(store->profile, store->path)) return false;
    store->dirty = false;
    store->dirty_since_us = 0;
    return true;
}

bool psp_profile_store_flush_due(
    PspProfileStore *store, uint64_t now_us, uint64_t delay_us,
    bool *attempted)
{
    if (attempted != NULL) *attempted = false;
    if (store == NULL || !store->dirty
        || now_us < store->dirty_since_us
        || now_us - store->dirty_since_us < delay_us)
        return true;
    if (attempted != NULL) *attempted = true;
    bool saved = psp_profile_store_flush(store);
    if (!saved) store->dirty_since_us = now_us;
    return saved;
}
