#include "tilefinch/session_persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "PERSISTENCE CHECK failed at %s:%d: %s\n",         \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char persistence_path[] =
    "/tmp/tilefinch-session-persistence-test.bin";

static uint32_t test_hash_update(uint32_t hash, const void *data,
                                 size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static void test_put_u32(unsigned char output[4], uint32_t value)
{
    output[0] = (unsigned char) value;
    output[1] = (unsigned char) (value >> 8);
    output[2] = (unsigned char) (value >> 16);
    output[3] = (unsigned char) (value >> 24);
}

static bool forge_cached_etag_control(const char *path)
{
    static const unsigned char needle[] = "\"css-v1\"";
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long signed_length = ftell(file);
    if (signed_length < 36 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t) signed_length;
    unsigned char *bytes = malloc(length);
    if (bytes == NULL || fread(bytes, 1, length, file) != length
        || fclose(file) != 0) {
        free(bytes);
        return false;
    }
    size_t found = SIZE_MAX;
    for (size_t at = 36; at + sizeof(needle) - 1u <= length; at++) {
        if (memcmp(bytes + at, needle, sizeof(needle) - 1u) == 0) {
            found = at;
            break;
        }
    }
    if (found == SIZE_MAX) {
        free(bytes);
        return false;
    }
    bytes[found + 1u] = '\n';
    uint32_t hash = test_hash_update(
        UINT32_C(2166136261), bytes + 36, length - 36u);
    test_put_u32(bytes + 24, hash);
    file = fopen(path, "wb");
    bool ok = false;
    if (file != NULL) {
        ok = fwrite(bytes, 1, length, file) == length;
        if (fclose(file) != 0) ok = false;
    }
    free(bytes);
    return ok;
}

static bool write_truncated_blob_file(const char *path)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'S', 'E', 'S', 'S', 1, 0
    };
    static const char url[] = "https://truncated.test/body";
    unsigned char bytes[36u + 8u + 4u + sizeof(url) - 1u + 4u] = {0};
    memcpy(bytes, magic, sizeof(magic));
    test_put_u32(bytes + 8, 1);
    test_put_u32(bytes + 12, 36);
    test_put_u32(bytes + 16, BROWSER_SESSION_PERSIST_CACHE);
    test_put_u32(bytes + 20, sizeof(bytes) - 36u);
    test_put_u32(bytes + 28, 1);
    size_t at = 36;
    test_put_u32(bytes + at, 1);
    at += 4;
    test_put_u32(bytes + at, sizeof(bytes) - 44u);
    at += 4;
    test_put_u32(bytes + at, sizeof(url) - 1u);
    at += 4;
    memcpy(bytes + at, url, sizeof(url) - 1u);
    at += sizeof(url) - 1u;
    test_put_u32(bytes + at, 4u * 1024u * 1024u);
    uint32_t hash = test_hash_update(
        UINT32_C(2166136261), bytes + 36, sizeof(bytes) - 36u);
    test_put_u32(bytes + 24, hash);
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL
        && fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    if (file != NULL && fclose(file) != 0) ok = false;
    return ok;
}

static bool flip_file_byte(const char *path, size_t offset)
{
    FILE *file = fopen(path, "r+b");
    if (file == NULL || fseek(file, (long) offset, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    int byte = fgetc(file);
    bool ok = byte != EOF && fseek(file, -1, SEEK_CUR) == 0
        && fputc(byte ^ 0x80, file) != EOF;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long signed_length = ftell(file);
    if (signed_length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t) signed_length;
    unsigned char *bytes = malloc(length == 0 ? 1u : length);
    bool found = false;
    if (bytes != NULL && fread(bytes, 1, length, file) == length) {
        size_t needle_length = strlen(needle);
        for (size_t at = 0; at + needle_length <= length; at++) {
            if (memcmp(bytes + at, needle, needle_length) == 0) {
                found = true;
                break;
            }
        }
    }
    free(bytes);
    fclose(file);
    return found;
}

static BrowserCacheEntry *find_cache(BrowserSession *session,
                                     const char *url)
{
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        BrowserCacheEntry *entry = &session->cache[i];
        if (entry->data != NULL && strcmp(entry->url, url) == 0)
            return entry;
    }
    return NULL;
}

static bool populate(BrowserSession *session)
{
    static const unsigned char css[] = "body{color:#123}";
    static const unsigned char image[] = {0x89, 'P', 'N', 'G'};
    const char *css_url = "https://static.test/site.css";
    const char *image_url = "https://static.test/icon.png";
    const unsigned char *ignored = NULL;
    size_t ignored_length = 0;
    return browser_session_cache_put_http(
               session, css_url, css, sizeof(css) - 1u,
               "\"css-v1\"", "Mon, 27 Jul 2026 00:00:00 GMT",
               "text/css", "max-age=3600", "Accept-Encoding",
               UINT64_C(9000000000000))
        && browser_session_cache_set_response_provenance(
               session, css_url, "https://cdn.test/site.css",
               "strict-origin")
        && browser_session_cache_put_http(
               session, image_url, image, sizeof(image),
               "\"image-v1\"", NULL, "image/png", "immutable", NULL,
               UINT64_C(9000000000000))
        /* Make CSS newer than the image so lowering the cap has a stable
           observable LRU victim after persistence. */
        && browser_session_cache_get(
               session, css_url, &ignored, &ignored_length)
        && ignored_length == sizeof(css) - 1u
        && browser_session_cookie_set_http(
               session, "https://www.wikipedia.org/",
               "durable=yes; Path=/; Secure; HttpOnly; Max-Age=3600")
        && browser_session_cookie_set_http(
               session, "https://www.wikipedia.org/",
               "session_only=no; Path=/; Secure")
        && browser_session_storage_set(
               session, "https://www.wikipedia.org/", true,
               "theme", "dark", 4)
        && browser_session_storage_set(
               session, "https://www.wikipedia.org/", false,
               "tab", "source-only", 11);
}

static int test_round_trip_and_clear(void)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BrowserSession source, target;
    CHECK(browser_session_init(&source, &budget, 4u * 1024u * 1024u)
          && browser_session_init(&target, &budget, 4u * 1024u * 1024u)
          && populate(&source));
    static const unsigned char protected_css[] = "body{color:secret}";
    unsigned char *protected_copy = budget_malloc(
        &budget, sizeof(protected_css) - 1u);
    CHECK(protected_copy != NULL);
    memcpy(protected_copy, protected_css, sizeof(protected_css) - 1u);
    BrowserSharedBody *protected_body = browser_shared_body_take(
        &budget, protected_copy, sizeof(protected_css) - 1u);
    TilefinchRequestContext protected_context = {
        .target_url = "https://cdn.test/private.css",
        .initiator_url = "https://page-a.test/article",
        .top_level_url = "https://page-a.test/article",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant protected_grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_CROSS_ORIGIN,
        .cors_validated = true
    };
    CHECK(protected_body != NULL
          && browser_session_cache_put_http_shared_resource(
                 &source, protected_context.target_url, protected_body,
                 NULL, NULL, "text/css", "max-age=3600", NULL, 1,
                 &protected_context, &protected_grant));
    browser_shared_body_release(protected_body);
    CHECK(browser_session_storage_set(
              &target, "https://old.test/", true, "old", "gone", 4)
          && browser_session_storage_set(
              &target, "https://www.wikipedia.org/", false,
              "tab", "target-session", 14));

    BrowserSessionPersistenceLimits limits;
    browser_session_persistence_limits_default(&limits);
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK
          && !file_contains(persistence_path, "durable=yes")
          && !file_contains(persistence_path, "session_only=no")
          && !file_contains(persistence_path, "source-only")
          && browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK
          && browser_session_storage_get(
              &target, "https://old.test/", true, "old", NULL, NULL)
          && browser_session_persistence_load(
              &target, persistence_path,
              BROWSER_SESSION_PERSIST_LOCAL_STORAGE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);

    BrowserCacheEntry *css = find_cache(
        &target, "https://static.test/site.css");
    BrowserCacheEntry *image = find_cache(
        &target, "https://static.test/icon.png");
    CHECK(css != NULL && image != NULL
          && css->length == sizeof("body{color:#123}") - 1u
          && strcmp(css->etag, "\"css-v1\"") == 0
          && strcmp(css->content_type, "text/css") == 0
          && css->response_url_known
          && css->response_referrer_policy_known
          && strcmp(browser_cache_entry_response_url(css),
                    "https://cdn.test/site.css") == 0
          && strcmp(css->response_referrer_policy, "strict-origin") == 0);
    const BrowserCacheEntry *protected_match = NULL;
    CHECK(browser_session_cache_lookup(
              &target, protected_context.target_url) == NULL
          && browser_session_cache_match_resource(
                 &target, protected_context.target_url,
                 &protected_context, 1, &protected_match)
                 == BROWSER_CACHE_MISS
          && protected_match == NULL);
    const BrowserCacheEntry *matched = NULL;
    /* The saved monotonic deadline was very high. It must not turn into a
       falsely fresh response after this process/reboot boundary. */
    CHECK(browser_session_cache_match_http(
              &target, "https://static.test/site.css", 1, &matched)
              == BROWSER_CACHE_STALE
          && matched == css);

    const char *value = NULL;
    size_t value_length = 0;
    char cookies[256];
    CHECK(browser_session_storage_get(
              &target, "https://www.wikipedia.org/", true,
              "theme", &value, &value_length)
          && value_length == 4 && memcmp(value, "dark", 4) == 0
          && !browser_session_storage_get(
              &target, "https://old.test/", true, "old", NULL, NULL)
          && browser_session_storage_get(
              &target, "https://www.wikipedia.org/", false,
              "tab", &value, &value_length)
          && value_length == 14
          && memcmp(value, "target-session", 14) == 0
          && browser_session_cookie_header(
              &target, "https://www.wikipedia.org/", cookies,
              sizeof(cookies))
          && cookies[0] == '\0');

    size_t css_length = css->length;
    CHECK(browser_session_cache_set_maximum_bytes(&target, css_length)
          && find_cache(&target, "https://static.test/site.css") != NULL
          && find_cache(&target, "https://static.test/icon.png") == NULL
          && !browser_session_cache_set_maximum_bytes(&target, 0)
          && !browser_session_cache_set_maximum_bytes(
                 &target, budget.limit + 1u));
    CHECK(browser_session_cookie_set_http(
        &target, "https://www.wikipedia.org/",
        "clear-me=yes; Path=/; Secure"));
    CHECK(browser_session_persistence_clear(
              &target, BROWSER_SESSION_PERSIST_ALL)
              == BROWSER_SESSION_PERSISTENCE_OK);
    browser_session_cookie_clear(&target);
    CHECK(target.cache_bytes == 0 && target.cookie_bytes == 0
          && browser_session_storage_length(
                 &target, "https://www.wikipedia.org/", true) == 0
          && browser_session_storage_length(
                 &target, "https://www.wikipedia.org/", false) == 1);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && browser_session_persistence_load(
              &target, persistence_path,
              BROWSER_SESSION_PERSIST_LOCAL_STORAGE, &limits)
                 == BROWSER_SESSION_PERSISTENCE_NOT_FOUND);

    browser_session_destroy(&target);
    browser_session_destroy(&source);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

static int test_transaction_limits_and_backup(void)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BrowserSession source, target;
    CHECK(browser_session_init(&source, &budget, 4u * 1024u * 1024u)
          && browser_session_init(&target, &budget, 4u * 1024u * 1024u)
          && populate(&source)
          && browser_session_cache_put(
              &target, "https://sentinel.test/keep",
              (const unsigned char *) "keep", 4)
          && browser_session_storage_set(
              &target, "https://sentinel.test/", true,
              "keep", "yes", 3));
    BrowserSessionPersistenceLimits limits;
    browser_session_persistence_limits_default(&limits);
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);

    /* The checksum protects accidental damage, not a deliberately edited
       Memory Stick file. Restored validators must still be safe to copy into
       conditional HTTP request headers, and rejection is transactional. */
    CHECK(forge_cached_etag_control(persistence_path)
          && browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_CORRUPT
          && find_cache(&target, "https://sentinel.test/keep") != NULL
          && browser_session_storage_get(
              &target, "https://sentinel.test/", true,
              "keep", NULL, NULL)
          && browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);

    FILE *file = fopen(persistence_path, "r+b");
    CHECK(file != NULL && fseek(file, 41, SEEK_SET) == 0);
    int byte = fgetc(file);
    CHECK(byte != EOF && fseek(file, -1, SEEK_CUR) == 0
          && fputc(byte ^ 0x40, file) != EOF && fclose(file) == 0);
    CHECK(browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_CORRUPT
          && find_cache(&target, "https://sentinel.test/keep") != NULL
          && browser_session_storage_get(
              &target, "https://sentinel.test/", true,
              "keep", NULL, NULL));

    file = fopen(persistence_path, "wb");
    CHECK(file != NULL && fwrite("TFSESS", 1, 6, file) == 6
          && fclose(file) == 0
          && browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_CORRUPT
          && find_cache(&target, "https://sentinel.test/keep") != NULL
          && browser_session_storage_get(
              &target, "https://sentinel.test/", true,
              "keep", NULL, NULL));

    /* Restore a valid primary, then force the first allocation in load to
       fail. The existing state remains intact. */
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);
    /* The synthetic primary preceding that save was corrupt. A second normal
       save rotates the now-valid primary into the retained backup. */
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);
    budget_inject_failure_after(&budget, 0);
    CHECK(browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_ALL,
              &limits) == BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY);
    budget_clear_failure_injection(&budget);
    CHECK(find_cache(&target, "https://sentinel.test/keep") != NULL);

    BrowserSessionPersistenceLimits small = limits;
    small.maximum_cache_entries = 1;
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &small) == BROWSER_SESSION_PERSISTENCE_OK);
    BrowserSession one_entry;
    CHECK(browser_session_init(
              &one_entry, &budget, 4u * 1024u * 1024u)
          && browser_session_persistence_load(
              &one_entry, persistence_path,
              BROWSER_SESSION_PERSIST_CACHE, &small)
                 == BROWSER_SESSION_PERSISTENCE_OK
          && find_cache(
                 &one_entry, "https://static.test/site.css") != NULL
          && find_cache(
                 &one_entry, "https://static.test/icon.png") == NULL);
    browser_session_destroy(&one_entry);

    /* Every replacement retains the preceding complete generation at .bak.
       A torn/corrupt primary therefore recovers without trusting partial data. */
    char backup[sizeof(persistence_path) + 4u];
    snprintf(backup, sizeof(backup), "%s.bak", persistence_path);
    file = fopen(backup, "rb");
    CHECK(file != NULL && fclose(file) == 0);
    file = fopen(persistence_path, "wb");
    CHECK(file != NULL && fwrite("bad", 1, 3, file) == 3
          && fclose(file) == 0);
    BrowserSession recovered;
    CHECK(browser_session_init(
              &recovered, &budget, 4u * 1024u * 1024u)
          && browser_session_persistence_load(
              &recovered, persistence_path,
              BROWSER_SESSION_PERSIST_CACHE, &limits)
                 == BROWSER_SESSION_PERSISTENCE_OK
          && find_cache(
                 &recovered, "https://static.test/site.css") != NULL);

    browser_session_destroy(&recovered);
    browser_session_destroy(&target);
    browser_session_destroy(&source);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

static int test_smaller_live_cache_and_truncated_input(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    BrowserSession source, target;
    CHECK(browser_session_init(
              &source, &budget, 1024u * 1024u)
          && browser_session_init(
              &target, &budget, 256u * 1024u));
    const size_t body_bytes = 160u * 1024u;
    unsigned char *body = malloc(body_bytes);
    CHECK(body != NULL);
    memset(body, 0x5a, body_bytes);
    CHECK(browser_session_cache_put(
              &source, "https://cache.test/oldest", body, body_bytes)
          && browser_session_cache_put(
              &source, "https://cache.test/newer", body, body_bytes));
    const unsigned char *ignored = NULL;
    size_t ignored_length = 0;
    /* Make the first URL newest; serialized order must not be slot order. */
    CHECK(browser_session_cache_get(
        &source, "https://cache.test/oldest", &ignored, &ignored_length));
    free(body);
    BrowserSessionPersistenceLimits limits;
    browser_session_persistence_limits_default(&limits);
    limits.maximum_cache_bytes = 1024u * 1024u;
    CHECK(browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK
          && browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK
          && target.cache_bytes <= target.maximum_cache_bytes
          && find_cache(&target, "https://cache.test/oldest") != NULL
          && find_cache(&target, "https://cache.test/newer") == NULL);
    const BrowserCacheEntry *matched = NULL;
    CHECK(browser_session_cache_match_http(
              &target, "https://cache.test/oldest", UINT64_MAX, &matched)
              == BROWSER_CACHE_STALE
          && matched != NULL);
    browser_session_destroy(&target);
    browser_session_destroy(&source);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && budget.current == 0 && budget.external_reserved == 0);

    /*
     * A larger live LRU need not fit its independent disk ceiling. Snapshot
     * selection retains the newest bodies that fit, then a still-smaller live
     * restore retains the hottest tail of that selected set.
     */
    Budget tier_budget;
    budget_init(&tier_budget, 8u * 1024u * 1024u);
    BrowserSession tier_source, disk_restore, small_restore;
    CHECK(browser_session_init(
              &tier_source, &tier_budget, 4u * 1024u * 1024u)
          && browser_session_init(
              &disk_restore, &tier_budget, 4u * 1024u * 1024u)
          && browser_session_init(
              &small_restore, &tier_budget, 512u * 1024u));
    const size_t tier_body_bytes = 400u * 1024u;
    unsigned char *tier_body = malloc(tier_body_bytes);
    CHECK(tier_body != NULL);
    memset(tier_body, 0x3c, tier_body_bytes);
    CHECK(browser_session_cache_put(
              &tier_source, "https://tier.test/oldest",
              tier_body, tier_body_bytes)
          && browser_session_cache_put(
              &tier_source, "https://tier.test/middle",
              tier_body, tier_body_bytes)
          && browser_session_cache_put(
              &tier_source, "https://tier.test/newest",
              tier_body, tier_body_bytes));
    free(tier_body);
    BrowserSessionPersistenceLimits disk_limit;
    browser_session_persistence_limits_default(&disk_limit);
    disk_limit.maximum_cache_bytes = 1024u * 1024u;
    CHECK(browser_session_persistence_save(
              &tier_source, persistence_path,
              BROWSER_SESSION_PERSIST_CACHE, &disk_limit)
                 == BROWSER_SESSION_PERSISTENCE_OK
          && browser_session_persistence_load(
              &disk_restore, persistence_path,
              BROWSER_SESSION_PERSIST_CACHE, &disk_limit)
                 == BROWSER_SESSION_PERSISTENCE_OK
          && find_cache(
                 &disk_restore, "https://tier.test/oldest") == NULL
          && find_cache(
                 &disk_restore, "https://tier.test/middle") != NULL
          && find_cache(
                 &disk_restore, "https://tier.test/newest") != NULL
          && browser_session_persistence_load(
              &small_restore, persistence_path,
              BROWSER_SESSION_PERSIST_CACHE, &disk_limit)
                 == BROWSER_SESSION_PERSISTENCE_OK
          && find_cache(
                 &small_restore, "https://tier.test/middle") == NULL
          && find_cache(
                 &small_restore, "https://tier.test/newest") != NULL);
    browser_session_destroy(&small_restore);
    browser_session_destroy(&disk_restore);
    browser_session_destroy(&tier_source);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && tier_budget.current == 0
          && tier_budget.external_reserved == 0);

    /*
     * A declared body larger than the remaining file is corruption before it
     * is an allocation request. A 2 MiB Budget could not satisfy the forged
     * 4 MiB length, so OUT_OF_MEMORY here would expose the old ordering bug.
     */
    Budget small_budget;
    budget_init(&small_budget, 2u * 1024u * 1024u);
    BrowserSession sentinel;
    CHECK(browser_session_init(
              &sentinel, &small_budget, 256u * 1024u)
          && browser_session_cache_put(
              &sentinel, "https://sentinel.test/keep",
              (const unsigned char *) "ok", 2)
          && write_truncated_blob_file(persistence_path)
          && browser_session_persistence_load(
              &sentinel, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              NULL) == BROWSER_SESSION_PERSISTENCE_CORRUPT
          && find_cache(
              &sentinel, "https://sentinel.test/keep") != NULL);
    browser_session_destroy(&sentinel);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && small_budget.current == 0
          && small_budget.external_reserved == 0);
    return 0;
}

static int test_oversized_live_body_streams_without_allocation(void)
{
    static const char large_url[] = "https://skip.test/large";
    static const char small_url[] = "https://skip.test/small";
    const size_t large_body_bytes = 768u * 1024u;

    Budget source_budget;
    budget_init(&source_budget, 4u * 1024u * 1024u);
    BrowserSession source;
    CHECK(browser_session_init(
              &source, &source_budget, 2u * 1024u * 1024u));
    unsigned char *large_body = malloc(large_body_bytes);
    CHECK(large_body != NULL);
    memset(large_body, 0xa7, large_body_bytes);
    CHECK(browser_session_cache_put(
              &source, large_url, large_body, large_body_bytes)
          && browser_session_cache_put(
              &source, small_url,
              (const unsigned char *) "small", 5));
    free(large_body);

    BrowserSessionPersistenceLimits limits;
    browser_session_persistence_limits_default(&limits);
    limits.maximum_cache_bytes = 1024u * 1024u;
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && browser_session_persistence_save(
              &source, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK);
    browser_session_destroy(&source);
    CHECK(source_budget.current == 0
          && source_budget.external_reserved == 0);

    /*
     * Leave room for the transactional BrowserSession and a modest amount of
     * metadata, but not for the 768 KiB body. A loader that allocates the
     * disk-sized body before applying the 256 KiB live cap fails this gate.
     */
    Budget target_budget;
    budget_init(&target_budget, 4u * 1024u * 1024u);
    BrowserSession target;
    CHECK(browser_session_init(
              &target, &target_budget, 256u * 1024u));
    size_t baseline = target_budget.current;
    size_t load_allowance = sizeof(BrowserSession)
        + target.accounting_bytes + 128u * 1024u;
    CHECK(load_allowance <= target_budget.limit - baseline);
    target_budget.limit = baseline + load_allowance;
    target_budget.peak = baseline;
    CHECK(browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_OK
          && target_budget.failure_count == 0
          && target_budget.peak <= baseline + load_allowance
          && find_cache(&target, large_url) == NULL
          && find_cache(&target, small_url) != NULL);

    /*
     * Skipping is still a real read: damage inside the discarded body must
     * alter the whole-file hash and leave the already-live session untouched.
     * The large entry is serialized first because it was inserted first.
     */
    size_t large_body_offset = 36u + 8u + 4u
        + strlen(large_url) + 4u;
    CHECK(flip_file_byte(persistence_path, large_body_offset)
          && browser_session_persistence_load(
              &target, persistence_path, BROWSER_SESSION_PERSIST_CACHE,
              &limits) == BROWSER_SESSION_PERSISTENCE_CORRUPT
          && find_cache(&target, large_url) == NULL
          && find_cache(&target, small_url) != NULL);

    browser_session_destroy(&target);
    CHECK(browser_session_persistence_remove(persistence_path)
              == BROWSER_SESSION_PERSISTENCE_OK
          && target_budget.current == 0
          && target_budget.external_reserved == 0);
    return 0;
}

int main(void)
{
    (void) browser_session_persistence_remove(persistence_path);
    CHECK(test_round_trip_and_clear() == 0);
    CHECK(test_transaction_limits_and_backup() == 0);
    CHECK(test_smaller_live_cache_and_truncated_input() == 0);
    CHECK(test_oversized_live_body_streams_without_allocation() == 0);
    puts("session persistence: bounded transactional round-trip passed");
    return 0;
}
