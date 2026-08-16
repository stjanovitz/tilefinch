#include "tilefinch/psp_profile_store.h"

#include <stdio.h>
#include <unistd.h>

#include "tilefinch/budget.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-profile-store-%ld.cfg",
             (long) getpid());

    PspProfileStore store;
    psp_profile_store_init(&store, profile, path);
    browser_profile_set_ui_scale(profile, 2);
    psp_profile_store_mark_dirty(&store, 100);
    psp_profile_store_mark_dirty(&store, 500);
    bool attempted = true;
    CHECK(psp_profile_store_flush_due(&store, 1199, 700, &attempted));
    CHECK(!attempted && store.dirty);
    CHECK(psp_profile_store_flush_due(&store, 1200, 700, &attempted));
    CHECK(attempted && !store.dirty);

    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL);
    CHECK(browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 2);

    char unavailable[160];
    snprintf(unavailable, sizeof(unavailable), "%s/missing/profile.cfg",
             path);
    store.path = unavailable;
    browser_profile_set_ui_scale(profile, 1);
    psp_profile_store_mark_dirty(&store, 1000);
    CHECK(!psp_profile_store_flush_due(
        &store, 1700, 700, &attempted));
    CHECK(attempted && store.dirty && store.dirty_since_us == 1700);
    CHECK(psp_profile_store_flush_due(
        &store, 2399, 700, &attempted));
    CHECK(!attempted && store.dirty);
    store.path = path;
    CHECK(psp_profile_store_flush_due(
        &store, 2400, 700, &attempted));
    CHECK(attempted && !store.dirty);

    browser_profile_destroy(loaded);
    loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL);
    CHECK(browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 1);
    CHECK(remove(path) == 0);
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0);
    puts("psp-profile-store-tests: all checks passed");
    return 0;
}
