/* Writes the Memory Stick seed used by the `site-restore` scripted-input
   scenario: a profile with local-storage persistence and the persistent cache
   enabled, a local-storage snapshot, and a cache snapshot large enough to take
   several 16 KiB restore slices. It uses the shipping writers, so the files
   are whatever format the browser currently reads.

     tilefinch-site-data-fixture --write DIR   (re)generate the seed
     tilefinch-site-data-fixture --check DIR   load it back with the shipping
                                               readers; run as a ctest so a
                                               format change fails on the host
                                               instead of as a silent no-op
                                               inside the emulator

   Regenerate with --write and commit the result whenever --check fails. */

#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/session.h"
#include "tilefinch/session_persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_ORIGIN "https://site-restore.test/"
#define FIXTURE_CACHE_ENTRIES 6u
#define FIXTURE_CACHE_BODY_BYTES (20u * 1024u)

static bool path_in(char *output, size_t size, const char *directory,
                    const char *name)
{
    int written = snprintf(output, size, "%s/%s", directory, name);
    return written > 0 && (size_t) written < size;
}

static bool populate(BrowserSession *session)
{
    /* Local storage is capped at 16 KiB per session; stay well inside it. */
    char value[1536];
    memset(value, 'v', sizeof(value) - 1u);
    value[sizeof(value) - 1u] = '\0';
    for (unsigned at = 0; at < 6u; at++) {
        char key[32];
        snprintf(key, sizeof(key), "restore-key-%u", at);
        if (!browser_session_storage_set(session, FIXTURE_ORIGIN, true, key,
                                         value, strlen(value))) return false;
    }
    unsigned char *body = malloc(FIXTURE_CACHE_BODY_BYTES);
    if (body == NULL) return false;
    bool ok = true;
    for (unsigned at = 0; ok && at < FIXTURE_CACHE_ENTRIES; at++) {
        char url[96];
        snprintf(url, sizeof(url), FIXTURE_ORIGIN "asset-%u.css", at);
        memset(body, 'a' + (int) at, FIXTURE_CACHE_BODY_BYTES);
        ok = browser_session_cache_put_http(
            session, url, body, FIXTURE_CACHE_BODY_BYTES, "\"fixture\"",
            NULL, "text/css", "max-age=31536000", NULL,
            UINT64_C(9000000000000));
    }
    free(body);
    return ok;
}

static int write_fixture(const char *directory)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    char path[512];
    BrowserProfile *profile = browser_profile_create(&budget);
    if (profile == NULL) return 1;
    browser_profile_set_persist_local_storage(profile, true);
    browser_profile_set_persistent_cache_mb(profile, 1u);
    bool ok = path_in(path, sizeof(path), directory, "profile.cfg")
        && browser_profile_save(profile, path);
    browser_profile_destroy(profile);

    BrowserSession session;
    ok = ok && browser_session_init(&session, &budget, 4u * 1024u * 1024u);
    if (ok) {
        BrowserSessionPersistenceLimits limits;
        browser_session_persistence_limits_default(&limits);
        ok = populate(&session)
            && path_in(path, sizeof(path), directory, "local-storage.bin")
            && browser_session_persistence_save(
                   &session, path, BROWSER_SESSION_PERSIST_LOCAL_STORAGE,
                   &limits) == BROWSER_SESSION_PERSISTENCE_OK
            && path_in(path, sizeof(path), directory, "http-cache.bin")
            && browser_session_persistence_save(
                   &session, path, BROWSER_SESSION_PERSIST_CACHE, &limits)
                   == BROWSER_SESSION_PERSISTENCE_OK;
        browser_session_destroy(&session);
    }
    if (!ok) fprintf(stderr, "site-data fixture: could not write %s\n",
                     directory);
    return ok ? 0 : 1;
}

static int check_fixture(const char *directory)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    char path[512];
    BrowserProfile *profile = browser_profile_create(&budget);
    bool ok = profile != NULL
        && path_in(path, sizeof(path), directory, "profile.cfg")
        && browser_profile_load(profile, path)
        && browser_profile_persist_local_storage(profile)
        && browser_profile_persistent_cache_mb(profile) == 1u;
    if (!ok) fprintf(stderr, "site-data fixture: profile.cfg is stale\n");
    if (profile != NULL) browser_profile_destroy(profile);

    BrowserSession session;
    bool initialized =
        browser_session_init(&session, &budget, 4u * 1024u * 1024u);
    if (ok && initialized) {
        BrowserSessionPersistenceLimits limits;
        browser_session_persistence_limits_default(&limits);
        bool storage = path_in(path, sizeof(path), directory,
                               "local-storage.bin")
            && browser_session_persistence_load(
                   &session, path, BROWSER_SESSION_PERSIST_LOCAL_STORAGE,
                   &limits) == BROWSER_SESSION_PERSISTENCE_OK;
        const char *value = NULL;
        size_t value_length = 0;
        storage = storage && browser_session_storage_get(
            &session, FIXTURE_ORIGIN, true, "restore-key-5", &value,
            &value_length) && value_length == 1535u;
        if (!storage)
            fprintf(stderr, "site-data fixture: local-storage.bin is stale\n");
        bool cache = path_in(path, sizeof(path), directory, "http-cache.bin")
            && browser_session_persistence_load(
                   &session, path, BROWSER_SESSION_PERSIST_CACHE, &limits)
                   == BROWSER_SESSION_PERSISTENCE_OK;
        const unsigned char *body = NULL;
        size_t body_length = 0;
        cache = cache && browser_session_cache_get(
            &session, FIXTURE_ORIGIN "asset-5.css", &body, &body_length)
            && body_length == FIXTURE_CACHE_BODY_BYTES;
        if (!cache)
            fprintf(stderr, "site-data fixture: http-cache.bin is stale\n");
        ok = storage && cache;
    } else {
        ok = false;
    }
    if (initialized) browser_session_destroy(&session);
    if (ok) puts("site-data fixture: PASS");
    else fprintf(stderr,
                 "regenerate with: tilefinch-site-data-fixture --write %s\n",
                 directory);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_fixture(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_fixture(argv[2]);
    fprintf(stderr, "usage: %s --write DIR | --check DIR\n", argv[0]);
    return 2;
}
