/*
 * Host-side tests for the cross-boot TLS session store
 * (docs/engineering/PSP_TRANSPORT.md, src/tls_session_store.c).
 *
 * Exercises the file round-trip, the corruption -> miss discipline, LRU
 * eviction under the host cap, expiry and far-future (wrong-RTC) pruning with
 * an injected clock, the over-cap skip, and the store-format-version and
 * crypto-pin mismatch gates. Curl and Mbed TLS are deliberately not linked:
 * the store is pure data plus file I/O.
 */

#include "tilefinch/tls_session_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define PIN UINT32_C(0x03060600)

static char g_path[512];

static void make_temp_path(void)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') dir = "/tmp";
    snprintf(g_path, sizeof(g_path), "%s/tf-tls-store-%ld.bin", dir,
             (long) getpid());
}

static void cleanup(void)
{
    tls_session_store_remove(g_path);
}

typedef struct {
    size_t count;
    char keys[64][TLS_SESSION_STORE_MAX_KEY_BYTES];
    unsigned char first_blob[TLS_SESSION_STORE_MAX_ENTRY_BYTES];
    size_t first_blob_length;
} ImportCapture;

static void capture_import(void *context, const char *key,
                           const unsigned char *blob, size_t blob_length,
                           int32_t ietf_tls_id)
{
    (void) ietf_tls_id;
    ImportCapture *capture = context;
    if (capture->count < 64) {
        snprintf(capture->keys[capture->count],
                 sizeof(capture->keys[0]), "%s", key);
        if (capture->count == 0 && blob_length <= sizeof(capture->first_blob)) {
            memcpy(capture->first_blob, blob, blob_length);
            capture->first_blob_length = blob_length;
        }
    }
    capture->count++;
}

/* Round-trip: add, save, load, and the blob survives byte-for-byte. */
static int test_round_trip(void)
{
    unsigned char blob[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x7F};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "example.com:443:G", blob, sizeof(blob),
                                2000, 772 /* TLS1.3 */, 1000, 1) == 1);
    CHECK(tls_session_store_count(store) == 1);
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    tls_session_store_free(store);

    TlsSessionStore *loaded = tls_session_store_load(g_path, PIN, 1000);
    CHECK(loaded != NULL);
    CHECK(tls_session_store_count(loaded) == 1);
    ImportCapture capture = {0};
    CHECK(tls_session_store_for_each(loaded, 1000, capture_import, &capture)
          == 1);
    CHECK(capture.count == 1);
    CHECK(strcmp(capture.keys[0], "example.com:443:G") == 0);
    CHECK(capture.first_blob_length == sizeof(blob));
    CHECK(memcmp(capture.first_blob, blob, sizeof(blob)) == 0);
    tls_session_store_free(loaded);
    cleanup();
    return 0;
}

/* A single flipped payload byte fails the checksum -> miss. */
static int test_corruption_is_miss(void)
{
    unsigned char blob[] = {1, 2, 3, 4, 5, 6, 7, 8};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "corrupt.example:443:G", blob,
                                sizeof(blob), 2000, 772, 1000, 1) == 1);
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    tls_session_store_free(store);

    FILE *file = fopen(g_path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, 40, SEEK_SET) == 0); /* into the payload */
    int byte = fgetc(file);
    CHECK(byte != EOF);
    CHECK(fseek(file, 40, SEEK_SET) == 0);
    CHECK(fputc(byte ^ 0xFF, file) != EOF);
    fclose(file);

    /* No .bak exists, so a torn primary is a straight miss. */
    CHECK(tls_session_store_load(g_path, PIN, 1000) == NULL);
    cleanup();
    return 0;
}

/* Torn primary recovers from .bak (backup rotation). */
static int test_backup_recovery(void)
{
    unsigned char blob[] = {9, 8, 7, 6};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "backup.example:443:G", blob,
                                sizeof(blob), 2000, 772, 1000, 1) == 1);
    /* First save creates the primary; second save rotates it to .bak. */
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    tls_session_store_free(store);

    /* Truncate the primary; loader must fall back to .bak. */
    FILE *file = fopen(g_path, "wb");
    CHECK(file != NULL);
    CHECK(fputc('x', file) != EOF);
    fclose(file);

    TlsSessionStore *loaded = tls_session_store_load(g_path, PIN, 1000);
    CHECK(loaded != NULL);
    CHECK(tls_session_store_count(loaded) == 1);
    tls_session_store_free(loaded);
    cleanup();
    return 0;
}

/* LRU eviction: over the host cap, the least-recently-used host is dropped. */
static int test_lru_eviction(void)
{
    unsigned char blob[] = {0x11, 0x22};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    char key[64];
    /* Fill all 16 host slots with increasing last_use. */
    for (unsigned i = 0; i < TLS_SESSION_STORE_MAX_HOSTS; i++) {
        snprintf(key, sizeof(key), "host%02u.example:443:G", i);
        CHECK(tls_session_store_add(store, key, blob, sizeof(blob),
                                    100000, 772, 1000, i + 1) == 1);
    }
    CHECK(tls_session_store_count(store) == TLS_SESSION_STORE_MAX_HOSTS);
    /* host00 has the smallest last_use (1); adding a 17th host evicts it. */
    CHECK(tls_session_store_add(store, "host99.example:443:G", blob,
                                sizeof(blob), 100000, 772, 1000, 999) == 1);
    CHECK(tls_session_store_count(store) == TLS_SESSION_STORE_MAX_HOSTS);

    ImportCapture capture = {0};
    tls_session_store_for_each(store, 1000, capture_import, &capture);
    int saw_host00 = 0, saw_host99 = 0;
    for (size_t i = 0; i < capture.count; i++) {
        if (strcmp(capture.keys[i], "host00.example:443:G") == 0) saw_host00 = 1;
        if (strcmp(capture.keys[i], "host99.example:443:G") == 0) saw_host99 = 1;
    }
    CHECK(!saw_host00);
    CHECK(saw_host99);
    tls_session_store_free(store);
    return 0;
}

/* At most two sessions per host; a third evicts the oldest for that key. */
static int test_per_host_cap(void)
{
    unsigned char blob[] = {0xAB};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    const char *key = "multi.example:443:G";
    CHECK(tls_session_store_add(store, key, blob, sizeof(blob), 5000, 772,
                                1000, 1) == 1);
    CHECK(tls_session_store_add(store, key, blob, sizeof(blob), 5000, 772,
                                1000, 2) == 1);
    CHECK(tls_session_store_add(store, key, blob, sizeof(blob), 5000, 772,
                                1000, 3) == 1);
    CHECK(tls_session_store_count(store) == TLS_SESSION_STORE_MAX_PER_HOST);
    tls_session_store_free(store);
    return 0;
}

/* Expiry and wrong-RTC far-future entries are pruned when the clock is known;
 * an unknown clock (now <= 0) keeps them. */
static int test_expiry_pruning(void)
{
    unsigned char blob[] = {0x5A, 0x5A};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    /* Expired: valid_until < now. */
    CHECK(tls_session_store_add(store, "expired.example:443:G", blob,
                                sizeof(blob), 500, 772, 1000, 1) == 0);
    /* Far future (wrong RTC at export): beyond the 8-day ceiling. */
    CHECK(tls_session_store_add(store, "future.example:443:G", blob,
                                sizeof(blob),
                                1000 + TLS_SESSION_STORE_MAX_FUTURE_SECONDS
                                    + 10, 772, 1000, 2) == 0);
    /* Live. */
    CHECK(tls_session_store_add(store, "live.example:443:G", blob,
                                sizeof(blob), 2000, 772, 1000, 3) == 1);
    CHECK(tls_session_store_count(store) == 1);
    /* Unknown clock keeps everything. */
    CHECK(tls_session_store_add(store, "unknown.example:443:G", blob,
                                sizeof(blob), 500, 772, 0, 4) == 1);
    CHECK(tls_session_store_count(store) == 2);

    /* Save with a known clock prunes non-live entries on the way out. Add an
     * expired one directly is impossible (add drops it), so verify the load
     * side: write with now=0, then load with a clock past validity. */
    TlsSessionStore *store2 = tls_session_store_create();
    CHECK(store2 != NULL);
    CHECK(tls_session_store_add(store2, "aa.example:443:G", blob, sizeof(blob),
                                1500, 772, 0, 1) == 1);
    CHECK(tls_session_store_add(store2, "bb.example:443:G", blob, sizeof(blob),
                                9000, 772, 0, 2) == 1);
    CHECK(tls_session_store_save(store2, g_path, PIN, 0) == 0);
    tls_session_store_free(store2);
    /* Load at now=2000: aa (valid_until 1500) is expired, bb survives. */
    TlsSessionStore *loaded = tls_session_store_load(g_path, PIN, 2000);
    CHECK(loaded != NULL);
    CHECK(tls_session_store_count(loaded) == 1);
    tls_session_store_free(loaded);
    tls_session_store_free(store);
    cleanup();
    return 0;
}

/* An oversized blob is skipped, not stored. */
static int test_over_cap_skip(void)
{
    static unsigned char big[TLS_SESSION_STORE_MAX_ENTRY_BYTES + 1];
    memset(big, 0x33, sizeof(big));
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "big.example:443:G", big, sizeof(big),
                                5000, 772, 1000, 1) == 0);
    CHECK(tls_session_store_count(store) == 0);
    /* Exactly at the cap is accepted. */
    CHECK(tls_session_store_add(store, "ok.example:443:G", big,
                                TLS_SESSION_STORE_MAX_ENTRY_BYTES, 5000, 772,
                                1000, 1) == 1);
    tls_session_store_free(store);
    return 0;
}

/* A store-format-version bump makes an old file a miss (bump-and-ignore). */
static int test_version_mismatch_is_miss(void)
{
    unsigned char blob[] = {1, 2, 3, 4};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "ver.example:443:G", blob, sizeof(blob),
                                5000, 772, 1000, 1) == 1);
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    tls_session_store_free(store);

    /* Bump the version field (offset 8) in place. */
    FILE *file = fopen(g_path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, 8, SEEK_SET) == 0);
    CHECK(fputc(0x02, file) != EOF); /* version 2 != 1 */
    fclose(file);
    CHECK(tls_session_store_load(g_path, PIN, 1000) == NULL);
    cleanup();
    return 0;
}

/* A crypto-pin mismatch (different Mbed TLS build) is a miss. */
static int test_crypto_pin_mismatch_is_miss(void)
{
    unsigned char blob[] = {1, 2, 3, 4};
    TlsSessionStore *store = tls_session_store_create();
    CHECK(store != NULL);
    CHECK(tls_session_store_add(store, "pin.example:443:G", blob, sizeof(blob),
                                5000, 772, 1000, 1) == 1);
    CHECK(tls_session_store_save(store, g_path, PIN, 1000) == 0);
    tls_session_store_free(store);
    CHECK(tls_session_store_load(g_path, PIN, 1000) != NULL);
    /* A different pin (e.g. an Mbed TLS upgrade) is a straight miss. */
    TlsSessionStore *other = tls_session_store_load(g_path, PIN + 1, 1000);
    CHECK(other == NULL);
    cleanup();
    return 0;
}

/* A missing file is a plain miss, and remove is idempotent. */
static int test_missing_is_miss(void)
{
    cleanup();
    CHECK(tls_session_store_load(g_path, PIN, 1000) == NULL);
    CHECK(tls_session_store_remove(g_path) == 0);
    return 0;
}

int main(void)
{
    make_temp_path();
    cleanup();
    struct { const char *name; int (*fn)(void); } tests[] = {
        {"round_trip", test_round_trip},
        {"corruption_is_miss", test_corruption_is_miss},
        {"backup_recovery", test_backup_recovery},
        {"lru_eviction", test_lru_eviction},
        {"per_host_cap", test_per_host_cap},
        {"expiry_pruning", test_expiry_pruning},
        {"over_cap_skip", test_over_cap_skip},
        {"version_mismatch_is_miss", test_version_mismatch_is_miss},
        {"crypto_pin_mismatch_is_miss", test_crypto_pin_mismatch_is_miss},
        {"missing_is_miss", test_missing_is_miss},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int rc = tests[i].fn();
        if (rc != 0) {
            fprintf(stderr, "tls-session-store test '%s' FAILED\n",
                    tests[i].name);
            cleanup();
            return 1;
        }
        printf("tls-session-store test '%s' ok\n", tests[i].name);
    }
    cleanup();
    printf("tilefinch-tls-session-store: outcome=pass\n");
    return 0;
}
