/* The in-memory classic-script bytecode table (BrowserScriptBytecodeTable,
   CLASSIC kind). Each runtime test drives the real external classic-script
   path (script_runtime_evaluate_external_classic_cached) in fresh realms
   over one BrowserSession, the way a revisit does. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"
#include "tilefinch/viewport.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/node.h>

#include "script_test_support.h"

#define MIB (1024u * 1024u)
#define KIB 1024u

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char page_url[] = "https://site.test/";

typedef ScriptPageFixture ClassicFixture;

/* A 640 KiB response cache: the PSP application's. */
static bool fixture_open(ClassicFixture *fixture)
{
    return script_page_fixture_open(
        fixture, "<script src=/app.js></script>", 640u * KIB);
}

static bool fixture_close(ClassicFixture *fixture)
{
    return script_page_fixture_close(fixture);
}

/* A classic script of about `minimum` bytes of functions large enough to
   be compiled lazily (as most of a real site's are; their bytecode keeps
   their text, so it is about the size of the source), and a summary naming
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
             "globalThis.pocSummary='RAN:%s:'+typeof f0;", tag);
    return source;
}

/* The HTTP response for `url`, as the script loader stores it before
   evaluating. */
static bool put_response(ClassicFixture *fixture, const char *url,
                         const char *source)
{
    return browser_session_cache_put_http(
        &fixture->session, url, (const unsigned char *) source,
        strlen(source), "\"v1\"", NULL, "text/javascript",
        "public,max-age=3600", NULL, 1);
}

/* The page's idle turns after its load: every store the load queued. */
static void run_idle_stores(ScriptRuntime *runtime, ScriptResult *result)
{
    for (unsigned turn = 0; turn < 1000u
         && script_runtime_store_pending_bytecode(runtime, result); turn++) {}
}

/* One visit: a fresh realm evaluates the external classic script `source`
   from `url`, as fetched (no-store or not), then idles (`idle`) or is left
   at once. */
static bool visit_idle(ClassicFixture *fixture, const char *url,
                       const char *source, bool no_store, bool idle,
                       ScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000, page_url,
        &fixture->options, result);
    if (runtime == NULL) return false;
    bool ok = script_runtime_evaluate_external_classic_cached(
        runtime, fixture->script, source, strlen(source), url, url,
        no_store, result);
    if (idle) run_idle_stores(runtime, result);
    script_runtime_destroy(runtime);
    return ok;
}

static bool visit(ClassicFixture *fixture, const char *url,
                  const char *source, bool no_store, ScriptResult *result)
{
    return visit_idle(fixture, url, source, no_store, true, result);
}

static bool ran(const ScriptResult *result, const char *tag)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "RAN:%s:function", tag);
    if (strcmp(result->summary, expected) == 0) return true;
    fprintf(stderr, "summary=%s (wanted %s) error=%s\n", result->summary,
            expected, result->error);
    return false;
}

/* The census finding: a revisit hit almost nothing because the bytecode
   lived on the HTTP response entry, and images and other scripts evicted
   that entry long before the page was visited again. The table keeps it. */
static int test_revisit_hits_after_response_eviction(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("app", 48u * KIB);
    CHECK(source != NULL && put_response(&fixture, url, source));
    ScriptResult first;
    CHECK(visit(&fixture, url, source, false, &first) && ran(&first, "app")
          && first.external_script_bytecode_cache_misses == 1
          && first.external_script_bytecode_cache_stores == 1);

    /* The rest of the page: 640 KiB of images push the script out. */
    static unsigned char image[160u * KIB];
    for (unsigned i = 0; i < 6; i++) {
        char image_url[64];
        snprintf(image_url, sizeof(image_url),
                 "https://site.test/image-%u.png", i);
        memset(image, (int) i, sizeof(image));
        CHECK(browser_session_cache_put_http(
            &fixture.session, image_url, image, sizeof(image), NULL, NULL,
            "image/png", "public,max-age=3600", NULL, 2));
    }
    /* The revisit fetches the script again and evaluates it. */
    CHECK(put_response(&fixture, url, source));
    ScriptResult second;
    CHECK(visit(&fixture, url, source, false, &second)
          && ran(&second, "app"));
    if (second.external_script_bytecode_cache_hits != 1) {
        fprintf(stderr, "revisit: hits=%zu misses=%zu\n",
                second.external_script_bytecode_cache_hits,
                second.external_script_bytecode_cache_misses);
    }
    CHECK(second.external_script_bytecode_cache_hits == 1
          && second.external_script_bytecode_cache_misses == 0
          && second.host_compile_attempts + 1 == first.host_compile_attempts);
    free(source);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Changed bytes at the same URL miss, compile, and replace the entry; a
   different top-level site is a different partition and shares nothing;
   a no-store response runs from source and keeps nothing. */
static int test_keying_and_no_store(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://cdn.test/lib.js";
    char *one = make_script("one", 24u * KIB);
    char *two = make_script("two", 24u * KIB);
    CHECK(one != NULL && two != NULL);
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    ScriptResult result;
    CHECK(visit(&fixture, url, one, false, &result) && ran(&result, "one")
          && result.external_script_bytecode_cache_stores == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 1);
    CHECK(visit(&fixture, url, one, false, &result) && ran(&result, "one")
          && result.external_script_bytecode_cache_hits == 1);

    CHECK(visit(&fixture, url, two, false, &result) && ran(&result, "two")
          && result.external_script_bytecode_cache_hits == 0
          && result.external_script_bytecode_cache_misses == 1
          && result.external_script_bytecode_cache_stores == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 1);
    CHECK(visit(&fixture, url, two, false, &result) && ran(&result, "two")
          && result.external_script_bytecode_cache_hits == 1);

    ScriptRuntimeOptions saved = fixture.options;
    fixture.options.top_level_url = "https://other.test/";
    CHECK(visit(&fixture, url, two, false, &result) && ran(&result, "two")
          && result.external_script_bytecode_cache_hits == 0
          && result.external_script_bytecode_cache_misses == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2);
    fixture.options = saved;

    static const char fresh_url[] = "https://site.test/private.js";
    CHECK(visit(&fixture, fresh_url, one, true, &result)
          && ran(&result, "one")
          && result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_cache_admission_skips == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2);
    CHECK(visit(&fixture, fresh_url, one, true, &result)
          && ran(&result, "one")
          && result.external_script_bytecode_cache_hits == 0);
    CHECK(browser_session_cache_control_no_store("public, no-store")
          && browser_session_cache_control_no_store("No-Store")
          && !browser_session_cache_control_no_store("no-storex, max-age=1")
          && !browser_session_cache_control_no_store("no-cache")
          && !browser_session_cache_control_no_store(NULL));

    /* data: and blob: scripts are not responses and never keyed. */
    CHECK(visit(&fixture, "blob:https://site.test/1", one, false, &result)
          && ran(&result, "one")
          && result.external_script_bytecode_cache_stores == 0);

    free(one);
    free(two);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Bytecode that does not restore is dropped; the script compiles from
   source with identical behaviour and its fresh bytecode replaces it. */
static int test_unrestorable_entry_falls_back(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("app", 8u * KIB);
    CHECK(source != NULL);
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    CHECK(tilefinch_url_site_key(page_url, partition, sizeof(partition)));
    BrowserScriptBytecodeKey key = {
        .module_name = url,
        .response_url = url,
        .partition_key = partition,
        .source = (const unsigned char *) source,
        .source_length = strlen(source),
        .compile_flags = 0
    };
    static const unsigned char damaged[256] = {0xff, 0x01, 0x02};
    uint32_t generation = browser_session_module_bytecode_generation(
        &fixture.session);
    CHECK(browser_session_script_bytecode_put(
        &fixture.session, BROWSER_SCRIPT_BYTECODE_CLASSIC, &key, generation,
        damaged, sizeof(damaged)));
    ScriptResult result;
    CHECK(visit(&fixture, url, source, false, &result) && ran(&result, "app")
          && result.external_script_bytecode_cache_restore_failures == 1
          && result.external_script_bytecode_cache_hits == 0
          && result.external_script_bytecode_cache_stores == 1);
    CHECK(visit(&fixture, url, source, false, &result) && ran(&result, "app")
          && result.external_script_bytecode_cache_hits == 1
          && result.external_script_bytecode_cache_restore_failures == 0);
    free(source);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The ceiling holds, entries a realm stored are not evicted to admit more
   of the same load (the next load hits them), and the next realm's stores
   may evict what it did not use. */
static int test_ceiling_and_realm_protection(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    browser_session_script_bytecode_set_limit(&fixture.session, classic,
                                              120u * KIB);
    char *a = make_script("a", 40u * KIB);
    char *b = make_script("b", 40u * KIB);
    char *c = make_script("c", 40u * KIB);
    CHECK(a != NULL && b != NULL && c != NULL);
    static const char *urls[3] = {
        "https://site.test/a.js", "https://site.test/b.js",
        "https://site.test/c.js"
    };
    const char *sources[3] = {a, b, c};
    const char *tags[3] = {"a", "b", "c"};
    ScriptResult result[3];
    /* One realm loads all three: two fit, the third is skipped. */
    memset(result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result[0]);
    CHECK(runtime != NULL);
    for (int i = 0; i < 3; i++) {
        CHECK(script_runtime_evaluate_external_classic_cached(
                  runtime, fixture.script, sources[i], strlen(sources[i]),
                  urls[i], urls[i], false, &result[i])
              && ran(&result[i], tags[i]));
    }
    run_idle_stores(runtime, &result[2]);
    script_runtime_destroy(runtime);
    if (result[2].external_script_bytecode_cache_stores != 2) {
        fprintf(stderr, "ceiling: stores=%zu skips=%zu stored=%zu bytes=%zu\n",
                result[2].external_script_bytecode_cache_stores,
                result[2].external_script_bytecode_cache_admission_skips,
                result[2].external_script_bytecode_cache_stored_bytes,
                browser_session_script_bytecode_bytes(&fixture.session,
                                                      classic));
    }
    CHECK(result[2].external_script_bytecode_cache_stores == 2
          && result[2].external_script_bytecode_cache_admission_skips == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2
          && browser_session_script_bytecode_bytes(
                 &fixture.session, classic) <= 120u * KIB);
    /* The next visit hits the two and still skips the third. */
    memset(result, 0, sizeof(result));
    runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result[0]);
    CHECK(runtime != NULL);
    for (int i = 0; i < 3; i++) {
        CHECK(script_runtime_evaluate_external_classic_cached(
                  runtime, fixture.script, sources[i], strlen(sources[i]),
                  urls[i], urls[i], false, &result[i])
              && ran(&result[i], tags[i]));
    }
    run_idle_stores(runtime, &result[2]);
    script_runtime_destroy(runtime);
    CHECK(result[2].external_script_bytecode_cache_hits == 2
          && result[2].external_script_bytecode_cache_misses == 1
          && fixture.session.classic_bytecode_evictions == 0);
    /* A realm that loads only c evicts the least recently used entry. */
    CHECK(visit(&fixture, urls[2], c, false, &result[0])
          && ran(&result[0], "c")
          && result[0].external_script_bytecode_cache_stores == 1
          && fixture.session.classic_bytecode_evictions == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2);
    /* Zero disables the table and releases everything. */
    browser_session_script_bytecode_set_limit(&fixture.session, classic, 0);
    CHECK(browser_session_script_bytecode_entries(
              &fixture.session, classic) == 0
          && fixture.session.classic_bytecode == NULL);
    CHECK(visit(&fixture, urls[0], a, false, &result[0])
          && ran(&result[0], "a")
          && result[0].external_script_bytecode_cache_stores == 0);
    free(a);
    free(b);
    free(c);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The profiles' ceilings (bytes bind, not entries: see browser_engine.h),
   and the largest script whose compile is queued for the table: an 800 KiB
   bundle runs from source every visit while a 512 KiB one is stored. */
static int test_profile_ceilings_and_entry_size(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    CHECK(config.classic_bytecode_cache_limit == 3u * MIB);
    CHECK(browser_config_apply_psp_memory_profile(
              &config, BROWSER_PSP_MEMORY_REALISTIC)
          && config.classic_bytecode_cache_limit == 3u * MIB);
    CHECK(browser_config_apply_psp_memory_profile(
              &config, BROWSER_PSP_MEMORY_STRICT)
          && config.classic_bytecode_cache_limit == 1536u * KIB);

    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    browser_session_script_bytecode_set_limit(&fixture.session, classic,
                                              8u * MIB);
    char *large = make_script("large", 800u * KIB);
    char *medium = make_script("medium", 512u * KIB);
    CHECK(large != NULL && medium != NULL);
    /* The device's script policy admits the 800 KiB bundle itself. */
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
        &fixture.options.execution_policy));
    /* A realm with room for the store's heap reserve, so only the size
       decides. */
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 32u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    bool large_ran = script_runtime_evaluate_external_classic_cached(
        runtime, fixture.script, large, strlen(large),
        "https://site.test/large.js", "https://site.test/large.js", false,
        &result);
    run_idle_stores(runtime, &result);
    script_runtime_destroy(runtime);
    if (result.external_script_bytecode_cache_admission_skips != 1)
        fprintf(stderr, "large: stores=%zu skips=%zu\n",
                result.external_script_bytecode_cache_stores,
                result.external_script_bytecode_cache_admission_skips);
    CHECK(large_ran
          && ran(&result, "large")
          && result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_cache_admission_skips == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 0);
    CHECK(visit(&fixture, "https://site.test/medium.js", medium, false,
                &result)
          && ran(&result, "medium")
          && result.external_script_bytecode_cache_stores == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 1);
    free(large);
    free(medium);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The store is idle work, not part of the load: evaluating a script only
   queues it (no table entry, no serialization) and the page's idle turns
   store it. A page left before any idle turn stores what fits the teardown
   budget and drops the rest (the next visit compiles that again and stores
   it then). A cache clear drops what is queued before it. Every queued
   byte is returned either way. */
static int test_store_waits_for_idle(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("app", 32u * KIB);
    CHECK(source != NULL);
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_external_classic_cached(
              runtime, fixture.script, source, strlen(source), url, url,
              false, &result)
          && ran(&result, "app"));
    /* Loaded: queued, nothing serialized or stored yet. */
    CHECK(result.external_script_bytecode_deferred == 1
          && result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_cache_stored_bytes == 0
          && script_runtime_pending_bytecode_count(runtime) == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 0);
    /* One idle turn stores it; the next has nothing to do. */
    CHECK(script_runtime_store_pending_bytecode(runtime, &result)
          && !script_runtime_store_pending_bytecode(runtime, &result)
          && result.external_script_bytecode_cache_stores == 1
          && result.external_script_bytecode_cache_stored_bytes != 0
          && script_runtime_pending_bytecode_count(runtime) == 0
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 1);
    script_runtime_destroy(runtime);
    CHECK(visit(&fixture, url, source, false, &result) && ran(&result, "app")
          && result.external_script_bytecode_cache_hits == 1
          && result.external_script_bytecode_deferred == 0);

    /* Left before idling: teardown stores what fits its budget... */
    static const char other[] = "https://site.test/other.js";
    CHECK(visit_idle(&fixture, other, source, false, false, &result)
          && ran(&result, "app")
          && result.external_script_bytecode_deferred == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2);
    CHECK(visit(&fixture, other, source, false, &result)
          && ran(&result, "app")
          && result.external_script_bytecode_cache_hits == 1);
    /* ...and drops the rest: the next visit compiles and stores it. */
    char *large = make_script("big", SCRIPT_BYTECODE_TEARDOWN_FLUSH_BYTES
                                         + 16u * KIB);
    CHECK(large != NULL);
    static const char big[] = "https://site.test/big.js";
    CHECK(visit_idle(&fixture, big, large, false, false, &result)
          && ran(&result, "big")
          && result.external_script_bytecode_deferred == 1
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 2);
    CHECK(visit(&fixture, big, large, false, &result) && ran(&result, "big")
          && result.external_script_bytecode_cache_misses == 1
          && result.external_script_bytecode_cache_stores == 1);
    free(large);

    /* Queued before a cache clear: dropped by the next idle turn. */
    static const char third[] = "https://site.test/third.js";
    memset(&result, 0, sizeof(result));
    runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_external_classic_cached(
              runtime, fixture.script, source, strlen(source), third, third,
              false, &result)
          && script_runtime_pending_bytecode_count(runtime) == 1);
    browser_session_cache_clear(&fixture.session);
    CHECK(script_runtime_store_pending_bytecode(runtime, &result)
          && result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_deferred_dropped == 1
          && script_runtime_pending_bytecode_count(runtime) == 0
          && browser_session_script_bytecode_entries(
                 &fixture.session, classic) == 0);
    script_runtime_destroy(runtime);
    free(source);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Segment ordinals are distinct records of one URL; the session API alone,
   without a realm. */
static int test_segment_ordinals_are_records(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB));
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    static const char one[] = "mw.loader.implement('a',function(){});";
    static const char two[] = "mw.loader.implement('b',function(){});";
    static const unsigned char bytecode[512] = {1};
    BrowserScriptBytecodeKey keys[2];
    for (int i = 0; i < 2; i++) {
        keys[i] = (BrowserScriptBytecodeKey) {
            .module_name = "https://w.test/load.php",
            .response_url = "https://w.test/load.php",
            .partition_key = "https://w.test",
            .source = (const unsigned char *) (i == 0 ? one : two),
            .source_length = sizeof(one) - 1u,
            .ordinal = (uint32_t) i + 1u
        };
    }
    uint32_t generation = browser_session_module_bytecode_generation(
        &session);
    CHECK(browser_session_script_bytecode_put(
              &session, classic, &keys[0], generation, bytecode,
              sizeof(bytecode))
          && browser_session_script_bytecode_put(
              &session, classic, &keys[1], generation, bytecode,
              sizeof(bytecode))
          && browser_session_script_bytecode_entries(&session, classic) == 2
          && browser_session_module_bytecode_entries(&session) == 0);
    for (int i = 0; i < 2; i++) {
        BrowserScriptBytecodeKey probe = keys[i];
        probe.digest_ready = false;
        BrowserSharedBody *hit = browser_session_script_bytecode_acquire(
            &session, classic, &probe, generation);
        CHECK(hit != NULL);
        browser_shared_body_release(hit);
    }
    /* Segment 2's bytes under ordinal 1: a changed record, not a hit. */
    BrowserScriptBytecodeKey swapped = keys[1];
    swapped.ordinal = 1;
    swapped.digest_ready = false;
    CHECK(browser_session_script_bytecode_acquire(
              &session, classic, &swapped, generation) == NULL);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static size_t reclaim_script_bytecode(void *opaque, size_t needed)
{
    return browser_session_script_bytecode_reclaim(opaque, needed);
}

/* Bytecode gives way under memory pressure: with the engine's reclaim hook
   an allocation the Budget could not otherwise admit evicts classic (and
   module) entries instead of failing, and clear and teardown return every
   byte. */
static int test_pressure_clear_and_teardown(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB));
    size_t baseline = budget.current;
    BrowserScriptBytecodeKind classic = BROWSER_SCRIPT_BYTECODE_CLASSIC;
    static unsigned char bytecode[320u * KIB];
    static const char source_a[] = "var a=1;";
    static const char source_b[] = "var b=2;";
    BrowserScriptBytecodeKey a = {
        .module_name = "https://s.test/a.js",
        .response_url = "https://s.test/a.js",
        .partition_key = "https://s.test",
        .source = (const unsigned char *) source_a,
        .source_length = sizeof(source_a) - 1u
    };
    BrowserScriptBytecodeKey b = a;
    b.module_name = b.response_url = "https://s.test/b.js";
    b.source = (const unsigned char *) source_b;
    BrowserScriptBytecodeKey m = a;
    m.module_name = m.response_url = "https://s.test/m.js";
    browser_session_module_bytecode_set_limit(&session, 512u * KIB);
    uint32_t generation = browser_session_module_bytecode_generation(
        &session);
    CHECK(browser_session_script_bytecode_put(
              &session, classic, &a, generation, bytecode, sizeof(bytecode))
          && browser_session_script_bytecode_put(
              &session, classic, &b, generation, bytecode, sizeof(bytecode))
          && browser_session_module_bytecode_put(
              &session, &m, generation, bytecode, sizeof(bytecode)));
    size_t room = budget_remaining(&budget);
    size_t request = room + 200u * KIB;
    CHECK(budget_malloc(&budget, request) == NULL);
    budget_set_reclaim_hook(&budget, reclaim_script_bytecode, &session);
    void *allocation = budget_malloc(&budget, request);
    /* One entry covers the shortfall; it comes from the table holding more
       bytes (classic: two entries against the module table's one), and is
       that table's least recently used. */
    CHECK(allocation != NULL
          && browser_session_script_bytecode_entries(&session, classic) == 1
          && browser_session_module_bytecode_entries(&session) == 1
          && budget.reclaim_calls == 1);
    BrowserScriptBytecodeKey probe = b;
    BrowserSharedBody *kept = browser_session_script_bytecode_acquire(
        &session, classic, &probe, generation);
    CHECK(kept != NULL);
    browser_shared_body_release(kept);
    budget_free(&budget, allocation);
    budget_set_reclaim_hook(&budget, NULL, NULL);

    CHECK(browser_session_script_bytecode_put(
        &session, classic, &a, generation, bytecode, sizeof(bytecode)));
    browser_session_cache_clear(&session);
    CHECK(browser_session_script_bytecode_entries(&session, classic) == 0
          && browser_session_script_bytecode_bytes(&session, classic) == 0
          && browser_session_module_bytecode_entries(&session) == 0
          && budget.current == baseline);
    CHECK(browser_session_script_bytecode_put(
        &session, classic, &b, generation, bytecode, sizeof(bytecode)));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

int main(void)
{
    CHECK(test_revisit_hits_after_response_eviction() == 0);
    puts("classic bytecode survives response eviction: PASS");
    CHECK(test_keying_and_no_store() == 0);
    puts("classic bytecode keying and no-store: PASS");
    CHECK(test_unrestorable_entry_falls_back() == 0);
    puts("classic bytecode restore failure: PASS");
    CHECK(test_ceiling_and_realm_protection() == 0);
    puts("classic bytecode ceiling: PASS");
    CHECK(test_profile_ceilings_and_entry_size() == 0);
    puts("classic bytecode ceilings and entry size: PASS");
    CHECK(test_store_waits_for_idle() == 0);
    puts("classic bytecode stores at idle: PASS");
    CHECK(test_segment_ordinals_are_records() == 0);
    puts("classic bytecode segment records: PASS");
    CHECK(test_pressure_clear_and_teardown() == 0);
    puts("classic bytecode pressure, clear and teardown: PASS");
    return 0;
}
