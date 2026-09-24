#include "tilefinch/session.h"
#include "tilefinch/session_persistence.h"
#include "../src/tilefinch_test_faults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "SITE STORAGE failed at %s:%d: %s\n",               \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

#define MIB (1024u * 1024u)

static const char *const game = "https://game.test/play";
static const char *const news = "https://news.test/";

static char directory[128];
static char value[600u * 1024u];

static bool file_exists(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "rb");
    if (file != NULL) fclose(file);
    return file != NULL;
}

static long file_size(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return size;
}

/* The always-kept file for game.test: p-<FNV-1a 64 of its origin>. */
static bool always_file(char *name, size_t capacity)
{
    unsigned long long hash = 14695981039346656037ull;
    for (const unsigned char *at = (const unsigned char *) "https://game.test";
         *at != '\0'; at++) {
        hash ^= *at;
        hash *= 1099511628211ull;
    }
    snprintf(name, capacity, "p-%016llx", hash);
    return file_exists(name);
}

static int test_per_site_allowances(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);   /* no room to grow */
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && session.site_store == NULL);
    /* Each site has its own allowance: the old 16 KiB pool shared by every
       origin would refuse the second site. */
    memset(value, 'a', 20u * 1024u);
    CHECK(browser_session_storage_set(&session, game, true, "k", value,
                                      20u * 1024u)
          && browser_session_storage_set(&session, news, true, "k", value,
                                         20u * 1024u)
          && session.site_store != NULL);
    /* Key and value both count, and sessionStorage shares the allowance. */
    CHECK(!browser_session_storage_set(
              &session, game, false, "s", value,
              BROWSER_SITE_STORAGE_INITIAL_BYTES - 20u * 1024u - 1u)
          && browser_session_storage_set(
                 &session, game, false, "s", value,
                 BROWSER_SITE_STORAGE_INITIAL_BYTES - 20u * 1024u - 2u));
    BrowserSiteStorageInfo info;
    CHECK(browser_session_site_storage_info_for(&session, game, &info)
          && info.tier == BROWSER_SITE_STORAGE_MEMORY
          && info.bytes == BROWSER_SITE_STORAGE_INITIAL_BYTES
          && info.limit_bytes == BROWSER_SITE_STORAGE_INITIAL_BYTES
          && info.item_count == 2);
    /* Unconfigured: no Memory Stick offer is made. */
    CHECK(!browser_session_site_storage_take_request(&session, NULL));
    /* Clearing everything returns the session to one pointer. */
    CHECK(browser_session_clear_site_data(&session, game)
          && browser_session_clear_site_data(&session, news)
          && session.site_store == NULL);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static int test_background_growth(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u));
    memset(value, 'g', 300u * 1024u);
    CHECK(browser_session_storage_set(&session, game, true, "save", value,
                                      300u * 1024u));
    BrowserSiteStorageInfo info;
    CHECK(browser_session_site_storage_info_for(&session, game, &info)
          && info.limit_bytes == 512u * 1024u);
    /* The per-site RAM ceiling holds even with memory to spare. */
    CHECK(!browser_session_storage_set(&session, game, true, "more", value,
                                       250u * 1024u));
    /* So does the total across sites. */
    CHECK(browser_session_storage_set(&session, news, true, "a", value,
                                      400u * 1024u)
          && !browser_session_storage_set(
                 &session, "https://third.test/", true, "a", value,
                 400u * 1024u));
    const char *read = NULL;
    size_t length = 0;
    CHECK(browser_session_storage_get(&session, game, true, "save", &read,
                                      &length)
          && length == 300u * 1024u && read[0] == 'g');
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static int test_offer_and_session_grant(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory));
    memset(value, 'v', 40u * 1024u);
    CHECK(browser_session_storage_set(&session, game, true, "a", value,
                                      20u * 1024u)
          && browser_session_opfs_create(&session, game, "/saves",
                                         BROWSER_OPFS_DIRECTORY)
                 == BROWSER_OPFS_OK
          && browser_session_opfs_create(&session, game, "/saves/slot",
                                         BROWSER_OPFS_FILE)
                 == BROWSER_OPFS_OK
          && browser_session_opfs_write(&session, game, "/saves/slot",
                                        (const unsigned char *) "state", 5)
                 == BROWSER_OPFS_OK
          && browser_session_storage_set(&session, game, false, "tab", "1",
                                         1));
    /* Out of RAM: the write fails and one offer carries the sizes the
       prompt shows. */
    CHECK(!browser_session_storage_set(&session, game, true, "b", value,
                                       40u * 1024u));
    BrowserSiteStorageRequest request;
    size_t current = 20u * 1024u + 1u + 6u + 11u + 5u + 3u + 1u;
    CHECK(browser_session_site_storage_take_request(&session, &request)
          && strcmp(request.origin, "https://game.test") == 0
          && request.current_bytes == current
          && request.needed_bytes == current + 40u * 1024u + 1u
          && request.stick_limit_bytes
                 == BROWSER_SITE_STORAGE_STICK_SITE_BYTES);
    /* Not offered again while the prompt is up. */
    CHECK(!browser_session_storage_set(&session, game, true, "b", value,
                                       40u * 1024u)
          && !browser_session_site_storage_take_request(&session, NULL));
    CHECK(browser_session_site_storage_grant(&session, game, false)
          && file_exists("s-00") && file_exists("session-files"));
    BrowserSiteStorageInfo info;
    CHECK(browser_session_site_storage_info_for(&session, game, &info)
          && info.tier == BROWSER_SITE_STORAGE_STICK_SESSION
          && info.limit_bytes == BROWSER_SITE_STORAGE_STICK_SITE_BYTES
          && info.bytes == current && info.file_bytes > current);
    /* Everything reads back from the Memory Stick, and the write that ran
       out now fits. */
    const char *read = NULL;
    size_t length = 0;
    BrowserOpfsView view;
    CHECK(browser_session_storage_get(&session, game, true, "a", &read,
                                      &length)
          && length == 20u * 1024u && read[0] == 'v' && read[length] == 0
          && browser_session_storage_get(&session, game, false, "tab", &read,
                                         &length)
          && length == 1 && read[0] == '1'
          && browser_session_opfs_stat(&session, game, "/saves/slot", &view)
                 == BROWSER_OPFS_OK
          && view.data == NULL && view.data_length == 5
          && browser_session_opfs_read(&session, game, "/saves/slot", &view)
                 == BROWSER_OPFS_OK
          && memcmp(view.data, "state", 5) == 0
          && browser_session_storage_set(&session, game, true, "b", value,
                                         40u * 1024u)
          && browser_session_storage_length(&session, game, true) == 2);
    /* A stick site takes OPFS files past the RAM per-file limit. */
    CHECK(browser_session_opfs_write(&session, game, "/saves/slot",
                                     (const unsigned char *) value,
                                     100u * 1024u) == BROWSER_OPFS_OK);
    /* The site's RAM is released: another site gets the whole pool. */
    CHECK(browser_session_storage_set(&session, news, true, "n", value,
                                      30u * 1024u));
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && !file_exists("s-00")
          && !file_exists("session-files"));
    return 0;
}

static int test_decline_and_policies(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory));
    memset(value, 'd', 40u * 1024u);
    CHECK(!browser_session_storage_set(&session, game, true, "a", value,
                                       40u * 1024u)
          && browser_session_site_storage_take_request(&session, NULL));
    browser_session_site_storage_decline(&session, game);
    CHECK(!browser_session_storage_set(&session, game, true, "a", value,
                                       40u * 1024u)
          && !browser_session_site_storage_take_request(&session, NULL));
    /* RAM only: never offered. */
    CHECK(browser_session_site_storage_set_policy(
              &session, news, BROWSER_SITE_STORAGE_MEMORY_ONLY)
          && !browser_session_storage_set(&session, news, true, "a", value,
                                          40u * 1024u)
          && !browser_session_site_storage_take_request(&session, NULL));
    /* Offers switched off globally. */
    browser_session_site_storage_set_offers(&session, false);
    CHECK(!browser_session_storage_set(&session, "https://third.test/",
                                       true, "a", value, 40u * 1024u)
          && !browser_session_site_storage_take_request(&session, NULL));
    browser_session_site_storage_set_offers(&session, true);
    /* A write too big even for the Memory Stick is not offered. */
    CHECK(!browser_session_storage_set(
              &session, "https://fourth.test/", true, "a", value,
              BROWSER_SITE_STORAGE_STICK_VALUE_BYTES + 1u)
          && !browser_session_site_storage_take_request(&session, NULL));
    /* Standing choices are listed for the Storage menu. */
    BrowserSiteStorageInfo info;
    CHECK(browser_session_site_storage_count(&session) == 1
          && browser_session_site_storage_info(&session, 0, &info)
          && strcmp(info.origin, "https://news.test") == 0
          && info.policy == BROWSER_SITE_STORAGE_MEMORY_ONLY);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static int test_always_survives_reboot(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory));
    CHECK(browser_session_storage_set(&session, game, true, "level", "7", 1)
          && browser_session_storage_set(&session, game, false, "tab", "x",
                                         1)
          && browser_session_opfs_create(&session, game, "/save",
                                         BROWSER_OPFS_FILE)
                 == BROWSER_OPFS_OK
          && browser_session_opfs_write(&session, game, "/save",
                                        (const unsigned char *) "abc", 3)
                 == BROWSER_OPFS_OK
          && browser_session_site_storage_grant(&session, game, true));
    char name[64];
    CHECK(always_file(name, sizeof(name)));
    /* Later writes and removals are logged. */
    CHECK(browser_session_storage_set(&session, game, true, "level", "8", 1)
          && browser_session_storage_set(&session, game, true, "gone", "1",
                                         1));
    browser_session_storage_remove(&session, game, true, "gone");
    /* "Clear session storage" keeps an always-kept site's files. */
    browser_session_opfs_clear_all(&session);
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && always_file(name, sizeof(name)));

    /* Next boot: registered from the profile, loaded on first use. */
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    BrowserSiteStorageInfo info;
    CHECK(browser_session_site_storage_info_for(&session, game, &info)
          && !info.loaded && info.file_bytes == (size_t) file_size(name));
    const char *read = NULL;
    size_t length = 0;
    BrowserOpfsView view;
    CHECK(browser_session_storage_get(&session, game, true, "level", &read,
                                      &length)
          && length == 1 && read[0] == '8'
          && !browser_session_storage_get(&session, game, true, "gone", NULL,
                                          NULL)
          && !browser_session_storage_get(&session, game, false, "tab", NULL,
                                          NULL)
          && browser_session_opfs_read(&session, game, "/save", &view)
                 == BROWSER_OPFS_OK
          && view.data_length == 3 && memcmp(view.data, "abc", 3) == 0);

    /* Switching back to Ask keeps the data until exit, then deletes it. */
    CHECK(browser_session_site_storage_set_policy(
              &session, game, BROWSER_SITE_STORAGE_ASK)
          && !always_file(name, sizeof(name))
          && browser_session_storage_get(&session, game, true, "level",
                                         &read, &length));
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && !file_exists("s-00")
          && !always_file(name, sizeof(name)));
    return 0;
}

static int test_forget_and_clear(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_storage_set(&session, game, true, "k", "v", 1)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    char name[64];
    CHECK(always_file(name, sizeof(name))
          && browser_session_site_storage_tier(&session, game)
                 == BROWSER_SITE_STORAGE_STICK_ALWAYS
          && browser_session_site_storage_tier(&session, news)
                 == BROWSER_SITE_STORAGE_MEMORY);
    /* "Clear local storage" covers every site, in RAM and on the stick,
       and leaves the site's other storage and its choice alone. */
    CHECK(browser_session_storage_set(&session, news, true, "r", "1", 1)
          && browser_session_opfs_create(&session, game, "/keep",
                                         BROWSER_OPFS_FILE)
                 == BROWSER_OPFS_OK);
    CHECK(browser_session_storage_clear_all(&session, true));
    CHECK(!browser_session_storage_get(&session, news, true, "r", NULL,
                                       NULL)
          && !browser_session_storage_get(&session, game, true, "k", NULL,
                                          NULL)
          && browser_session_opfs_stat(&session, game, "/keep", NULL)
                 == BROWSER_OPFS_OK
          && browser_session_site_storage_tier(&session, game)
                 == BROWSER_SITE_STORAGE_STICK_ALWAYS);
    /* The removals are logged: a reboot does not bring the key back, even
       when the site is cleared before its first use that boot. */
    CHECK(browser_session_storage_set(&session, game, true, "k", "v", 1));
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    /* A site whose file cannot be loaded keeps its data, so the clear
       reports failure rather than success. */
    budget_inject_failure_after(&budget, 0);
    bool refused = !browser_session_storage_clear_all(&session, true);
    budget_clear_failure_injection(&budget);
    CHECK(refused
          && browser_session_storage_get(&session, game, true, "k", NULL,
                                         NULL)
          && browser_session_storage_clear_all(&session, true));
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && !browser_session_storage_get(&session, game, true, "k", NULL,
                                          NULL)
          && browser_session_opfs_stat(&session, game, "/keep", NULL)
                 == BROWSER_OPFS_OK
          && browser_session_storage_set(&session, game, true, "k", "v",
                                         1));
    /* Clear site data empties the site but keeps the choice. */
    CHECK(browser_session_clear_site_data(&session, game)
          && !always_file(name, sizeof(name))
          && !browser_session_storage_get(&session, game, true, "k", NULL,
                                          NULL)
          && browser_session_storage_set(&session, game, true, "k", "w", 1)
          && always_file(name, sizeof(name)));
    /* Forget deletes the file and the choice. */
    CHECK(browser_session_site_storage_forget(&session, game)
          && !always_file(name, sizeof(name))
          && browser_session_site_storage_count(&session) == 0);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static int test_torn_tail_and_compaction(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    memset(value, 'c', 64u * 1024u);
    /* Rewriting one key leaves dead records; the log is rewritten before
       it grows past twice the dead threshold plus live data. */
    for (int round = 0; round < 40; round++) {
        value[0] = (char) ('A' + round % 26);
        CHECK(browser_session_storage_set(&session, game, true, "big", value,
                                          64u * 1024u));
    }
    CHECK(browser_session_storage_set(&session, game, true, "small", "ok",
                                      2));
    browser_session_destroy(&session);
    char name[64];
    CHECK(always_file(name, sizeof(name)));
    long size = file_size(name);
    CHECK(size > 0 && size < 600L * 1024L);
    /* A crash mid-append leaves a torn record: what came before loads. */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "ab");
    CHECK(file != NULL && fwrite("\x01\x01\x05\x00garbage", 1, 11, file)
                              == 11);
    fclose(file);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    const char *read = NULL;
    size_t length = 0;
    CHECK(browser_session_storage_get(&session, game, true, "big", &read,
                                      &length)
          && length == 64u * 1024u && read[0] == 'A' + 39 % 26
          && browser_session_storage_get(&session, game, true, "small",
                                         &read, &length)
          && length == 2
          /* Rewritten without the torn tail (and any dead records). */
          && file_size(name) <= size
          && file_size(name) >= (long) (64u * 1024u + 2u * 24u + 10u));
    /* New writes land after the good records, and survive a reload. */
    CHECK(browser_session_storage_set(&session, game, true, "after", "1",
                                      1));
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && browser_session_storage_get(&session, game, true, "after",
                                         &read, &length)
          && browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A page that rewrites all of its data once must not leave the log at
   twice its live size: found on a device, where 24 rewritten 64 KiB chunks
   left 3.1 MB for 1.5 MB of data. */
static int test_full_rewrite_compacts(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    char key[16];
    for (int pass = 0; pass < 2; pass++) {
        for (int chunk = 0; chunk < 24; chunk++) {
            memset(value, 'a' + (chunk + pass) % 26, 64u * 1024u);
            snprintf(key, sizeof(key), "chunk%d", chunk);
            CHECK(browser_session_storage_set(&session, game, true, key,
                                              value, 64u * 1024u));
        }
    }
    char name[64];
    CHECK(always_file(name, sizeof(name)));
    long live = 24L * (64L * 1024L + 6L + 24L);
    CHECK(file_size(name) < live + live / 4);
    browser_session_destroy(&session);
    /* The same at load: a log written by a build that never compacted. */
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    const char *read = NULL;
    size_t length = 0;
    CHECK(browser_session_storage_get(&session, game, true, "chunk23", &read,
                                      &length)
          && length == 64u * 1024u && read[0] == 'a' + 24 % 26
          && file_size(name) < live + live / 4
          && browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* Overwrites every chunk until a compaction has run (the armed fault
   is consumed); false if none did. */
static bool rewrite_until_compaction(BrowserSession *session, bool *armed)
{
    char key[16];
    for (int pass = 0; pass < 3 && *armed; pass++) {
        for (int chunk = 0; chunk < 24 && *armed; chunk++) {
            memset(value, 'a' + (chunk + pass) % 26, 64u * 1024u);
            snprintf(key, sizeof(key), "chunk%d", chunk);
            if (!browser_session_storage_set(session, game, true, key,
                                             value, 64u * 1024u))
                return false;
        }
    }
    return !*armed;
}

static bool chunks_readable(BrowserSession *session)
{
    char key[16];
    for (int chunk = 0; chunk < 24; chunk++) {
        const char *read = NULL;
        size_t length = 0;
        snprintf(key, sizeof(key), "chunk%d", chunk);
        if (!browser_session_storage_get(session, game, true, key, &read,
                                         &length)
            || length != 64u * 1024u) return false;
    }
    return true;
}

/* Writes a copy of the site file from under name to, with one byte
   flipped: at at, or (at < 0) in the second record's key. */
static bool damaged_copy(const char *from, const char *to, long at)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", directory, from);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    static unsigned char bytes[4u * MIB];
    size_t length = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    if (at < 0) {
        int seen = 0;
        for (size_t i = 0; i + 5 <= length && at < 0; i++)
            if (memcmp(bytes + i, "chunk", 5) == 0 && ++seen == 2)
                at = (long) i;
    }
    if (at < 0 || (size_t) at >= length) return false;
    bytes[at] ^= 0x5a;
    snprintf(path, sizeof(path), "%s/%s", directory, to);
    file = fopen(path, "wb");
    if (file == NULL) return false;
    bool written = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

/* After a crash mid-swap, a rewritten log beside the backup is adopted
   only if every record and value in it checks out; one with an intact
   header but a damaged record or value gives way to the backup. */
static int test_swap_recovery_checks_contents(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    TilefinchTestFaults *faults = tilefinch_test_faults();
    char name[64], backup[80];
    (void) always_file(name, sizeof(name));
    snprintf(backup, sizeof(backup), "%s.bak", name);
    for (int round = 0; round < 2; round++) {
        faults->crash_next_site_storage_compact = true;
        CHECK(rewrite_until_compaction(
            &session, &faults->crash_next_site_storage_compact));
        browser_session_destroy(&session);
        /* The log's name holds the backup's records with one damaged: a
           record's key, then a byte of the last (live) record's value. */
        long size = file_size(backup);
        CHECK(!file_exists(name) && size > 0
              && damaged_copy(backup, name, round == 0 ? -1 : size - 1000));
        CHECK(browser_session_init(&session, &budget, 32u * 1024u)
              && browser_session_site_storage_configure(&session, directory)
              && browser_session_site_storage_set_policy(
                     &session, game, BROWSER_SITE_STORAGE_STICK)
              && chunks_readable(&session)
              && file_exists(name) && !file_exists(backup));
    }
    CHECK(browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A compaction swaps the rewritten log in without ever leaving the site
   without a complete one: a power loss right after the original moved
   aside loses nothing at the next boot, and a failed swap puts the
   original back so this session keeps reading it. */
static int test_compaction_swap_is_crash_safe(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK));
    TilefinchTestFaults *faults = tilefinch_test_faults();
    faults->crash_next_site_storage_compact = true;
    CHECK(rewrite_until_compaction(
        &session, &faults->crash_next_site_storage_compact));
    char name[64], other[80];
    (void) always_file(name, sizeof(name));
    /* "Power loss": the original is only the backup now. */
    snprintf(other, sizeof(other), "%s.bak", name);
    CHECK(!file_exists(name) && file_exists(other));
    /* A damaged log beside the backup does not cost the backup: only a
       log whose header holds replaces it. */
    char damaged_path[256];
    snprintf(damaged_path, sizeof(damaged_path), "%s/%s", directory, name);
    FILE *damaged = fopen(damaged_path, "wb");
    CHECK(damaged != NULL && fputs("damaged", damaged) >= 0
          && fclose(damaged) == 0);
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && chunks_readable(&session)
          && file_exists(name) && !file_exists(other));
    snprintf(other, sizeof(other), "%s.tmp", name);
    CHECK(!file_exists(other));

    /* The swap's rename fails: the original is restored and read on. */
    faults->fail_next_site_storage_compact_rename = true;
    CHECK(rewrite_until_compaction(
        &session, &faults->fail_next_site_storage_compact_rename));
    CHECK(file_exists(name) && !file_exists(other)
          && chunks_readable(&session)
          && browser_session_storage_set(&session, game, true, "after",
                                         "x", 1));
    browser_session_destroy(&session);
    const char *read = NULL;
    size_t length = 0;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && chunks_readable(&session)
          && browser_session_storage_get(&session, game, true, "after",
                                         &read, &length)
          && length == 1
          && browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A removal the Memory Stick refuses to log is reported and not applied,
   so it cannot silently come back at the next boot. */
static int test_failed_removal_is_not_applied(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && browser_session_storage_set(&session, game, true, "keep",
                                         "value", 5)
          && browser_session_storage_set(&session, game, true, "other",
                                         "v", 1));
    TilefinchTestFaults *faults = tilefinch_test_faults();
    const char *read = NULL;
    size_t length = 0;
    faults->fail_site_storage_appends = 1;
    CHECK(!browser_session_storage_remove(&session, game, true, "keep")
          && browser_session_storage_get(&session, game, true, "keep",
                                         &read, &length)
          && length == 5);
    faults->fail_site_storage_appends = 2;
    CHECK(!browser_session_storage_clear(&session, game, true)
          && browser_session_storage_get(&session, game, true, "keep",
                                         &read, &length)
          && browser_session_storage_get(&session, game, true, "other",
                                         &read, &length));
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && browser_session_storage_get(&session, game, true, "keep",
                                         &read, &length)
          && length == 5
          && browser_session_storage_remove(&session, game, true, "keep")
          && !browser_session_storage_get(&session, game, true, "keep",
                                          &read, &length));
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && !browser_session_storage_get(&session, game, true, "keep",
                                          &read, &length)
          && browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && faults->fail_site_storage_appends == 0);
    return 0;
}

static int test_snapshot_skips_stick_sites(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    char snapshot[192];
    snprintf(snapshot, sizeof(snapshot), "%s/local.bin", directory);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_storage_set(&session, news, true, "ram", "1", 1)
          && browser_session_storage_set(&session, game, true, "stick", "2",
                                         1)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && browser_session_persistence_save(
                 &session, snapshot, BROWSER_SESSION_PERSIST_LOCAL_STORAGE,
                 NULL) == BROWSER_SESSION_PERSISTENCE_OK);
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_site_storage_set_policy(
                 &session, game, BROWSER_SITE_STORAGE_STICK)
          && browser_session_persistence_load(
                 &session, snapshot, BROWSER_SESSION_PERSIST_LOCAL_STORAGE,
                 NULL) == BROWSER_SESSION_PERSISTENCE_OK);
    const char *read = NULL;
    size_t length = 0;
    CHECK(browser_session_storage_get(&session, news, true, "ram", &read,
                                      &length)
          && browser_session_storage_get(&session, game, true, "stick",
                                         &read, &length)
          && length == 1 && read[0] == '2'
          && browser_session_storage_length(&session, game, true) == 1
          && browser_session_site_storage_forget(&session, game));
    browser_session_destroy(&session);
    browser_session_persistence_remove(snapshot);
    CHECK(budget.current == 0);
    return 0;
}

static int test_captive_portal_is_ram_only(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    memset(value, 'p', 40u * 1024u);
    CHECK(browser_session_init(&session, &budget, 32u * 1024u)
          && browser_session_site_storage_configure(&session, directory)
          && browser_session_storage_set(&session, game, true, "k", "v", 1)
          && browser_session_captive_portal_begin(&session,
                                                  "http://portal.test/")
          && !browser_session_storage_get(&session, game, true, "k", NULL,
                                          NULL)
          && !browser_session_storage_set(&session, "http://portal.test/",
                                          true, "big", value, 40u * 1024u)
          && !browser_session_site_storage_take_request(&session, NULL));
    browser_session_captive_portal_end(&session);
    CHECK(browser_session_storage_get(&session, game, true, "k", NULL,
                                      NULL));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static int test_boot_cleanup(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/s-03", directory);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    fclose(file);
    /* Without the marker nothing is scanned or removed. */
    browser_site_storage_remove_session_files(directory);
    CHECK(file_exists("s-03"));
    snprintf(path, sizeof(path), "%s/session-files", directory);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    fclose(file);
    browser_site_storage_remove_session_files(directory);
    CHECK(!file_exists("s-03") && !file_exists("session-files"));
    return 0;
}

int main(void)
{
    snprintf(directory, sizeof(directory),
             "/tmp/tilefinch-site-storage-XXXXXX");
    if (mkdtemp(directory) == NULL) return 1;
    int failed = test_per_site_allowances()
        || test_background_growth()
        || test_offer_and_session_grant()
        || test_decline_and_policies()
        || test_always_survives_reboot()
        || test_forget_and_clear()
        || test_torn_tail_and_compaction()
        || test_full_rewrite_compacts()
        || test_compaction_swap_is_crash_safe()
        || test_swap_recovery_checks_contents()
        || test_failed_removal_is_not_applied()
        || test_snapshot_skips_stick_sites()
        || test_captive_portal_is_ram_only()
        || test_boot_cleanup();
    rmdir(directory);
    if (failed) return 1;
    puts("session-site-storage-tests status=PASS");
    return 0;
}
