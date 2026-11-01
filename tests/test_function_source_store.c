/* Function source outside the heap (evaluation.inc, "Function source
   outside the heap"; js_source_store.h): a classic page script of at least
   8 KiB keeps its text in the realm's function-source store, not the
   QuickJS heap. Through the real external classic-script path:
   Function.prototype.toString() and stack positions are unchanged, a
   revisit restored from the classic bytecode table reads the text the
   revisit registers, the threshold holds, a store under pressure drops
   only text no lazily compiled function needs (toString() then answers
   with the "[native code]" form), and worker realms are unaffected. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/script_admission.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"
#include "../src/js_source_store.h"
#include "../src/script_text_lz.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A classic script: `functions` (author text whose toString() the summary
   checks, one per line, each named f<i>), padded with a comment to at
   least `minimum` bytes. The summary is "ok" when every function's
   toString() equals its text, the given lazily compiled one still runs,
   and a thrown error's stack names this script at `line`; else what
   differed. Caller frees. */
typedef struct {
    const char *code;     /* the definition */
    const char *text;     /* what toString() returns, if not `code` */
} SourceForm;

static char *make_script(const SourceForm *functions, size_t count,
                         size_t minimum, const char *url)
{
    size_t capacity = minimum + 8192u;
    for (size_t i = 0; i < count; i++)
        capacity += 3u * strlen(functions[i].code);
    char *source = malloc(capacity);
    if (source == NULL) return NULL;
    size_t length = 0;
    /* first line: the thrower, so its position is known */
    length += (size_t) snprintf(source + length, capacity - length,
        "function thrower() { null.boom }\n");
    for (size_t i = 0; i < count; i++)
        length += (size_t) snprintf(source + length, capacity - length,
                                    "%s\n", functions[i].code);
    length += (size_t) snprintf(source + length, capacity - length,
        "(function () {\n"
        "  var expect = [");
    for (size_t i = 0; i < count; i++) {
        length += (size_t) snprintf(source + length, capacity - length,
                                    "%s'", i ? "," : "");
        const char *text = functions[i].text != NULL ? functions[i].text
                                                     : functions[i].code;
        for (const char *c = text; *c; c++) {
            if (*c == '\'' || *c == '\\')
                source[length++] = '\\';
            source[length++] = *c;
        }
        source[length++] = '\'';
    }
    length += (size_t) snprintf(source + length, capacity - length,
        "];\n"
        "  var bad = [];\n"
        "  for (var i = 0; i < expect.length; i++) {\n"
        "    var f = globalThis['f' + i];\n"
        "    if (String(f) !== expect[i]) bad.push(i + ':' + String(f).slice(0, 40));\n"
        "  }\n"
        "  var stack = '';\n"
        "  try { thrower() } catch (e) { stack = e.stack }\n"
        "  if (stack.indexOf('%s:1:') < 0) bad.push('stack ' + stack);\n"
        "  globalThis.pocSummary = bad.length ? bad.join('|') : 'ok';\n"
        "})();\n", url);
    length += (size_t) snprintf(source + length, capacity - length, "/*");
    while (length + 2u < minimum && length + 3u < capacity)
        source[length++] = '.';
    length += (size_t) snprintf(source + length, capacity - length, "*/\n");
    return source;
}

/* Author functions in many forms; f6 is long enough to compile lazily and
   is called by a later probe. */
static const SourceForm forms[] = {
    {"function f0(a, b) { return a + b }", NULL},
    {"var f1 = (p, q) => { return p - q };", "(p, q) => { return p - q }"},
    {"async function f2() { await 1; return 'h\xc3\xa9llo \xe2\x9c\x93' }",
     NULL},
    {"function* f3() { yield 1 }", NULL},
    {"var f4 = class C extends Object { constructor(v) { super(); this.v = v }"
        " get g() { return this.v } static s() {} };",
     "class C extends Object { constructor(v) { super(); this.v = v }"
        " get g() { return this.v } static s() {} }"},
    {"var f5 = { m(x) { return x } }.m;", "m(x) { return x }"},
    {"function f6(n) { /* long enough to be compiled lazily ............"
        "..................................................................."
        " */ var inner = function (k) { return k * n }; return inner(2) }",
     NULL},
};
#define FORM_COUNT (sizeof(forms) / sizeof(forms[0]))

typedef ScriptPageFixture SourceFixture;

static bool put_response(SourceFixture *fixture, const char *url,
                         const char *source)
{
    return browser_session_cache_put_http(
        &fixture->session, url, (const unsigned char *) source,
        strlen(source), "\"v1\"", NULL, "text/javascript",
        "public,max-age=3600", NULL, 1);
}

static void run_idle_stores(ScriptRuntime *runtime, ScriptResult *result)
{
    for (unsigned turn = 0; turn < 1000u
         && script_runtime_store_pending_bytecode(runtime, result); turn++) {}
}

/* Evaluates `source` (an external classic script at `url`) in a fresh
   realm, then `probe` (a diagnostic script, may be NULL) whose summary is
   returned in *probe_result; *stats gets the realm's store statistics
   before it is destroyed. */
static bool visit(SourceFixture *fixture, const char *url, const char *source,
                  const char *probe, ScriptResult *result,
                  ScriptResult *probe_result, ScriptSourceStoreStats *stats,
                  size_t relieve)
{
    memset(result, 0, sizeof(*result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000, page_url,
        &fixture->options, result);
    if (runtime == NULL) return false;
    /* The body the loader evaluates from: released (here overwritten)
       while the script runs or after it, as the loader may
       (script_runtime_offer_source_release). Every later read must come
       from the store's own copy. */
    size_t length = strlen(source);
    char *body = malloc(length + 1);
    if (body == NULL) {
        script_runtime_destroy(runtime);
        return false;
    }
    memcpy(body, source, length + 1);
    bool ok = script_runtime_evaluate_external_classic_cached(
        runtime, fixture->script, body, length, url, url, false, result);
    memset(body, '#', length);
    free(body);
    if (relieve != 0) {
        /* refused growth: compress at the safe point, then free */
        js_rt_source_store_safe_point(runtime, true);
        (void) js_rt_source_store_relieve(runtime, relieve);
    }
    if (ok && probe != NULL) {
        memset(probe_result, 0, sizeof(*probe_result));
        ok = script_runtime_evaluate_diagnostic(runtime, probe, "probe",
                                                probe_result);
    }
    run_idle_stores(runtime, result);
    script_source_store_stats(runtime->source_store, stats);
    script_runtime_destroy(runtime);
    return ok;
}

static bool summary_is(const ScriptResult *result, const char *expected)
{
    if (strcmp(result->summary, expected) == 0) return true;
    fprintf(stderr, "summary=%s (wanted %s) error=%s\n", result->summary,
            expected, result->error);
    return false;
}

/* toString() and stacks on the first visit, then on a revisit restored
   from the classic bytecode table; the lazily compiled f6 runs in both. */
static int test_tostring_and_revisit(void)
{
    SourceFixture fixture;
    CHECK(script_page_fixture_open(
        &fixture, "<script src=/app.js></script>", 640u * KIB));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script(forms, FORM_COUNT, 48u * KIB, url);
    CHECK(source != NULL && put_response(&fixture, url, source));
    static const char probe[] =
        "pocSummary = [f6(3), String(f6).length > 100,"
        " String(Math.max).indexOf('[native code]') > 0].join(',')";
    ScriptResult first, first_probe;
    ScriptSourceStoreStats stats;
    CHECK(visit(&fixture, url, source, probe, &first, &first_probe, &stats,
                0));
    CHECK(summary_is(&first, "ok"));
    CHECK(summary_is(&first_probe, "6,true,true"));
    CHECK(first.external_script_bytecode_cache_misses == 1
          && first.external_script_bytecode_cache_stores == 1);
    if (!(stats.added == 1 && stats.added_bytes == strlen(source)
          && stats.reads >= FORM_COUNT && stats.read_failures == 0))
        fprintf(stderr, "first visit: added=%llu bytes=%llu/%zu reads=%llu "
                "failures=%llu\n", (unsigned long long) stats.added,
                (unsigned long long) stats.added_bytes, strlen(source),
                (unsigned long long) stats.reads,
                (unsigned long long) stats.read_failures);
    CHECK(stats.added == 1 && stats.added_bytes == strlen(source)
          && stats.reads >= FORM_COUNT && stats.read_failures == 0);

    ScriptResult second, second_probe;
    CHECK(put_response(&fixture, url, source));
    CHECK(visit(&fixture, url, source, probe, &second, &second_probe, &stats,
                0));
    if (second.external_script_bytecode_cache_hits != 1)
        fprintf(stderr, "revisit: hits=%zu misses=%zu failures=%zu\n",
                second.external_script_bytecode_cache_hits,
                second.external_script_bytecode_cache_misses,
                second.external_script_bytecode_cache_restore_failures);
    CHECK(second.external_script_bytecode_cache_hits == 1
          && second.external_script_bytecode_cache_restore_failures == 0
          && second.host_compile_attempts + 1 == first.host_compile_attempts);
    /* the restored script's own check, then the probe's */
    CHECK(summary_is(&second, "ok"));
    CHECK(summary_is(&second_probe, "6,true,true"));
    CHECK(stats.added == 1 && stats.read_failures == 0
          && stats.reads >= FORM_COUNT);
    free(source);
    CHECK(script_page_fixture_close(&fixture));
    return 0;
}

/* The summary of the script's own self-check on its first visit. */
static int check_self_test(size_t minimum, bool stored)
{
    SourceFixture fixture;
    CHECK(script_page_fixture_open(
        &fixture, "<script src=/app.js></script>", 640u * KIB));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script(forms, FORM_COUNT, minimum, url);
    CHECK(source != NULL && put_response(&fixture, url, source));
    ScriptResult result, unused;
    ScriptSourceStoreStats stats;
    CHECK(visit(&fixture, url, source, NULL, &result, &unused, &stats, 0));
    CHECK(summary_is(&result, "ok"));
    if (stored != (stats.added == 1))
        fprintf(stderr, "threshold: %zu bytes added=%llu\n", strlen(source),
                (unsigned long long) stats.added);
    CHECK(stored ? stats.added == 1 && stats.reads >= FORM_COUNT
                 : stats.added == 0);
    free(source);
    CHECK(script_page_fixture_close(&fixture));
    return 0;
}

static int test_threshold(void)
{
    CHECK(check_self_test(4u * KIB, false) == 0);   /* text in the heap */
    CHECK(check_self_test(8u * KIB + 1u, true) == 0);
    CHECK(check_self_test(300u * KIB, true) == 0);  /* many LZ blocks */
    return 0;
}

/* A store under pressure compresses, then drops text no uncompiled lazy
   function needs. Dropped text: toString() answers with the native form,
   the functions and their stack positions are unaffected. Text a lazily
   compiled function still needs is kept, and that function compiles. */
static int test_pressure_fallback(void)
{
    SourceFixture fixture;
    CHECK(script_page_fixture_open(
        &fixture, "<script src=/app.js></script>", 640u * KIB));
    static const char url[] = "https://site.test/app.js";
    /* small functions only: compiled eagerly, nothing requires the text */
    static const SourceForm small[] = {
        {"function f0(a) { return a + 1 }", NULL},
        {"var f1 = x => x * 2;", "x => x * 2"},
    };
    char *source = make_script(small, 2, 24u * KIB, url);
    CHECK(source != NULL && put_response(&fixture, url, source));
    static const char probe[] =
        "var t = ''; try { thrower() } catch (e) { t = e.stack }"
        "pocSummary = [f0(1), f1(2), String(f0), String(f1),"
        " t.indexOf('app.js:1:') >= 0].join('|')";
    ScriptResult result, probe_result;
    ScriptSourceStoreStats stats;
    CHECK(visit(&fixture, url, source, probe, &result, &probe_result, &stats,
                SIZE_MAX));
    CHECK(summary_is(&result, "ok"));
    CHECK(summary_is(&probe_result,
        "2|4|function f0() {\n    [native code]\n}|"
        "function f1() {\n    [native code]\n}|true"));
    CHECK(stats.evicted == 1 && stats.entries == 0
          && stats.read_failures == 2);
    free(source);

    /* f6 is lazy and uncalled: its text is required and stays */
    source = make_script(forms, FORM_COUNT, 24u * KIB, url);
    CHECK(source != NULL && put_response(&fixture, url, source));
    static const char lazy_probe[] =
        "pocSummary = [f6(3), String(f0) === 'function f0(a, b) { return a + b }'"
        "].join('|')";
    CHECK(visit(&fixture, url, source, lazy_probe, &result, &probe_result,
                &stats, SIZE_MAX));
    CHECK(summary_is(&result, "ok"));
    CHECK(summary_is(&probe_result, "6|true"));
    CHECK(stats.evicted == 0 && stats.entries == 1
          && stats.compressed_blocks >= 1 && stats.read_failures == 0);
    free(source);
    CHECK(script_page_fixture_close(&fixture));
    return 0;
}

/* A worker realm shares the QuickJS runtime: its own scripts keep their
   text in the heap as before, and the page's stored text still answers. */
static int test_worker_realm(void)
{
    SourceFixture fixture;
    CHECK(script_page_fixture_open(
        &fixture, "<script src=/app.js></script>", 640u * KIB));
    static const char url[] = "https://site.test/app.js";
    char *source = make_script(forms, FORM_COUNT, 16u * KIB, url);
    CHECK(source != NULL && put_response(&fixture, url, source));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 8u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_external_classic_cached(
              runtime, fixture.script, source, strlen(source), url, url,
              false, &result)
          && summary_is(&result, "ok"));
    ScriptResult probe;
    memset(&probe, 0, sizeof(probe));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(function () {"
        " var body = 'function w(a) { return a * 7 }' + '/*' + 'x'.repeat(9000)"
        "   + '*/ postMessage(String(w) + \"|\" + w(6));';"
        " var worker = new Worker(URL.createObjectURL(new Blob([body])));"
        " worker.onmessage = function (e) {"
        "   pocSummary = e.data + '|' + (String(f0) === "
        "     'function f0(a, b) { return a + b }');"
        "   worker.terminate() };"
        " worker.onerror = function (e) { pocSummary = 'error:' + e.message };"
        " pocSummary = 'waiting' })()", "probe", &probe));
    for (unsigned tick = 0; tick < 64; tick++) {
        CHECK(script_runtime_advance(runtime, 1, 512, &probe));
        if (strcmp(probe.summary, "waiting") != 0) break;
    }
    CHECK(summary_is(&probe, "function w(a) { return a * 7 }|42|true"));
    script_runtime_destroy(runtime);
    free(source);
    CHECK(script_page_fixture_close(&fixture));
    return 0;
}

/* A function too large to compile at the realm's wall, called by eight
   queued callbacks: the first refusal ends the microtask checkpoint
   (event_loop.inc) instead of every queued callback retrying the same
   refused compile in the same turn; the rest run on later turns and
   succeed once the page has released memory. The callbacks are promise
   reactions that let the uncatchable error out, reactions whose Promise
   executor turns it into a rejection and so return normally, and event
   listeners, which run as checkpoint continuations, doing the same. */
enum { LAZY_REACTION, LAZY_REACTION_EXECUTOR, LAZY_LISTENER_EXECUTOR };

static int lazy_memory_failure_case(int trigger)
{
    SourceFixture fixture;
    CHECK(script_page_fixture_open(
        &fixture, "<script src=/app.js></script>", 640u * KIB));
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 4u * MIB, 8000, page_url,
        &fixture.options, &result);
    CHECK(runtime != NULL);
    /* big() is a lazy function of a page script: its first call needs a
       768 KiB compile buffer, refused while the hoard leaves only small
       room. */
    static const char url[] = "https://site.test/app.js";
    static const char head[] = "function big() { /*";
    static const char tail[] = "*/ return 1 }\n";
    size_t padding = 768u * KIB;
    size_t length = sizeof(head) - 1 + padding + sizeof(tail) - 1;
    char *source = malloc(length + 1);
    CHECK(source != NULL);
    memcpy(source, head, sizeof(head) - 1);
    memset(source + sizeof(head) - 1, 'x', padding);
    memcpy(source + sizeof(head) - 1 + padding, tail, sizeof(tail));
    CHECK(script_runtime_evaluate_external_classic_cached(
        runtime, fixture.script, source, length, url, url, false, &result));
    free(source);
    static const char *const setups[] = {
        "for (var i = 0; i < 8; i++)"
        "  Promise.resolve().then(function () { ran++; big(); done++ });",
        "for (var i = 0; i < 8; i++)"
        "  Promise.resolve().then(function () {"
        "    ran++; new Promise(function () { big(); done++ }) });",
        "var channel = new MessageChannel();"
        "for (var i = 0; i < 8; i++)"
        "  channel.port2.addEventListener('message', function () {"
        "    ran++; new Promise(function () { big(); done++ }) });"
        "channel.port2.start(); channel.port1.postMessage(1);",
    };
    char setup[1024];
    snprintf(setup, sizeof(setup),
        "globalThis.ran = 0; globalThis.done = 0;"
        "globalThis.hoard = [];"
        "try { for (;;) hoard.push(new ArrayBuffer(65536)) } catch (e) {}"
        "hoard.length -= 4; %s pocSummary = 'queued'", setups[trigger]);
    ScriptResult probe;
    memset(&probe, 0, sizeof(probe));
    CHECK(script_runtime_evaluate_diagnostic(runtime, setup, "setup",
                                             &probe));
    CHECK(summary_is(&probe, "queued"));
    JSLazyFunctionStats lazy;
    JS_GetLazyFunctionStats(runtime->runtime, &lazy);
    /* the message is delivered by a later task, its listeners resumed one
       by one as checkpoint continuations */
    for (unsigned tick = 0; trigger == LAZY_LISTENER_EXECUTOR
         && lazy.memory_failures == 0 && tick < 32; tick++) {
        CHECK(script_runtime_advance(runtime, 1, 64, &probe));
        JS_GetLazyFunctionStats(runtime->runtime, &lazy);
    }
    /* one refused compile in the checkpoint, not eight; a delivered
       message also had its first listener refused in the task itself */
    uint64_t expected = trigger == LAZY_LISTENER_EXECUTOR ? 2u : 1u;
    if (lazy.deferred != 1 || lazy.memory_failures != expected)
        fprintf(stderr, "trigger %d: deferred=%llu memory-failures=%llu\n",
                trigger, (unsigned long long) lazy.deferred,
                (unsigned long long) lazy.memory_failures);
    CHECK(lazy.deferred == 1 && lazy.memory_failures == expected);
    memset(&probe, 0, sizeof(probe));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "pocSummary = ran + '|' + done; hoard = null", "count", &probe));
    CHECK(summary_is(&probe, expected == 2u ? "2|0" : "1|0"));
    for (unsigned tick = 0; tick < 32; tick++)
        (void) script_runtime_advance(runtime, 1, 64, &probe);
    memset(&probe, 0, sizeof(probe));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "pocSummary = ran + '|' + (done >= 1) + '|' + big()", "after",
        &probe));
    CHECK(summary_is(&probe, "8|true|1"));
    script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    return 0;
}

static int test_lazy_memory_failure_yields_checkpoint(void)
{
    CHECK(lazy_memory_failure_case(LAZY_REACTION) == 0);
    CHECK(lazy_memory_failure_case(LAZY_REACTION_EXECUTOR) == 0);
    CHECK(lazy_memory_failure_case(LAZY_LISTENER_EXECUTOR) == 0);
    return 0;
}

/* What toString() asks before it allocates (script_source_store_query()):
   a span of ASCII blocks is 1, one touching a non-ASCII block 0, whether
   the blocks are plain (classified on first query) or compressed
   (classified when compressed); a dropped or unfilled entry is -1. */
static int test_query(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    const size_t length = 3u * SCRIPT_SOURCE_STORE_BLOCK_BYTES;
    char *text = malloc(length);
    CHECK(text != NULL);
    for (size_t i = 0; i < length; i++) text[i] = (char) ('a' + i % 26);
    /* one two-byte character in the middle block */
    size_t at = SCRIPT_SOURCE_STORE_BLOCK_BYTES + 100u;
    text[at] = (char) 0xc3;
    text[at + 1] = (char) 0xa9;
    for (int eager = 0; eager < 2; eager++) {
        ScriptSourceStore *store = script_source_store_create(&budget,
            eager ? SCRIPT_SOURCE_STORE_COMPRESS_EAGER
                  : SCRIPT_SOURCE_STORE_COMPRESS_NEVER);
        CHECK(store != NULL);
        uint32_t id = script_source_store_add(store, text, length);
        CHECK(id != 0);
        const uint32_t block = SCRIPT_SOURCE_STORE_BLOCK_BYTES;
        CHECK(script_source_store_query(store, id, 10, 200) == 1);
        CHECK(script_source_store_query(store, id, 2 * block + 5, 300) == 1);
        CHECK(script_source_store_query(store, id, block - 10, 20) == 0);
        CHECK(script_source_store_query(store, id, 0, (uint32_t) length) == 0);
        CHECK(script_source_store_query(store, id, 0, (uint32_t) length + 1)
              == -1);
        uint32_t reserved = script_source_store_reserve(store, 64);
        CHECK(reserved != 0
              && script_source_store_query(store, reserved, 0, 8) == -1);
        script_source_store_release(store, id);
        CHECK(script_source_store_query(store, id, 10, 200) == -1);
        script_source_store_destroy(store);
    }
    free(text);
    CHECK(budget.current == 0);
    return 0;
}

/* The codec round-trips text whose last bytes are a match (the block then
   ends with an empty literal token) and decodes a prefix that a later call
   continues. */
static int test_codec(void)
{
    static uint32_t table[SCRIPT_TEXT_LZ_HASH_SIZE];
    static unsigned char text[40000], packed[41000], out[40000];
    for (size_t i = 0; i < sizeof(text); i++)
        text[i] = (unsigned char) ("function f(a){return a+1}\n"[i % 26]);
    for (size_t length = 64; length <= sizeof(text); length += 9973) {
        size_t written = script_text_lz_compress(text, length, packed,
                                                 sizeof(packed), table);
        CHECK(written != 0 && written < length);
        memset(out, 0, sizeof(out));
        CHECK(script_text_lz_decompress(packed, written, out, length)
              && memcmp(out, text, length) == 0);
        CHECK(script_text_lz_matches(packed, written, text, length));
        size_t in = 0, at = 0;
        memset(out, 0, sizeof(out));
        CHECK(script_text_lz_decompress_until(packed, written, out, length,
                                              &in, &at, length / 3)
              && at >= length / 3 && at <= length
              && memcmp(out, text, length / 3) == 0);
        CHECK(script_text_lz_decompress_until(packed, written, out, length,
                                              &in, &at, length)
              && at == length && in == written
              && memcmp(out, text, length) == 0);
        /* a truncated block fails instead of reading past it */
        CHECK(!script_text_lz_decompress(packed, written / 2, out, length));
    }
    return 0;
}

/* ---- the store under reclamation, through the engine hooks ---- */

/* A QuickJS realm whose every allocation first asks the store to give up
   everything it can (as js_rt_heap_growth_hook() does when the realm is
   refused growth), while `armed`. */
typedef struct {
    ScriptSourceStore *store;
    bool armed;
    unsigned relieves;
} ReclaimRealm;

static ReclaimRealm reclaim_realm;

static void *reclaim_malloc(JSMallocState *state, size_t size)
{
    (void) state;
    if (reclaim_realm.armed) {
        reclaim_realm.relieves++;
        (void) script_source_store_relieve(reclaim_realm.store, SIZE_MAX);
    }
    return malloc(size);
}

static void reclaim_free(JSMallocState *state, void *ptr)
{
    (void) state;
    free(ptr);
}

static void *reclaim_realloc(JSMallocState *state, void *ptr, size_t size)
{
    (void) state;
    if (reclaim_realm.armed && size != 0) {
        reclaim_realm.relieves++;
        (void) script_source_store_relieve(reclaim_realm.store, SIZE_MAX);
    }
    return realloc(ptr, size);
}

static size_t reclaim_usable(const void *ptr)
{
    (void) ptr;
    return 0;
}

static int reclaim_read(void *opaque, uint32_t id, uint32_t offset,
                        uint32_t length, char *dest)
{
    return script_source_store_read(opaque, id, offset, length, dest);
}

static void reclaim_release(void *opaque, uint32_t id)
{
    script_source_store_release(opaque, id);
}

static void reclaim_pin(void *opaque, uint32_t id, int required)
{
    script_source_store_require(opaque, id, required != 0);
}

static char *reclaim_eval(JSContext *ctx, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "probe.js",
                            JS_EVAL_TYPE_GLOBAL);
    const char *text = JS_ToCString(ctx, JS_IsException(value)
                                    ? JS_GetException(ctx) : value);
    char *copy = strdup(text != NULL ? text : "(null)");
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return copy;
}

/* Reclamation while a script compiles (before its first lazy function
   exists) and while its bytecode is restored: the operation's lease keeps
   the text, every function still compiles on its first call and returns
   its text, and no lease is left. */
static int test_reclaim_during_compile_and_restore(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    ScriptSourceStore *store = script_source_store_create(
        &budget, SCRIPT_SOURCE_STORE_COMPRESS_UNDER_PRESSURE);
    CHECK(store != NULL);
    reclaim_realm = (ReclaimRealm) { store, false, 0 };
    static const JSMallocFunctions functions = {
        reclaim_malloc, reclaim_free, reclaim_realloc, reclaim_usable
    };
    JSRuntime *rt = JS_NewRuntime2(&functions, NULL);
    CHECK(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    JS_SetLazyFunctionThreshold(rt, 1);
    JS_SetExternalSourceHooks(rt, reclaim_read, reclaim_release,
                              reclaim_pin, store);
    char *source = malloc(24u * KIB);
    CHECK(source != NULL);
    size_t length = (size_t) snprintf(source, 24u * KIB,
        "function lazyA(n) { var k = function inner(q) { return q + n };"
        " return k(1) }\n"
        "var lazyB = (s) => { return 'b' + s };\n"
        "globalThis.check = function () { return [lazyA(2), lazyB(3),"
        " String(lazyA), String(lazyB)].join('|') };\n/*");
    while (length < 20u * KIB) source[length++] = '.';
    length += (size_t) snprintf(source + length, 24u * KIB - length, "*/\n");
    static const char expected[] =
        "3|b3|function lazyA(n) { var k = function inner(q) { return q + n };"
        " return k(1) }|(s) => { return 'b' + s }";

    /* the compile, as js_rt_compile_source_type() runs it */
    uint32_t id = script_source_store_add(store, source, length);
    CHECK(id != 0 && script_source_store_lease(store, id, true));
    JS_SetStripInfo(rt, JS_STRIP_SOURCE);
    JS_SetExternalSourceId(rt, id, (uint32_t) length);
    reclaim_realm.armed = true;
    JSValue compiled = JS_Eval(ctx, source, length, "app.js",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    reclaim_realm.armed = false;
    JS_SetStripInfo(rt, 0);
    CHECK(JS_SetExternalSourceId(rt, 0, 0) == 0);
    (void) script_source_store_lease(store, id, false);
    if (JS_IsException(compiled) || reclaim_realm.relieves < 10) fprintf(stderr, "compile exc=%d relieves=%u\n", JS_IsException(compiled), reclaim_realm.relieves);
    CHECK(!JS_IsException(compiled) && reclaim_realm.relieves >= 10);
    size_t bytes = 0;
    uint8_t *bytecode = JS_WriteObject(ctx, &bytes, compiled,
                                       JS_WRITE_OBJ_BYTECODE);
    CHECK(bytecode != NULL);
    JSValue result = JS_EvalFunction(ctx, compiled);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    /* more reclamation: the text uncompiled functions need stays */
    (void) script_source_store_relieve(store, SIZE_MAX);
    char *text = reclaim_eval(ctx, "check()");
    if (strcmp(text, expected) != 0) fprintf(stderr, "compile: %s\n", text);
    CHECK(strcmp(text, expected) == 0);
    free(text);

    /* the restore, as js_rt_source_store_begin_restore()/end run it */
    uint32_t restored_id = script_source_store_reserve(store, length);
    CHECK(restored_id != 0
          && script_source_store_lease(store, restored_id, true));
    JS_SetExternalSourceId(rt, restored_id, (uint32_t) length);
    reclaim_realm.relieves = 0;
    reclaim_realm.armed = true;
    JSValue restored = JS_ReadObject(ctx, bytecode, bytes,
                                     JS_READ_OBJ_BYTECODE);
    reclaim_realm.armed = false;
    js_free(ctx, bytecode);
    if (JS_IsException(restored) || reclaim_realm.relieves == 0) fprintf(stderr, "restore exc=%d relieves=%u\n", JS_IsException(restored), reclaim_realm.relieves);
    CHECK(!JS_IsException(restored) && reclaim_realm.relieves > 0);
    CHECK(JS_SetExternalSourceId(rt, 0, 0) == 0);
    CHECK(script_source_store_fill(store, restored_id, source));
    (void) script_source_store_lease(store, restored_id, false);
    result = JS_EvalFunction(ctx, restored);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    (void) script_source_store_relieve(store, SIZE_MAX);
    text = reclaim_eval(ctx, "check()");
    if (strcmp(text, expected) != 0) fprintf(stderr, "restore: %s\n", text);
    CHECK(strcmp(text, expected) == 0);
    free(text);

    ScriptSourceStoreStats stats;
    script_source_store_stats(store, &stats);
    CHECK(stats.leases == 0 && stats.read_failures == 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    script_source_store_stats(store, &stats);
    CHECK(stats.entries == 0 && stats.leases == 0
          && stats.required_entries == 0);
    script_source_store_destroy(store);
    free(source);
    CHECK(budget.current == 0);
    return 0;
}

/* A block that does not compress is tried once: later balancing passes
   neither compress it again nor allocate a match table for it, and the
   plain-text totals stay exact. */
static int test_incompressible_blocks_tried_once(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    ScriptSourceStore *store = script_source_store_create(
        &budget, SCRIPT_SOURCE_STORE_COMPRESS_UNDER_PRESSURE);
    CHECK(store != NULL);
    enum { LENGTH = 3 * SCRIPT_SOURCE_STORE_BLOCK_BYTES + 100 };
    static char text[LENGTH];
    uint32_t seed = 12345;
    for (size_t i = 0; i < LENGTH; i++) {
        seed = seed * 1103515245u + 12345u;
        text[i] = (char) (seed >> 16);
    }
    /* the last block is ordinary text: it compresses */
    for (size_t i = 3 * SCRIPT_SOURCE_STORE_BLOCK_BYTES; i < LENGTH; i++)
        text[i] = 'a';
    uint32_t id = script_source_store_add(store, text, LENGTH);
    CHECK(id != 0);
    ScriptSourceStoreStats first, again;
    script_source_store_balance(store, 0);
    script_source_store_stats(store, &first);
    CHECK(first.compress_attempts == 4 && first.compressed_blocks == 1
          && first.scratch_tables == 1
          && first.plain_bytes == 3u * SCRIPT_SOURCE_STORE_BLOCK_BYTES
          && first.compressible_plain_bytes == 0);
    /* leased, so relieving compresses but does not drop it */
    CHECK(script_source_store_lease(store, id, true));
    for (int pass = 0; pass < 10; pass++) {
        script_source_store_balance(store, 0);
        (void) script_source_store_relieve(store, 1);
    }
    (void) script_source_store_lease(store, id, false);
    script_source_store_stats(store, &again);
    CHECK(again.compress_attempts == first.compress_attempts
          && again.scratch_tables == first.scratch_tables
          && again.plain_bytes == first.plain_bytes);
    char back[LENGTH];
    CHECK(script_source_store_read(store, id, 0, LENGTH, back) == 0
          && memcmp(back, text, LENGTH) == 0);
    script_source_store_release(store, id);
    script_source_store_stats(store, &again);
    CHECK(again.plain_bytes == 0 && again.compressible_plain_bytes == 0
          && again.held_bytes == 0);
    script_source_store_destroy(store);
    CHECK(budget.current == 0);
    return 0;
}

int main(void)
{
    CHECK(test_codec() == 0);
    CHECK(test_query() == 0);
    CHECK(test_reclaim_during_compile_and_restore() == 0);
    CHECK(test_incompressible_blocks_tried_once() == 0);
    CHECK(test_tostring_and_revisit() == 0);
    CHECK(test_threshold() == 0);
    CHECK(test_pressure_fallback() == 0);
    CHECK(test_worker_realm() == 0);
    CHECK(test_lazy_memory_failure_yields_checkpoint() == 0);
    printf("tilefinch-function-source-store-tests: ok\n");
    return 0;
}
