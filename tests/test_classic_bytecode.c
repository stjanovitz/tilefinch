/* The in-memory classic-script bytecode table (BrowserScriptBytecodeTable,
   CLASSIC kind). Each runtime test drives the real external classic-script
   path (script_runtime_evaluate_external_classic_cached) in fresh realms
   over one BrowserSession, the way a revisit does. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/script_admission.h"
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
   their text, so it is about the size of the source: main() keeps script
   text in the heap, see there), and a summary naming `tag`. Caller
   frees. */
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

/* A queued store pins its compile's whole function tree in the realm heap
   until idle work stores it, and a busy page may give idle work no turn.
   Once the page's own data leaves less heap available than the store would
   need, the next safe point (a task boundary or a collection) gives the
   tree back instead of holding it until the page runs out of memory. A
   page with room keeps its queued store. */
static int test_queued_store_yields_to_page_heap(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script("app", 192u * KIB);
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
          && ran(&result, "app")
          && script_runtime_pending_bytecode_count(runtime) == 1);
    /* Plenty of room: a task boundary keeps the store. */
    CHECK(script_runtime_advance(runtime, 0u, 4u, &result)
          && script_runtime_pending_bytecode_count(runtime) == 1);
    /* What the idle store itself checks (pending_bytecode_heap_reserve). */
    size_t reserve = script_admission_work_reserve(
        strlen(source), SCRIPT_ADMISSION_STORE_HEAP_MULTIPLIER, SIZE_MAX);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, "globalThis.keep=[];", "about:fill", &result));
    for (unsigned chunk = 0; chunk < 2048u
         && script_runtime_heap_available(runtime) >= reserve; chunk++) {
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, "keep.push(new Array(4096).fill(1));", "about:fill",
            &result));
    }
    CHECK(script_runtime_heap_available(runtime) < reserve);
    CHECK(script_runtime_advance(runtime, 0u, 4u, &result)
          && script_runtime_pending_bytecode_count(runtime) == 0
          && result.external_script_bytecode_deferred_dropped == 1
          && result.external_script_bytecode_cache_stores == 0);
    script_runtime_destroy(runtime);
    free(source);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* A refusal the page caught is answered at the next safe point, as the
   advance boundary answers it: once. Collections after the refusal has been
   accounted keep queued stores, so the bytecode cache recovers when the
   page has heap again. */
static int test_queued_store_survives_an_old_refusal(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://site.test/after.js";
    char *source = make_script("after", 192u * KIB);
    CHECK(source != NULL);
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    /* One growth step, then filling the heap is refused. */
    script_runtime_enable_heap_growth(runtime, 8u * MIB + 512u * KIB, 0);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "try{var keep=[];for(;;)keep.push(new Array(4096).fill(1));}"
        "catch(e){}keep=null;",
        "about:fill", &result));
    ScriptHeapGrowth growth;
    script_runtime_heap_growth(runtime, &growth);
    CHECK(growth.refusals != 0);
    CHECK(script_runtime_advance(runtime, 0u, 4u, &result));
    (void) script_runtime_collect_and_trim(runtime);
    CHECK(script_runtime_evaluate_external_classic_cached(
              runtime, fixture.script, source, strlen(source), url, url,
              false, &result)
          && ran(&result, "after")
          && script_runtime_pending_bytecode_count(runtime) == 1);
    /* A collection with heap to spare keeps the store... */
    size_t collections = growth.collections;
    (void) script_runtime_collect_and_trim(runtime);
    script_runtime_heap_growth(runtime, &growth);
    CHECK(growth.collections > collections
          && script_runtime_pending_bytecode_count(runtime) == 1);
    /* ...for idle work to store. */
    CHECK(script_runtime_store_pending_bytecode(runtime, &result)
          && result.external_script_bytecode_cache_stores == 1
          && script_runtime_pending_bytecode_count(runtime) == 0);
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


/* ---- whole script bodies near the page's memory reserve ---- */

static const TilefinchRequestContext *script_context(const char *url)
{
    static TilefinchRequestContext context;
    context = (TilefinchRequestContext) {
        .target_url = url, .initiator_url = page_url,
        .top_level_url = page_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    return &context;
}

/* A response body of `length` bytes as the loader stores it: a shared body
   of exactly the response, which the cache entry then holds. Returns the
   body with the caller's reference (release it to leave the cache the sole
   holder), or NULL. A `destination` other than SCRIPT stores it as that
   kind of resource. */
static BrowserSharedBody *put_shared_body(BrowserSession *session,
                                          const char *url, size_t length,
                                          TilefinchRequestDestination destination)
{
    char *data = budget_malloc(session->budget, length + 1u);
    if (data == NULL) return NULL;
    memset(data, ';', length);
    data[length] = '\0';
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, (unsigned char *) data, length);
    if (body == NULL) {
        budget_free(session->budget, data);
        return NULL;
    }
    TilefinchRequestContext context = *script_context(url);
    context.destination = destination;
    TilefinchResourceGrant grant = {
        .destination = destination, .mode = context.mode,
        .credentials = context.credentials,
        .final_same_origin = true, .mime_validated = true
    };
    bool stored = destination == TILEFINCH_DESTINATION_SCRIPT
        ? browser_session_cache_put_http_shared_classic_script(
              session, url, body, "\"v1\"", NULL, "text/javascript",
              "public,max-age=3600", NULL, 1, &context, &grant)
        : browser_session_cache_put_http_shared_resource(
              session, url, body, "\"v1\"", NULL, "text/css",
              "public,max-age=3600", NULL, 1, &context, &grant);
    if (!stored) {
        browser_shared_body_release(body);
        return NULL;
    }
    return body;
}

static bool cached_script(BrowserSession *session, const char *url)
{
    const BrowserCacheEntry *entry = NULL;
    return browser_session_cache_match_classic_script(
               session, url, script_context(url), 1, &entry)
           != BROWSER_CACHE_MISS;
}

/* Only classic script bodies the cache alone holds go, least recently used
   first, as much as asked: never one a load still leases, a stylesheet, or
   a response stored without a shared body. */
static int test_cache_releases_sole_script_bodies(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 2u * MIB));
    static const char a[] = "https://site.test/a.js";
    static const char b[] = "https://site.test/b.js";
    static const char c[] = "https://site.test/c.js";
    static const char css[] = "https://site.test/site.css";
    static const char plain[] = "https://site.test/plain.js";
    BrowserSharedBody *body = put_shared_body(
        &session, a, 200u * KIB, TILEFINCH_DESTINATION_SCRIPT);
    CHECK(body != NULL);
    browser_shared_body_release(body);
    BrowserSharedBody *leased = put_shared_body(
        &session, b, 200u * KIB, TILEFINCH_DESTINATION_SCRIPT);
    CHECK(leased != NULL);
    body = put_shared_body(&session, c, 100u * KIB,
                           TILEFINCH_DESTINATION_SCRIPT);
    CHECK(body != NULL);
    browser_shared_body_release(body);
    body = put_shared_body(&session, css, 300u * KIB,
                           TILEFINCH_DESTINATION_STYLE);
    CHECK(body != NULL);
    browser_shared_body_release(body);
    CHECK(browser_session_cache_put_http(
        &session, plain, (const unsigned char *) "var p=1;", 8, "\"v1\"",
        NULL, "text/javascript", "public,max-age=3600", NULL, 1));

    /* One byte asked: the oldest sole body (a), all of it. */
    size_t before = budget_remaining(&budget);
    size_t freed = browser_session_cache_release_script_bodies(&session, 1);
    CHECK(freed >= 200u * KIB && budget_remaining(&budget) - before == freed
          && session.cache_script_body_releases == 1
          && !cached_script(&session, a) && cached_script(&session, b)
          && cached_script(&session, c));
    /* Everything: c goes; the leased b, the stylesheet and the plain
       response stay. */
    freed = browser_session_cache_release_script_bodies(&session, SIZE_MAX);
    CHECK(freed >= 100u * KIB && session.cache_script_body_releases == 2
          && !cached_script(&session, c) && cached_script(&session, b));
    const unsigned char *data = NULL;
    size_t length = 0;
    CHECK(browser_session_cache_get(&session, plain, &data, &length)
          && length == 8);
    const BrowserCacheEntry *entry = NULL;
    TilefinchRequestContext style = *script_context(css);
    style.destination = TILEFINCH_DESTINATION_STYLE;
    CHECK(browser_session_cache_match_resource(
              &session, css, &style, 1, &entry) != BROWSER_CACHE_MISS);
    /* Once the load lets go of b, b can go too. */
    browser_shared_body_release(leased);
    freed = browser_session_cache_release_script_bodies(&session, SIZE_MAX);
    CHECK(freed >= 200u * KIB && !cached_script(&session, b)
          && session.cache_script_body_releases == 3);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A realm whose page Budget is down to its reserve plus `spare`: the rest
   is held by a ballast allocation (returned; free it). Heap growth as the
   page lifecycle enables it, up to `ceiling`, with a `reserve` growth
   reserve. */
static void *leave_spare(ClassicFixture *fixture, ScriptRuntime *runtime,
                         size_t ceiling, size_t reserve, size_t spare)
{
    script_runtime_enable_heap_growth(runtime, ceiling, reserve);
    ScriptHeapGrowth growth;
    script_runtime_heap_growth(runtime, &growth);
    size_t live = script_runtime_heap_used(runtime);
    size_t granted = growth.limit > live ? growth.limit - live : 0;
    size_t keep = reserve + granted + spare;
    size_t remaining = budget_remaining(&fixture->budget);
    if (remaining <= keep) return NULL;
    return budget_malloc(&fixture->budget, remaining - keep);
}

/* Fills the realm until it is refused, then lets the garbage go; returns
   the number of 4096-element arrays it held. */
static long fill_realm(ScriptRuntime *runtime, ScriptResult *result)
{
    if (!script_runtime_evaluate_diagnostic(
            runtime,
            "var keep=[];try{for(;;)keep.push(new Array(4096).fill(1.5));}"
            "catch(e){}globalThis.pocSummary=String(keep.length);"
            "keep=null;",
            "about:fill", result)) return -1;
    return atol(result->summary);
}

/* The realm short of room to grow takes back the response cache's own
   script bodies first, exactly as much as it can then use; with room to
   spare it leaves them alone. */
static int test_growth_takes_back_cached_script_bodies(void)
{
    long held[2] = {0, 0};
    for (int with_body = 0; with_body < 2; with_body++) {
        ClassicFixture fixture;
        CHECK(fixture_open(&fixture));
        CHECK(browser_session_cache_set_maximum_bytes(
            &fixture.session, 4u * MIB));
        static const char url[] = "https://site.test/big.js";
        static const char css[] = "https://site.test/big.css";
        ScriptResult result;
        memset(&result, 0, sizeof(result));
        ScriptRuntime *runtime = script_runtime_create_configured(
            &fixture.document, &fixture.budget, 4u * MIB, 8000, page_url,
            &fixture.options, &result);
        CHECK(runtime != NULL);
        /* The stylesheet is not a script body: it stays either way. */
        BrowserSharedBody *body = put_shared_body(
            &fixture.session, css, 256u * KIB, TILEFINCH_DESTINATION_STYLE);
        CHECK(body != NULL);
        browser_shared_body_release(body);
        /* The control run keeps a load's lease on the same body: a body
           someone still reads never goes. */
        BrowserSharedBody *script = put_shared_body(
            &fixture.session, url, 1536u * KIB,
            TILEFINCH_DESTINATION_SCRIPT);
        CHECK(script != NULL);
        if (with_body) {
            browser_shared_body_release(script);
            script = NULL;
        }
        void *ballast = leave_spare(&fixture, runtime, 40u * MIB, 2u * MIB,
                                    1u * MIB);
        CHECK(ballast != NULL);
        /* Room to spare: a small task leaves the cache alone. */
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, "globalThis.small=new Array(1000).fill(0).length;",
            "about:small", &result));
        CHECK(result.script_body_cache_releases == 0);
        held[with_body] = fill_realm(runtime, &result);
        CHECK(held[with_body] > 0);
        if (with_body) {
            CHECK(result.script_body_cache_releases == 1
                  && result.script_body_cache_released_bytes
                         >= 1536u * KIB
                  && !cached_script(&fixture.session, url));
        } else {
            CHECK(result.script_body_cache_releases == 0
                  && cached_script(&fixture.session, url));
            browser_shared_body_release(script);
        }
        const BrowserCacheEntry *entry = NULL;
        TilefinchRequestContext style = *script_context(css);
        style.destination = TILEFINCH_DESTINATION_STYLE;
        CHECK(browser_session_cache_match_resource(
                  &fixture.session, css, &style, 1, &entry)
              != BROWSER_CACHE_MISS);
        script_runtime_destroy(runtime);
        budget_free(&fixture.budget, ballast);
        CHECK(fixture_close(&fixture));
    }
    /* The released 1.5 MiB bought the realm at least 1 MiB more of arrays
       (64 KiB each on a 64-bit host). */
    CHECK(held[1] >= held[0] + 16);
    return 0;
}


/* The owner of a running script's body, as the script loader is. */
typedef struct {
    Budget *budget;
    char *body;
    unsigned releases;
} TestSourceOwner;

static void test_source_release(void *opaque)
{
    TestSourceOwner *owner = opaque;
    budget_free(owner->budget, owner->body);
    owner->body = NULL;
    owner->releases++;
}

/* A classic script padded with a comment to `minimum` bytes, naming `tag`,
   that, run, fills its realm with up to globalThis.fillArrays arrays (none
   when unset) or until refused, and then lets the garbage go: the same
   bytes, and so the same bytecode record, under pressure or not. */
static char *make_filling_script(const char *tag, size_t minimum)
{
    char *functions = make_script(tag, 32u * KIB);
    if (functions == NULL) return NULL;
    static const char fill[] =
        "var keep=[];try{for(var n=0;n<(globalThis.fillArrays||0);n++)"
        "keep.push(new Array(4096).fill(1.5));}catch(e){}keep=null;\n/*";
    size_t base = strlen(functions);
    size_t length = base + sizeof(fill) - 1u + 2u;
    if (length < minimum) length = minimum;
    char *source = malloc(length + 1u);
    if (source == NULL) {
        free(functions);
        return NULL;
    }
    memcpy(source, fill, sizeof(fill) - 1u);
    size_t at = sizeof(fill) - 1u;
    memset(source + at, ' ', length - base - at - 2u);
    at = length - base - 2u;
    memcpy(source + at, "*/", 2u);
    memcpy(source + at + 2u, functions, base);
    source[length] = '\0';
    free(functions);
    return source;
}

/* One visit in a realm that grows to `ceiling`, its page down to its
   reserve plus `spare`: the external classic script `source` is evaluated
   from a body its owner offers to release (after `prelude`, when given),
   then idle work runs. */
static bool visit_pressed(ClassicFixture *fixture, const char *url,
                          const char *source, const char *prelude,
                          size_t ceiling, size_t spare,
                          TestSourceOwner *owner, bool *released,
                          ScriptResult *result)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 4u * MIB, 8000, page_url,
        &fixture->options, result);
    if (runtime == NULL) return false;
    size_t length = strlen(source);
    *owner = (TestSourceOwner) {
        .budget = &fixture->budget,
        .body = budget_malloc(&fixture->budget, length + 1u)
    };
    void *ballast = NULL;
    bool ok = owner->body != NULL
        && (prelude == NULL || script_runtime_evaluate_diagnostic(
                runtime, prelude, "about:prelude", result));
    if (ok) {
        memcpy(owner->body, source, length + 1u);
        ballast = leave_spare(fixture, runtime, ceiling, 2u * MIB, spare);
        ok = ballast != NULL;
    }
    if (ok) {
        script_runtime_offer_source_release(runtime, test_source_release,
                                            owner);
        ok = script_runtime_evaluate_external_classic_cached(
            runtime, fixture->script, owner->body, length, url, url, false,
            result);
        *released = script_runtime_withdraw_source_release(runtime);
        /* Withdrawn: a later evaluation is offered nothing. */
        ok = ok && !script_runtime_source_released(runtime);
        (void) script_runtime_collect_and_trim(runtime);
        run_idle_stores(runtime, result);
    }
    script_runtime_destroy(runtime);
    budget_free(&fixture->budget, ballast);
    budget_free(&fixture->budget, owner->body);
    owner->body = NULL;
    return ok;
}

/* A running script's body goes when its realm runs short, and only then;
   the script runs to completion either way. A store that happened serves
   the revisit, pressed or not; one the pressure dropped costs the revisit
   a compile, nothing else. */
static int test_running_script_body_and_revisits(void)
{
    ClassicFixture fixture;
    CHECK(fixture_open(&fixture));
    static const char url[] = "https://site.test/filling.js";
    static const char press[] = "globalThis.fillArrays=1e9;";
    char *source = make_filling_script("fill", 640u * KIB);
    CHECK(source != NULL);
    size_t length = strlen(source);
    TestSourceOwner owner;
    bool released = true;
    ScriptResult result;
    /* No pressure: kept, compiled and stored. */
    CHECK(visit_pressed(&fixture, url, source, NULL, 40u * MIB, 16u * MIB,
                        &owner, &released, &result)
          && ran(&result, "fill") && !released && owner.releases == 0
          && result.script_body_sources_released == 0
          && result.external_script_bytecode_cache_misses == 1
          && result.external_script_bytecode_cache_stores == 1);
    /* Stopped by the realm's ceiling with room in the page: kept, and the
       store serves this visit. */
    CHECK(visit_pressed(&fixture, url, source, press, 8u * MIB, 16u * MIB,
                        &owner, &released, &result)
          && ran(&result, "fill") && !released && owner.releases == 0
          && result.external_script_bytecode_cache_hits == 1);
    /* Short of room in the page: the body goes while the script runs, and
       the stored bytecode still serves it. */
    CHECK(visit_pressed(&fixture, url, source, press, 40u * MIB, 512u * KIB,
                        &owner, &released, &result)
          && ran(&result, "fill") && released && owner.releases == 1
          && result.script_body_sources_released == 1
          && result.script_body_sources_released_bytes == length
          && result.external_script_bytecode_cache_hits == 1
          && result.external_script_bytecode_cache_misses == 0);
    /* With no stored bytecode, the pressed visit compiles, the body goes,
       and the write it queued is dropped with the page short of heap... */
    browser_session_cache_clear(&fixture.session);
    CHECK(visit_pressed(&fixture, url, source, press, 40u * MIB, 512u * KIB,
                        &owner, &released, &result)
          && ran(&result, "fill") && released && owner.releases == 1
          && result.external_script_bytecode_cache_misses == 1
          && result.external_script_bytecode_cache_stores == 0
          && result.external_script_bytecode_deferred_dropped == 1);
    /* ...which costs the revisit a compile (and it stores)... */
    CHECK(visit_pressed(&fixture, url, source, NULL, 40u * MIB, 16u * MIB,
                        &owner, &released, &result)
          && ran(&result, "fill") && !released
          && result.external_script_bytecode_cache_misses == 1
          && result.external_script_bytecode_cache_stores == 1);
    /* ...and nothing else. */
    CHECK(visit_pressed(&fixture, url, source, NULL, 40u * MIB, 16u * MIB,
                        &owner, &released, &result)
          && ran(&result, "fill")
          && result.external_script_bytecode_cache_hits == 1);
    free(source);
    CHECK(fixture_close(&fixture));
    return 0;
}

int main(void)
{
    /* These ceilings are sized for bytecode that carries its script's text.
       A script of 8 KiB or more now keeps its text outside the heap
       (evaluation.inc, "Function source outside the heap") and its bytecode
       is about 0.4 of the source; test_function_source_store.c covers the
       table with that. */
    setenv("TILEFINCH_JS_SOURCE_STORE", "keep", 1);
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
    CHECK(test_queued_store_yields_to_page_heap() == 0);
    puts("classic bytecode queued store yields to the page heap: PASS");
    CHECK(test_queued_store_survives_an_old_refusal() == 0);
    puts("classic bytecode queued store survives an old refusal: PASS");
    CHECK(test_segment_ordinals_are_records() == 0);
    puts("classic bytecode segment records: PASS");
    CHECK(test_pressure_clear_and_teardown() == 0);
    puts("classic bytecode pressure, clear and teardown: PASS");
    CHECK(test_cache_releases_sole_script_bodies() == 0);
    puts("cached script bodies released, sole ones only: PASS");
    CHECK(test_growth_takes_back_cached_script_bodies() == 0);
    puts("realm growth takes back cached script bodies: PASS");
    CHECK(test_running_script_body_and_revisits() == 0);
    puts("running script body released; revisits recompile or hit: PASS");
    return 0;
}
