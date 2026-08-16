#ifndef TILEFINCH_PSP_PROFILE_STORE_H
#define TILEFINCH_PSP_PROFILE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/browser_profile.h"

typedef struct {
    BrowserProfile *profile;
    const char *path;
    uint64_t dirty_since_us;
    bool dirty;
} PspProfileStore;

void psp_profile_store_init(
    PspProfileStore *store, BrowserProfile *profile, const char *path);

void psp_profile_store_mark_dirty(
    PspProfileStore *store, uint64_t now_us);

bool psp_profile_store_flush(PspProfileStore *store);

bool psp_profile_store_flush_due(
    PspProfileStore *store, uint64_t now_us, uint64_t delay_us,
    bool *attempted);

#endif
