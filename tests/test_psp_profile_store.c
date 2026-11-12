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

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static void remove_generations(const char *path)
{
    char other[200];
    (void) remove(path);
    snprintf(other, sizeof(other), "%s.bak", path);
    (void) remove(other);
    snprintf(other, sizeof(other), "%s.tmp", path);
    (void) remove(other);
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-profile-store-%ld.cfg",
             (long) getpid());
    remove_generations(path);

    PspProfileStore store;
    psp_profile_store_init(&store, profile, path);
    browser_profile_set_ui_scale(profile, 2);
    psp_profile_store_mark_dirty(&store, 100);
    psp_profile_store_mark_dirty(&store, 500);
    bool attempted = true;
    CHECK(psp_profile_store_flush_due(&store, 1199, 700, false, &attempted));
    CHECK(!attempted && store.dirty);
    CHECK(psp_profile_store_flush_due(&store, 1200, 700, false, &attempted));
    CHECK(attempted && !store.dirty);

    BrowserProfile *loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL);
    CHECK(browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 2);

    /* An edit that changes nothing durable (a value stepped and stepped
       back, a history entry recorded with history off) does not touch the
       Memory Stick. The file is removed here so a write would show. */
    CHECK(remove(path) == 0);
    browser_profile_set_ui_scale(profile, 1);
    browser_profile_set_ui_scale(profile, 2);
    psp_profile_store_mark_dirty(&store, 2000);
    CHECK(psp_profile_store_flush_due(&store, 2700, 700, false, &attempted));
    CHECK(attempted && !store.dirty);
    CHECK(!file_exists(path));
    psp_profile_store_mark_dirty(&store, 3000);
    CHECK(psp_profile_store_flush(&store) && !store.dirty);
    CHECK(!file_exists(path));
    /* A real change is written. */
    browser_profile_set_ui_scale(profile, 1);
    psp_profile_store_mark_dirty(&store, 4000);
    CHECK(psp_profile_store_flush_due(&store, 4700, 700, false, &attempted));
    CHECK(attempted && !store.dirty && file_exists(path));
    browser_profile_destroy(loaded);
    loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL && browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 1);

    /* While the user acts or the page loads, the save waits, but no longer
       than the hold bound after the first unsaved edit. */
    uint64_t t = UINT64_C(10000000);
    browser_profile_set_ui_scale(profile, 2);
    psp_profile_store_mark_dirty(&store, t);
    CHECK(psp_profile_store_flush_due(&store, t + 800000u, 700000u, true,
                                      &attempted));
    CHECK(!attempted && store.dirty);
    psp_profile_store_mark_dirty(&store, t + 900000u);
    CHECK(psp_profile_store_flush_due(
        &store, t + PSP_PROFILE_STORE_MAXIMUM_HOLD_US - 1u, 700000u, true,
        &attempted));
    CHECK(!attempted && store.dirty);
    CHECK(psp_profile_store_flush_due(
        &store, t + PSP_PROFILE_STORE_MAXIMUM_HOLD_US, 700000u, true,
        &attempted));
    CHECK(attempted && !store.dirty);
    /* An idle frame saves at the debounce. */
    browser_profile_set_ui_scale(profile, 1);
    t += UINT64_C(20000000);
    psp_profile_store_mark_dirty(&store, t);
    CHECK(psp_profile_store_flush_due(&store, t + 700000u, 700000u, false,
                                      &attempted));
    CHECK(attempted && !store.dirty);

    /* A failing save (stick full, removed or write-protected) backs off:
       twice the debounce, doubling up to the retry bound, instead of
       writing until it fails every debounce period. */
    char unavailable[160];
    snprintf(unavailable, sizeof(unavailable), "%s/missing/profile.cfg",
             path);
    store.path = unavailable;
    browser_profile_set_ui_scale(profile, 2);
    t += UINT64_C(20000000);
    psp_profile_store_mark_dirty(&store, t);
    uint64_t now = t + 700000u;
    CHECK(!psp_profile_store_flush_due(&store, now, 700000u, false,
                                       &attempted));
    CHECK(attempted && store.dirty);
    uint64_t expected_delay = 1400000u;
    unsigned attempts = 1;
    for (unsigned step = 0; step < 12u; step++) {
        CHECK(psp_profile_store_flush_due(
            &store, now + expected_delay - 1u, 700000u, false, &attempted));
        CHECK(!attempted && store.dirty);
        now += expected_delay;
        CHECK(!psp_profile_store_flush_due(&store, now, 700000u, false,
                                           &attempted));
        CHECK(attempted && store.dirty);
        attempts++;
        expected_delay *= 2u;
        if (expected_delay > PSP_PROFILE_STORE_MAXIMUM_RETRY_US)
            expected_delay = PSP_PROFILE_STORE_MAXIMUM_RETRY_US;
    }
    /* Thirteen attempts in seven and a half minutes, where retrying every
       debounce period made about six hundred. */
    CHECK(attempts == 13u && now - t > UINT64_C(440000000));
    /* A new edit does not restart retrying at the debounce. */
    psp_profile_store_mark_dirty(&store, now + 1000u);
    CHECK(psp_profile_store_flush_due(&store, now + 701000u, 700000u, false,
                                      &attempted));
    CHECK(!attempted && store.dirty);
    /* The stick is back: the scheduled retry saves and the backoff ends. */
    store.path = path;
    now += expected_delay;
    CHECK(psp_profile_store_flush_due(&store, now, 700000u, false,
                                      &attempted));
    CHECK(attempted && !store.dirty && store.retry_delay_us == 0);
    browser_profile_set_ui_scale(profile, 1);
    psp_profile_store_mark_dirty(&store, now + 1u);
    CHECK(psp_profile_store_flush_due(&store, now + 700001u, 700000u, false,
                                      &attempted));
    CHECK(attempted && !store.dirty);

    browser_profile_destroy(loaded);
    loaded = browser_profile_create(&budget);
    CHECK(loaded != NULL);
    CHECK(browser_profile_load(loaded, path));
    CHECK(browser_profile_ui_scale(loaded) == 1);
    remove_generations(path);
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0);
    puts("psp-profile-store-tests: all checks passed");
    return 0;
}
