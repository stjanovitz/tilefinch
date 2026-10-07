/* The persistent compiled-script tier (src/session_script_disk.c): classic
   and module bytecode kept across browser restarts. A "restart" is a fresh
   BrowserSession (nothing in RAM) on the same directory.

   Every directory is a fresh one under TILEFINCH_TEST_SCRATCH_DIR (CTest
   sets it inside the build tree; a manual run may point it at any host
   directory). Nothing here can reach a Memory Stick: the tier refuses a
   device path on host builds, and the test checks that it does. */
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/session.h"
#include "tilefinch/session_persistence.h"
#include "tilefinch/url.h"
#include "tilefinch/viewport.h"
#include "tilefinch/platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <lexbor/dom/interfaces/node.h>

#include "script_test_support.h"
#include "../src/tilefinch_test_faults.h"

#define MIB (1024u * 1024u)
#define KIB 1024u

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static char scratch_root[512];
static unsigned directory_serial;

/* ---- directories ---- */

static bool make_scratch_root(void)
{
    return script_test_make_scratch_root(scratch_root, sizeof(scratch_root),
                                         "script-disk");
}

static void fresh_directory(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s/cache-%u", scratch_root, ++directory_serial);
}

/* Files in `directory` whose name ends with `suffix` (NULL: all); the first
   one's path goes to `first`. */
static size_t count_files(const char *directory, const char *suffix,
                          char *first, size_t capacity)
{
    size_t count = 0;
    DIR *dir = opendir(directory);
    if (dir == NULL) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        size_t length = strlen(entry->d_name);
        if (suffix != NULL
            && (length < strlen(suffix)
                || strcmp(entry->d_name + length - strlen(suffix), suffix)
                       != 0)) continue;
        if (count++ == 0 && first != NULL)
            snprintf(first, capacity, "%s/%s", directory, entry->d_name);
    }
    closedir(dir);
    return count;
}

static bool exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static long file_size(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (long) info.st_size : -1;
}

static unsigned idle(BrowserSession *session, unsigned limit)
{
    unsigned turns = 0;
    while (turns < limit && browser_session_script_disk_maintenance(session))
        turns++;
    return turns;
}

/* Reads a whole file, lets `edit` change it, recomputes the CRC-32 trailer
   (so only the edit, not a checksum mismatch, is what a reader sees), and
   writes it back. */
static bool rewrite_file(const char *path, void (*edit)(unsigned char *,
                                                        size_t),
                         bool fix_trailer)
{
    long size = file_size(path);
    if (size < 8) return false;
    unsigned char *data = malloc((size_t) size);
    FILE *file = fopen(path, "rb");
    bool ok = data != NULL && file != NULL
        && fread(data, 1, (size_t) size, file) == (size_t) size;
    if (file != NULL) fclose(file);
    if (ok) {
        edit(data, (size_t) size);
        if (fix_trailer) {
            uint32_t crc = (uint32_t) crc32(0L, data, (uInt) (size - 4));
            data[size - 4] = (unsigned char) crc;
            data[size - 3] = (unsigned char) (crc >> 8);
            data[size - 2] = (unsigned char) (crc >> 16);
            data[size - 1] = (unsigned char) (crc >> 24);
        }
        file = fopen(path, "wb");
        ok = file != NULL
            && fwrite(data, 1, (size_t) size, file) == (size_t) size;
        if (file != NULL) ok = fclose(file) == 0 && ok;
    }
    free(data);
    return ok;
}

static void edit_abi(unsigned char *data, size_t length)
{
    (void) length;
    data[8] ^= 0x5au; /* the pack header's bytecode ABI */
}

static void edit_damage(unsigned char *data, size_t length)
{
    data[length / 2u] ^= 0xffu;
}

/* ---- a page with one external classic script ---- */

static const char page_url[] = "https://site.test/";

typedef ScriptPageFixture PageFixture;

static bool page_open(PageFixture *fixture)
{
    return script_page_fixture_open(
        fixture, "<script src=/app.js></script>", 0);
}

static bool page_close(PageFixture *fixture)
{
    return script_page_fixture_close(fixture);
}

/* A browser start: a fresh session on `directory` (NULL: tier off). */
static bool session_start(PageFixture *fixture, BrowserSession *session,
                          const char *directory, bool write)
{
    if (!browser_session_init(session, &fixture->budget, 640u * KIB))
        return false;
    fixture->options.session = session;
    return directory == NULL
        || browser_session_script_disk_configure(session, directory, write,
                                                 0);
}

/* About `minimum` bytes of lazily compiled functions and a summary naming
   `tag`. Caller frees. */
static char *make_script(const char *tag, size_t minimum)
{
    size_t capacity = minimum + 256u;
    char *source = malloc(capacity);
    if (source == NULL) return NULL;
    size_t length = 0;
    for (unsigned i = 0; length + 192u < minimum; i++) {
        length += (size_t) snprintf(
            source + length, capacity - length,
            "function f%u(a){var b=a*%u,c='%s';for(var i=0;i<b;i++){"
            "c+=String.fromCharCode(65+i%%26)}if(c.length>b){return c}"
            "return [a,b,c].join(',')+'/'+typeof a}\n", i, i, tag);
    }
    snprintf(source + length, capacity - length,
             "globalThis.pocSummary='RAN:%s:'+f1(2).length;", tag);
    return source;
}

/* One visit: a fresh realm evaluates `source` from `url`, then the page's
   idle turns store what the load queued (the persistent tier is not run:
   callers do that with idle()). */
static bool visit(PageFixture *fixture, const char *url, const char *source,
                  bool no_store, ScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000, page_url,
        &fixture->options, result);
    if (runtime == NULL) return false;
    bool ok = script_runtime_evaluate_external_classic_cached(
        runtime, fixture->script, source, strlen(source), url, url,
        no_store, result);
    for (unsigned turn = 0; turn < 1000u
         && script_runtime_store_pending_bytecode(runtime, result); turn++) {}
    script_runtime_destroy(runtime);
    return ok;
}

static bool ran(const ScriptResult *result, const char *tag)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "RAN:%s:", tag);
    if (strncmp(result->summary, expected, strlen(expected)) == 0)
        return true;
    fprintf(stderr, "summary=%s (wanted %s...) error=%s\n", result->summary,
            expected, result->error);
    return false;
}

/* ---- tests ---- */

/* The tier off (the default) or read-only writes nothing at all. */
static int test_off_writes_nothing(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    char *source = make_script("off", 24u * KIB);
    CHECK(source != NULL);
    BrowserSession session;
    /* Off: no directory configured. */
    CHECK(session_start(&fixture, &session, NULL, false));
    ScriptResult result;
    CHECK(visit(&fixture, "https://site.test/app.js", source, false,
                &result) && ran(&result, "off"));
    CHECK(!browser_session_script_disk_enabled(&session)
          && idle(&session, 100) == 0 && !exists(directory));
    browser_session_destroy(&session);
    /* Read-only (boot.cfg module_cache_write=0): reads, never writes. */
    CHECK(session_start(&fixture, &session, directory, false));
    CHECK(visit(&fixture, "https://site.test/app.js", source, false,
                &result) && ran(&result, "off"));
    (void) idle(&session, 100);
    CHECK(!exists(directory));
    browser_session_destroy(&session);
    free(source);
    CHECK(page_close(&fixture));
    return 0;
}

/* On: the load writes nothing; idle turns write the pack in slices of at
   most BROWSER_SCRIPT_DISK_WRITE_SLICE; a restarted browser restores the
   script from it without compiling, with identical behaviour. */
static int test_idle_write_and_restart_restore(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("app", 96u * KIB);
    CHECK(source != NULL);
    BrowserSession session;
    CHECK(session_start(&fixture, &session, directory, true));
    ScriptResult first;
    CHECK(visit(&fixture, url, source, false, &first) && ran(&first, "app")
          && first.external_script_bytecode_cache_stores == 1);
    /* Nothing on disk before an idle turn. */
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);
    /* The first idle turns load the (absent) index and sweep; then the
       pack is written a slice at a time: after one write slice it is still
       a temporary file. */
    unsigned turns = 0;
    while (turns < 50u && count_files(directory, ".tmp", NULL, 0) == 0
           && browser_session_script_disk_maintenance(&session)) turns++;
    CHECK(count_files(directory, ".tmp", NULL, 0) == 1
          && count_files(directory, ".tfsc", NULL, 0) == 0);
    CHECK(idle(&session, 1000) > 1);
    char pack[700];
    CHECK(count_files(directory, ".tfsc", pack, sizeof(pack)) == 1
          && count_files(directory, ".tmp", NULL, 0) == 0
          && exists(directory) && file_size(pack) > 96 * 1024);
    BrowserScriptDiskStats stats;
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.writes == 1 && stats.index_writes == 1 && stats.files == 1
          && stats.bytes == (uint64_t) file_size(pack));
    browser_session_destroy(&session);

    /* Restart. */
    CHECK(session_start(&fixture, &session, directory, true));
    ScriptResult second;
    CHECK(visit(&fixture, url, source, false, &second)
          && ran(&second, "app"));
    if (second.script_bytecode_disk_hits != 1)
        fprintf(stderr, "restart: hits=%zu disk=%zu misses=%zu\n",
                second.external_script_bytecode_cache_hits,
                second.script_bytecode_disk_hits,
                second.external_script_bytecode_cache_misses);
    CHECK(second.script_bytecode_disk_hits == 1
          && second.external_script_bytecode_cache_hits == 1
          && second.external_script_bytecode_cache_misses == 0
          && second.host_compile_attempts + 1 == first.host_compile_attempts
          && strcmp(second.summary, first.summary) == 0);
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.index_reads == 1 && stats.reads == 1 && stats.promoted == 1
          && stats.rejects == 0);
    /* Already on disk: the restored record is not written again. */
    CHECK(idle(&session, 1000) <= 2u);
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.writes == 0);
    /* A script the index does not name costs no read. */
    ScriptResult other;
    CHECK(visit(&fixture, "https://site.test/other.js", source, false,
                &other) && ran(&other, "app"));
    browser_session_script_disk_stats(&session, &stats);
    CHECK(other.script_bytecode_disk_hits == 0 && stats.reads == 1
          && stats.index_misses >= 1);
    browser_session_destroy(&session);
    free(source);
    CHECK(page_close(&fixture));
    script_test_remove_tree(directory);
    return 0;
}

/* A pack from another bytecode ABI (or engine build) is ignored and
   removed: the script compiles. A damaged pack likewise. */
static int test_abi_mismatch_and_damage_fall_back(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("abi", 32u * KIB);
    CHECK(source != NULL);
    void (*edits[2])(unsigned char *, size_t) = {edit_abi, edit_damage};
    for (int round = 0; round < 2; round++) {
        char directory[600];
        fresh_directory(directory, sizeof(directory));
        BrowserSession session;
        CHECK(session_start(&fixture, &session, directory, true));
        ScriptResult result;
        CHECK(visit(&fixture, url, source, false, &result)
              && ran(&result, "abi"));
        (void) idle(&session, 1000);
        browser_session_destroy(&session);
        char pack[700];
        CHECK(count_files(directory, ".tfsc", pack, sizeof(pack)) == 1);
        /* The ABI edit keeps the checksum valid; damage does not. */
        CHECK(rewrite_file(pack, edits[round], round == 0));

        CHECK(session_start(&fixture, &session, directory, true));
        CHECK(visit(&fixture, url, source, false, &result)
              && ran(&result, "abi"));
        BrowserScriptDiskStats stats;
        browser_session_script_disk_stats(&session, &stats);
        CHECK(result.script_bytecode_disk_hits == 0
              && result.external_script_bytecode_cache_misses == 1
              && stats.reads == 1 && stats.rejects == 1
              && !exists(pack));
        /* The fresh compile is written again at idle and restores. */
        (void) idle(&session, 1000);
        browser_session_destroy(&session);
        CHECK(count_files(directory, ".tfsc", NULL, 0) == 1);
        CHECK(session_start(&fixture, &session, directory, true));
        CHECK(visit(&fixture, url, source, false, &result)
              && ran(&result, "abi")
              && result.script_bytecode_disk_hits == 1);
        browser_session_destroy(&session);
        script_test_remove_tree(directory);
    }
    free(source);
    CHECK(page_close(&fixture));
    return 0;
}

/* Another engine build's packs, temporary files, retired per-module files
   and orphans are swept by a writing session; unrelated files are kept. */
static int test_sweep_removes_foreign_files(void)
{
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    CHECK(mkdir(directory, 0777) == 0);
    static const char *names[] = {
        "zzzzzzzz-00112233445566778899aabbccddeeff.tfsc",
        "zzzzzzzz-00112233445566778899aabbccddeeff.tfmb",
        "zzzzzzzz-00112233445566778899aabbccddeeff.tfsc.tmp",
        "index.tfsi.tmp",
        "notes.txt"
    };
    char path[800];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        FILE *file = fopen(path, "wb");
        CHECK(file != NULL && fputs("x", file) >= 0 && fclose(file) == 0);
    }
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, true,
                                                   0));
    (void) idle(&session, 100);
    CHECK(count_files(directory, NULL, NULL, 0) == 1);
    snprintf(path, sizeof(path), "%s/notes.txt", directory);
    CHECK(exists(path));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

/* no-store responses and captive sign-ins never reach the card. */
static int test_no_store_and_captive_never_written(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    char *source = make_script("ns", 24u * KIB);
    CHECK(source != NULL);
    BrowserSession session;
    CHECK(session_start(&fixture, &session, directory, true));
    ScriptResult result;
    CHECK(visit(&fixture, "https://site.test/private.js", source, true,
                &result) && ran(&result, "ns"));
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);

    /* A captive sign-in: nothing is stored, nothing written. */
    CHECK(browser_session_captive_portal_begin(&session,
                                               "http://portal.test/"));
    CHECK(visit(&fixture, "https://site.test/portal.js", source, false,
                &result) && ran(&result, "ns")
          && result.external_script_bytecode_cache_stores == 0);
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);
    browser_session_captive_portal_end(&session);

    /* Records stored before a sign-in wait for it to end. */
    CHECK(visit(&fixture, "https://site.test/app.js", source, false, &result)
          && result.external_script_bytecode_cache_stores == 1);
    CHECK(browser_session_captive_portal_begin(&session,
                                               "http://portal.test/"));
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);
    browser_session_captive_portal_end(&session);
    /* ...and site data disallowed writes nothing either. */
    session.site_data_allowed = false;
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);
    session.site_data_allowed = true;
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 1);
    browser_session_destroy(&session);
    free(source);
    CHECK(page_close(&fixture));
    script_test_remove_tree(directory);
    return 0;
}

/* Clearing the cache empties the directory; a site's data clear removes
   that site's packs (and RAM records) only. */
static int test_clears_remove_files(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    char *source = make_script("clr", 24u * KIB);
    CHECK(source != NULL);
    BrowserSession session;
    CHECK(session_start(&fixture, &session, directory, true));
    ScriptResult result;
    CHECK(visit(&fixture, "https://cdn.test/a.js", source, false, &result));
    ScriptRuntimeOptions saved = fixture.options;
    fixture.options.top_level_url = "https://other.test/";
    CHECK(visit(&fixture, "https://cdn.test/a.js", source, false, &result));
    fixture.options = saved;
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 2);

    CHECK(browser_session_clear_site_data(&session, "https://other.test/x"));
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 1
          && browser_session_script_bytecode_entries(
                 &session, BROWSER_SCRIPT_BYTECODE_CLASSIC) == 1);
    uint64_t bytes = 0;
    size_t files = 0;
    CHECK(browser_session_script_disk_usage(&session, &bytes, &files)
          && files == 1 && bytes != 0);
    /* The remaining site still restores after a restart... */
    browser_session_destroy(&session);
    CHECK(session_start(&fixture, &session, directory, true));
    CHECK(visit(&fixture, "https://cdn.test/a.js", source, false, &result)
          && result.script_bytecode_disk_hits == 1);
    /* ...until the cache is cleared. */
    CHECK(browser_session_persistence_clear(
              &session, BROWSER_SESSION_PERSIST_CACHE)
          == BROWSER_SESSION_PERSISTENCE_OK);
    CHECK(count_files(directory, NULL, NULL, 0) == 0
          && browser_session_script_disk_usage(&session, &bytes, &files)
          && files == 0 && bytes == 0);
    browser_session_destroy(&session);
    CHECK(session_start(&fixture, &session, directory, true));
    CHECK(visit(&fixture, "https://cdn.test/a.js", source, false, &result)
          && result.script_bytecode_disk_hits == 0
          && result.external_script_bytecode_cache_misses == 1);
    browser_session_destroy(&session);
    free(source);
    CHECK(page_close(&fixture));
    script_test_remove_tree(directory);
    return 0;
}

/* "Clear data for this site" while the site's page is open: a compile the
   page queued for idle storage before the clear is dropped, so neither the
   RAM table nor the tier gets the site's script back. A read-only tier,
   which cannot remove the site's packs, stops reading them. */
static int test_site_clear_reaches_queue_and_read_only_tier(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    char *source = make_script("sit", 24u * KIB);
    CHECK(source != NULL);
    static const char url[] = "https://cdn.test/site.js";
    BrowserSession session;
    CHECK(session_start(&fixture, &session, directory, true));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_external_classic_cached(
              runtime, fixture.script, source, strlen(source), url, url,
              false, &result)
          && ran(&result, "sit")
          && script_runtime_pending_bytecode_count(runtime) == 1);
    CHECK(browser_session_clear_site_data(&session, page_url));
    for (unsigned turn = 0; turn < 1000u
         && script_runtime_store_pending_bytecode(runtime, &result); turn++) {}
    (void) idle(&session, 1000);
    CHECK(result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_deferred_dropped == 1
          && browser_session_script_bytecode_entries(
                 &session, BROWSER_SCRIPT_BYTECODE_CLASSIC) == 0
          && count_files(directory, ".tfsc", NULL, 0) == 0);
    script_runtime_destroy(runtime);

    /* Written by a later visit, then cleared through a read-only tier. */
    CHECK(visit(&fixture, url, source, false, &result)
          && result.external_script_bytecode_cache_stores == 1);
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 1);
    browser_session_destroy(&session);
    CHECK(session_start(&fixture, &session, directory, false));
    CHECK(browser_session_clear_site_data(&session, page_url));
    CHECK(visit(&fixture, url, source, false, &result) && ran(&result, "sit")
          && result.script_bytecode_disk_hits == 0
          && result.external_script_bytecode_cache_misses == 1);
    browser_session_destroy(&session);
    free(source);
    CHECK(page_close(&fixture));
    script_test_remove_tree(directory);
    return 0;
}

/* Session-level: one RAM record per group with `bytes` of stand-in
   bytecode (the tier copies bytes; it never interprets them). */
static bool put_group(BrowserSession *session, unsigned index, size_t bytes,
                      uint32_t generation)
{
    static unsigned char bytecode[64u * 1024u];
    char url[64], source[64];
    snprintf(url, sizeof(url), "https://site.test/g%u.js", index);
    snprintf(source, sizeof(source), "var g%u=1;", index);
    memset(bytecode, (int) index, sizeof(bytecode));
    BrowserScriptBytecodeKey key = {
        .module_name = url, .response_url = url,
        .partition_key = "https://site.test",
        .source = (const unsigned char *) source,
        .source_length = strlen(source)
    };
    return bytes <= sizeof(bytecode)
        && browser_session_script_bytecode_put(
               session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key, generation,
               bytecode, bytes);
}

static bool group_on_disk(BrowserSession *session, unsigned index,
                          uint32_t generation)
{
    char url[64], source[64];
    snprintf(url, sizeof(url), "https://site.test/g%u.js", index);
    snprintf(source, sizeof(source), "var g%u=1;", index);
    BrowserScriptBytecodeKey key = {
        .module_name = url, .response_url = url,
        .partition_key = "https://site.test",
        .source = (const unsigned char *) source,
        .source_length = strlen(source)
    };
    return browser_session_script_disk_promote(
        session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key, generation);
}

/* The size ceiling holds, and the least recently used pack goes first: a
   pack read since it was written outlives an older unread one. */
typedef struct {
    bool cancel;
    size_t calls, last_read, largest_read;
} CacheReadProbe;

static bool cache_read_cooperate(void *opaque, const char *phase, size_t done)
{
    CacheReadProbe *probe = opaque;
    if (strcmp(phase, "script-cache-read") != 0) return true;
    probe->calls++;
    size_t delta = done - probe->last_read;
    if (delta > probe->largest_read) probe->largest_read = delta;
    probe->last_read = done;
    return !probe->cancel;
}

static int test_sliced_restore_retries_cancel(void)
{
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    uint32_t generation = browser_session_module_bytecode_generation(&session);
    CHECK(put_group(&session, 0, 64u * KIB, generation));
    (void) idle(&session, 1000);
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    generation = browser_session_module_bytecode_generation(&session);
    /* Index allocation refusal must not become a permanently empty index,
       nor let idle maintenance delete the existing pack. */
    size_t limit = budget.limit;
    budget.limit = budget.current;
    bool pressure_miss = group_on_disk(&session, 0, generation);
    (void) idle(&session, 2);
    budget.limit = limit;
    CHECK(!pressure_miss && count_files(directory, ".tfsc", NULL, 0) == 1);
    CacheReadProbe probe = {.cancel = true};
    TilefinchPlatformServices services = {
        .context = &probe, .cooperate = cache_read_cooperate
    };
    tilefinch_platform_set_services(&services);
    bool first = group_on_disk(&session, 0, generation);
    probe = (CacheReadProbe) {0};
    bool retried = group_on_disk(&session, 0, generation);
    tilefinch_platform_set_services(NULL);
    CHECK(!first && retried && probe.calls >= 4
        && probe.largest_read <= BROWSER_SCRIPT_DISK_WRITE_SLICE);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

static int test_restore_io_failure_is_generation_bounded(void)
{
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    uint32_t generation = browser_session_module_bytecode_generation(&session);
    CHECK(put_group(&session, 0, 64u * KIB, generation));
    (void) idle(&session, 1000);
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    generation = browser_session_module_bytecode_generation(&session);
    tilefinch_test_faults()->script_cache_read_attempts = 0;
    tilefinch_test_faults()->fail_next_script_cache_read = true;
    CHECK(!group_on_disk(&session, 0, generation));
    CHECK(!group_on_disk(&session, 0, generation)
        && tilefinch_test_faults()->script_cache_read_attempts == 1);
    generation = browser_session_module_bytecode_generation(&session);
    CHECK(group_on_disk(&session, 0, generation)
        && tilefinch_test_faults()->script_cache_read_attempts == 2);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

static int test_backup_index_survives_primary_refusal(void)
{
    char directory[600], primary[640], backup[640];
    fresh_directory(directory, sizeof(directory));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    CHECK(put_group(&session, 0, 64u * KIB,
        browser_session_module_bytecode_generation(&session)));
    (void) idle(&session, 1000);
    browser_session_destroy(&session);
    snprintf(primary, sizeof(primary), "%s/index.tfsi", directory);
    snprintf(backup, sizeof(backup), "%s/index.tfsi.tmp", directory);
    CHECK(rename(primary, backup) == 0);
    FILE *file = fopen(primary, "wb");
    unsigned char padding[4096] = {0};
    CHECK(file != NULL && fwrite(padding, 1, sizeof(padding), file) == sizeof(padding));
    CHECK(fclose(file) == 0);
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
        && browser_session_script_disk_configure(&session, directory, true, 0));
    size_t limit = budget.limit;
    budget.limit = budget.current + 1024u; /* Main refuses; small backup fits. */
    uint64_t bytes = 0;
    size_t files = 0;
    bool loaded = browser_session_script_disk_usage(&session, &bytes, &files);
    budget.limit = limit;
    CHECK(loaded && files == 1 && bytes > 64u * KIB);
    CHECK(browser_session_script_disk_usage(&session, NULL, &files) && files == 1);
    BrowserScriptDiskStats stats;
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.index_reads == 1); /* Successful backup stays latched. */
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

static int test_ceiling_and_lru(void)
{
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    /* Three 40 KiB packs fit in 128 KiB; a fourth does not. */
    const size_t ceiling = 128u * KIB, record = 40u * KIB;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, true,
                                                   ceiling));
    uint32_t generation = browser_session_module_bytecode_generation(
        &session);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(put_group(&session, i, record, generation));
        (void) idle(&session, 1000);
    }
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 3);
    browser_session_destroy(&session);

    /* Restart; read group 0 (the oldest), then add group 3. */
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, true,
                                                   ceiling));
    generation = browser_session_module_bytecode_generation(&session);
    CHECK(group_on_disk(&session, 0, generation));
    CHECK(put_group(&session, 3, record, generation));
    (void) idle(&session, 1000);
    BrowserScriptDiskStats stats;
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.evictions == 1 && stats.files == 3
          && stats.bytes <= ceiling);
    browser_session_destroy(&session);
    /* Group 1, the least recently used, is gone; 0, 2 and 3 remain. */
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, false,
                                                   ceiling));
    generation = browser_session_module_bytecode_generation(&session);
    CHECK(!group_on_disk(&session, 1, generation)
          && group_on_disk(&session, 0, generation)
          && group_on_disk(&session, 2, generation)
          && group_on_disk(&session, 3, generation));
    /* A group larger than the ceiling is kept in RAM only. */
    browser_session_destroy(&session);
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, true,
                                                   32u * KIB));
    generation = browser_session_module_bytecode_generation(&session);
    CHECK(put_group(&session, 4, record, generation));
    (void) idle(&session, 1000);
    browser_session_script_disk_stats(&session, &stats);
    CHECK(stats.skipped == 1 && stats.writes == 0);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

/* Every path a host build is given stays on the host. */
static int test_device_paths_refused(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB));
    static const char *devices[] = {
        "ms0:/PSP/GAME/TILEFINCH/data/script-cache", "host0:/script-cache",
        "ef0:/cache", "ms0:"
    };
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        CHECK(!browser_session_script_disk_configure(&session, devices[i],
                                                     true, 0)
              && !browser_session_script_disk_enabled(&session));
    }
    CHECK(strchr(scratch_root, ':') == NULL);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* ---- modules ---- */

#define MODULE_LIMIT 4u

typedef struct {
    const char *url[MODULE_LIMIT];
    const char *source[MODULE_LIMIT];
    size_t count;
} ModuleMap;

static char *copy_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1u);
    if (copy != NULL) memcpy(copy, text, length + 1u);
    return copy;
}

static bool module_load(void *opaque, const ScriptModuleLoadRequest *request,
                        ScriptModuleLoadResult *result)
{
    ModuleMap *map = opaque;
    for (size_t i = 0; i < map->count; i++) {
        if (strcmp(request->request_url, map->url[i]) != 0) continue;
        result->source = copy_text(map->source[i]);
        result->source_length = strlen(map->source[i]);
        result->response_url = copy_text(map->url[i]);
        if (result->source == NULL || result->response_url == NULL) {
            free(result->source);
            free(result->response_url);
            memset(result, 0, sizeof(*result));
            return false;
        }
        return true;
    }
    return false;
}

static void module_release(void *opaque, ScriptModuleLoadResult *result)
{
    (void) opaque;
    free(result->source);
    free(result->response_url);
    memset(result, 0, sizeof(*result));
}

static bool module_visit(PageFixture *fixture, ModuleMap *map,
                         const char *root, ScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000,
        "https://mod.test/", &fixture->options, result);
    if (runtime == NULL) return false;
    script_runtime_set_module_loader(runtime, module_load, module_release,
                                     map);
    (void) script_runtime_evaluate_external_module_context(
        runtime, fixture->script, root, strlen(root),
        "https://mod.test/root.js", "https://mod.test/root.js", "",
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, result);
    bool ok = true;
    for (size_t turn = 0; ok && turn < 8; turn++)
        ok = script_runtime_advance(runtime, 0, 32, result);
    script_runtime_destroy(runtime);
    return ok;
}

/* Modules use the same tier: a restarted browser restores the root and its
   imports from their packs, compiles nothing and behaves identically. */
static int test_modules_restore_after_restart(void)
{
    PageFixture fixture;
    CHECK(page_open(&fixture));
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    char *dep = make_script("dep", 12u * KIB);
    CHECK(dep != NULL);
    size_t dep_length = strlen(dep);
    char *dep_module = malloc(dep_length + 64u);
    CHECK(dep_module != NULL);
    snprintf(dep_module, dep_length + 64u,
             "%s\nexport const depValue=f2(3).length;", dep);
    ModuleMap map = {
        .url = {"https://mod.test/dep.js", "https://mod.test/leaf.js"},
        .source = {dep_module,
                   "export function leaf(){return import.meta.url}"},
        .count = 2
    };
    static const char root[] =
        "import {depValue} from './dep.js';"
        "import {leaf} from './leaf.js';"
        "globalThis.pocSummary='MOD:'+depValue+':'+leaf()";
    BrowserSession session;
    CHECK(session_start(&fixture, &session, directory, true));
    browser_session_module_bytecode_set_limit(&session, 1024u * 1024u);
    ScriptResult first;
    CHECK(module_visit(&fixture, &map, root, &first)
          && strncmp(first.summary, "MOD:", 4) == 0
          && first.module_bytecode_cache_stores == 3);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0);
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 3);
    browser_session_destroy(&session);

    CHECK(session_start(&fixture, &session, directory, true));
    browser_session_module_bytecode_set_limit(&session, 1024u * 1024u);
    ScriptResult second;
    CHECK(module_visit(&fixture, &map, root, &second));
    if (strcmp(second.summary, first.summary) != 0
        || second.script_bytecode_disk_hits != 3)
        fprintf(stderr, "modules after restart: %s hits=%zu disk=%zu "
                "compiles=%zu\n", second.summary,
                second.module_bytecode_cache_hits,
                second.script_bytecode_disk_hits,
                second.module_compile_count);
    CHECK(strcmp(second.summary, first.summary) == 0
          && second.script_bytecode_disk_hits == 3
          && second.module_bytecode_cache_hits == 3
          && second.module_compile_count == 0);
    browser_session_destroy(&session);
    free(dep_module);
    free(dep);
    CHECK(page_close(&fixture));
    script_test_remove_tree(directory);
    return 0;
}

/* RAM permits a record larger than one disk pack. It must remain in RAM,
   not wrap the subtraction gate and create a pack no reader can open. */
static int test_oversized_record_stays_in_ram(void)
{
    char directory[600];
    fresh_directory(directory, sizeof(directory));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB)
          && browser_session_script_disk_configure(&session, directory, true, 0));
    BrowserScriptBytecodeKey key = {
        .module_name = "https://site.test/large.js",
        .response_url = "https://site.test/large.js",
        .partition_key = "https://site.test",
        .source = (const unsigned char *) "var large=1;",
        .source_length = sizeof("var large=1;") - 1u
    };
    size_t length = BROWSER_SCRIPT_DISK_FILE_LIMIT + 1u;
    unsigned char *bytes = calloc(1, length);
    CHECK(bytes != NULL);
    uint32_t generation = browser_session_module_bytecode_generation(&session);
    CHECK(browser_session_script_bytecode_put(
              &session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key, generation,
              bytes, length));
    free(bytes);
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 0
          && browser_session_script_bytecode_entries(
                 &session, BROWSER_SCRIPT_BYTECODE_CLASSIC) == 1);
    /* Refusing the large group must not block subsequent normal packs. */
    key.module_name = key.response_url = "https://site.test/small.js";
    static const unsigned char small[] = {1, 2, 3};
    CHECK(browser_session_script_bytecode_put(
              &session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key, generation,
              small, sizeof(small)));
    (void) idle(&session, 1000);
    CHECK(count_files(directory, ".tfsc", NULL, 0) == 1);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    script_test_remove_tree(directory);
    return 0;
}

int main(void)
{
    if (!make_scratch_root()) {
        fprintf(stderr, "no scratch directory\n");
        return 1;
    }
    int failed = 0;
    failed |= test_device_paths_refused();
    puts("script disk cache: device paths refused");
    failed |= test_off_writes_nothing();
    puts("script disk cache: off writes nothing");
    failed |= test_idle_write_and_restart_restore();
    puts("script disk cache: idle write, restart restore");
    failed |= test_abi_mismatch_and_damage_fall_back();
    puts("script disk cache: ABI mismatch and damage fall back");
    failed |= test_sweep_removes_foreign_files();
    puts("script disk cache: sweep");
    failed |= test_no_store_and_captive_never_written();
    puts("script disk cache: no-store and captive");
    failed |= test_clears_remove_files();
    puts("script disk cache: clears");
    failed |= test_site_clear_reaches_queue_and_read_only_tier();
    puts("script disk cache: site clear, queued stores and read-only tier");
    failed |= test_ceiling_and_lru();
    failed |= test_sliced_restore_retries_cancel();
    puts("script disk cache: sliced restore retries cancellation");
    failed |= test_restore_io_failure_is_generation_bounded();
    puts("script disk cache: I/O failure reads once per generation");
    failed |= test_backup_index_survives_primary_refusal();
    puts("script disk cache: valid backup survives primary Budget refusal");
    puts("script disk cache: ceiling and LRU");
    failed |= test_oversized_record_stays_in_ram();
    puts("script disk cache: oversized record stays in RAM");
    failed |= test_modules_restore_after_restart();
    puts("script disk cache: modules");
    script_test_remove_tree(scratch_root);
    if (failed) {
        fprintf(stderr, "script disk cache: FAIL\n");
        return 1;
    }
    puts("script disk cache: PASS");
    return 0;
}
