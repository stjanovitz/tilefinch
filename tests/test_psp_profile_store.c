#include "tilefinch/psp_profile_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/session.h"
#include "../src/tilefinch_test_faults.h"

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

static int test_failed_forget_preserves_policy(void)
{
    char directory[] = "/tmp/tilefinch-profile-forget-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    const char *origin = "https://storage.example.test/";
    Budget budget; budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session;
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL && browser_session_init(&session, &budget, 32768)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(&session, origin, BROWSER_SITE_STORAGE_STICK)
          && browser_profile_set_site_storage_policy(profile, origin, BROWSER_SITE_STORAGE_STICK)
          && browser_session_storage_set(&session, origin, true, "key", "value", 5));
    PspProfileStore store;
    psp_profile_store_init(&store, profile, NULL);
    tilefinch_test_faults()->fail_site_storage_remove_at = 1;
    CHECK(!psp_profile_store_forget_site_storage(&store, &session, origin, 123));
    CHECK(!store.dirty && browser_profile_site_storage_policy(profile, origin)
              == BROWSER_SITE_STORAGE_STICK);
    CHECK(psp_profile_store_forget_site_storage(&store, &session, origin, 456)
          && store.dirty && store.dirty_since_us == 456
          && browser_profile_site_storage_policy(profile, origin) == BROWSER_SITE_STORAGE_ASK);
    browser_session_destroy(&session);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0 && rmdir(directory) == 0);
    return 0;
}

static int test_history_delete_save_failure(void)
{
    char directory[] = "/tmp/tilefinch-history-delete-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[160];
    snprintf(path, sizeof(path), "%s/profile.cfg", directory);
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    BrowserProfile *profile = browser_profile_create(&budget);
    BrowserProfile *restart = browser_profile_create(&budget);
    const char *url = "https://history.example.test/";
    browser_profile_set_history_enabled(profile, true);
    CHECK(profile != NULL && restart != NULL
          && browser_profile_record_history(profile, url, "Visit")
          && browser_profile_save(profile, path));
    PspProfileStore store;
    psp_profile_store_init(&store, profile, path);
    CHECK(browser_profile_forget_history(profile, url));
    tilefinch_test_faults()->replace_file_fail_install = true;
    CHECK(!psp_profile_store_commit_edit(&store, 123)
          && store.dirty && browser_profile_history_count(profile) == 0
          && browser_profile_load(restart, path)
          && browser_profile_history_count(restart) == 1);
    bool attempted = false;
    CHECK(psp_profile_store_flush_due(&store, 223, 100, false, &attempted)
          && attempted && !store.dirty && browser_profile_load(restart, path)
          && browser_profile_history_count(restart) == 0);
    remove_generations(path);
    browser_profile_destroy(restart);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0 && rmdir(directory) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_failed_forget_preserves_policy() == 0);
    CHECK(test_history_delete_save_failure() == 0);
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
    browser_profile_destroy(loaded);
    /* A primary boot could not read blocks the store's saves until a
       retry finds it gone; the backup this run loaded from stays. */
    CHECK(browser_profile_save(profile, path) && chmod(path, 0) == 0);
    loaded = browser_profile_create(&budget);
    bool loaded_from_backup = loaded != NULL
        && browser_profile_load(loaded, path);
    PspProfileStore unread;
    psp_profile_store_init(&unread, loaded, path);
    browser_profile_set_ui_scale(loaded, 2);
    psp_profile_store_mark_dirty(&unread, 1u);
    bool refused = loaded_from_backup && !psp_profile_store_flush(&unread)
        && unread.dirty;
    CHECK(chmod(path, 0600) == 0);
    char backup[200];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    CHECK(refused && remove(path) == 0 && psp_profile_store_flush(&unread)
          && !unread.dirty && file_exists(path) && file_exists(backup));
    remove_generations(path);
    browser_profile_destroy(loaded);
    browser_profile_destroy(profile);
    CHECK(budget.current == 0);
    puts("psp-profile-store-tests: all checks passed");
    return 0;
}
