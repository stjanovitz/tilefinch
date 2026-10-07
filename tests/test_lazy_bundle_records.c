/* Lazy webpack bundle records (src/js_lazy_webpack.c, src/session_lazy_
   bundle.c): a later load of the exact same bundle bytes skips the planner
   and the factory syntax preflight; anything else plans and preflights as a
   first visit does.

   Persistent-tier directories are fresh ones under TILEFINCH_TEST_SCRATCH_DIR
   (CTest sets it inside the build tree). Nothing here can reach a Memory
   Stick: the tier refuses device paths on host builds. */
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"
#include "tilefinch/viewport.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

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

static const char site_a[] = "https://site-a.test/";
static const char site_b[] = "https://other-site.test/";
static const char bundle_url[] = "https://cdn.test/chunk.js";

/* Three factories; `tag` (three characters) is returned by factory 3. */
static void make_bundle(char *out, size_t capacity, const char *tag,
                        const char *extra)
{
    snprintf(out, capacity,
             "(self.webpackChunk_t=self.webpackChunk_t||[]).push([[1],{"
             "1:(m,e,r)=>{e.answer=r(2)+1},"
             "2:m=>{m.exports=41%s},"
             "3:function(m,e,r){e.tag='%s'}"
             "}]);", extra, tag);
}

/* ---- fixture ---- */

typedef ScriptPageFixture Fixture;

static bool fixture_open(Fixture *fixture)
{
    return script_page_fixture_open(
        fixture, "<script src=/chunk.js></script>", 0);
}

/* Every byte the page, the session and the records owned is returned. */
static bool fixture_close(Fixture *fixture)
{
    return script_page_fixture_close(fixture);
}

static bool session_start(Fixture *fixture, BrowserSession *session,
                          const char *directory)
{
    if (!browser_session_init(session, &fixture->budget, 512u * KIB))
        return false;
    fixture->options.session = session;
    return directory == NULL
        || browser_session_script_disk_configure(session, directory, true,
                                                 0);
}

static BrowserSharedBody *shared_source(Budget *budget, const char *source)
{
    size_t length = strlen(source);
    unsigned char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, source, length + 1);
    BrowserSharedBody *body = browser_shared_body_take(budget, copy, length);
    if (body == NULL) budget_free(budget, copy);
    return body;
}

static void release_shared_source(void *opaque)
{
    browser_shared_body_release((BrowserSharedBody *) opaque);
}

typedef struct {
    bool planned;
    bool verified;
    bool digest_ready;
    size_t factories;
    ScriptLazyEvaluation evaluation;
    /* Not planned: the ordinary eager evaluation of the whole script. */
    bool eager;
    bool eager_ok;
    ScriptResult result;
    char summary[256];
} Visit;

static const char run_default[] =
    "(()=>{const m=globalThis.__m,c={};"
    "function r(id){if(c[id])return c[id].exports;"
    "const x=c[id]={exports:{}};m[id](x,x.exports,r);"
    "return x.exports}"
    "globalThis.pocSummary='RAN:'+r(1).answer+':'+r(3).tag})()";

/* One page load of `source` from bundle_url on `page_url`: plan (the
   planner or a record), evaluate on the lazy path, then call the factories
   through a captured webpack registration. */
/* When set, evaluated after `run` (and the microtasks it queued) to put
   what the page observed into the summary. */
static const char *visit_after;

static bool visit_run(Fixture *fixture, const char *page_url,
                      const char *source, bool no_store, const char *run,
                      Visit *out)
{
    memset(out, 0, sizeof(*out));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture->document, &fixture->budget, 8u * MIB, 8000, page_url,
        &fixture->options, &out->result);
    if (runtime == NULL) return false;
    static const char capture[] =
        "globalThis.webpackChunk_t=[];"
        "globalThis.webpackChunk_t.push=function(v){globalThis.__m=v[1]};";
    bool ok = script_runtime_evaluate_diagnostic(
        runtime, capture, "<capture>", &out->result);
    BrowserSharedBody *body = shared_source(&fixture->budget, source);
    ok = ok && body != NULL;
    ScriptLazyWebpackPlan plan;
    memset(&plan, 0, sizeof(plan));
    if (ok) {
        out->planned = script_runtime_lazy_webpack_plan(
            runtime, &fixture->budget, bundle_url,
            (const char *) body->data, body->length, &plan);
        out->verified = plan.from_record;
        out->digest_ready = plan.digest_ready;
        out->factories = plan.factory_count;
    }
    if (ok && out->planned) {
        ScriptLazyBundleRecordTarget target = {
            .request_url = bundle_url,
            .body = body,
            .no_store = no_store
        };
        BrowserSharedBody *lease = browser_shared_body_retain(body);
        out->evaluation =
            script_runtime_evaluate_external_lazy_webpack_recorded(
                runtime, fixture->script, (const char *) body->data,
                body->length, bundle_url, &plan, &target, lease,
                release_shared_source, &out->result);
        if (out->evaluation == SCRIPT_LAZY_EVALUATION_FALLBACK)
            browser_shared_body_release(lease);
        script_lazy_webpack_plan_destroy(&plan);
    } else if (ok) {
        out->eager = true;
        out->eager_ok = script_runtime_evaluate_external_classic_cached(
            runtime, fixture->script, (const char *) body->data,
            body->length, bundle_url, bundle_url, no_store, &out->result);
    }
    if (ok && (out->evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
               || out->eager_ok)) {
        ok = script_runtime_evaluate_diagnostic(runtime, run, "<run>",
                                                &out->result);
        if (ok && visit_after != NULL)
            ok = script_runtime_evaluate_diagnostic(
                runtime, visit_after, "<after>", &out->result);
        snprintf(out->summary, sizeof(out->summary), "%s",
                 out->result.summary);
    }
    script_runtime_refresh_result(runtime, &out->result);
    browser_shared_body_release(body);
    script_runtime_destroy(runtime);
    return ok;
}

static bool visit(Fixture *fixture, const char *page_url, const char *source,
                  bool no_store, Visit *out)
{
    return visit_run(fixture, page_url, source, no_store, run_default, out);
}

static unsigned idle(BrowserSession *session)
{
    unsigned turns = 0;
    while (turns < 10000u
           && (browser_session_lazy_bundle_record_maintenance(session)
               || browser_session_script_disk_maintenance(session)))
        turns++;
    return turns;
}

static size_t records(const BrowserSession *session)
{
    return browser_session_script_bytecode_entries(
        session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE);
}

/* A first visit: the planner ran, no factory was compiled before it ran
   (no syntax preflight), nothing was hashed. */
static bool first_visit(const Visit *v)
{
    return v->planned && !v->verified && v->factories >= 3
        && v->evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
        && v->result.lazy_webpack_record_hits == 0
        && v->result.lazy_webpack_syntax_preflight_attempts == 0;
}

/* A hit: the plan came from the record, and again no factory was compiled
   before it ran. */
static bool record_hit(const Visit *v)
{
    return v->planned && v->verified && v->factories >= 3
        && v->evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
        && v->result.lazy_webpack_record_hits == 1
        && v->result.lazy_webpack_syntax_preflight_attempts == 0
        && v->result.lazy_webpack_record_queued == 0;
}

/* ---- tests ---- */

/* The second load of the same bytes skips planning and preflight and runs
   the same factories; the first visit hashed nothing on its load. */
static int test_hit_skips_plan_and_preflight(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_bundle(source, sizeof(source), "TAG", "");
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v)
          && !v.digest_ready && strcmp(v.summary, "RAN:42:TAG") == 0
          && v.result.lazy_webpack_record_queued == 1);
    /* Queued, not stored: the digest is idle work. */
    CHECK(browser_session_lazy_bundle_pending_count(&session) == 1
          && records(&session) == 0);
    CHECK(idle(&session) >= 1
          && browser_session_lazy_bundle_pending_count(&session) == 0
          && records(&session) == 1);
    size_t bytes = browser_session_script_bytecode_bytes(
        &session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE);
    /* 32-byte header, 12 bytes per factory, the key strings. */
    CHECK(bytes >= 32u + 3u * 12u && bytes < 256u);
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v)
          && strcmp(v.summary, "RAN:42:TAG") == 0);
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Other bytes under the same URL miss and are planned and preflighted:
   same length (the candidate is hashed and replaced at once) or not (no
   hash at all). */
static int test_changed_bytes_miss_and_replan(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512], same_length[512], longer[512];
    make_bundle(source, sizeof(source), "TAG", "");
    make_bundle(same_length, sizeof(same_length), "TAH", "");
    make_bundle(longer, sizeof(longer), "TAG", "+0");
    CHECK(strlen(source) == strlen(same_length));
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    CHECK(records(&session) == 1);
    CHECK(visit(&fixture, site_a, same_length, false, &v)
          && first_visit(&v) && v.digest_ready
          && strcmp(v.summary, "RAN:42:TAH") == 0);
    /* The digest was taken by the lookup: stored without queueing. */
    CHECK(browser_session_lazy_bundle_pending_count(&session) == 0
          && records(&session) == 1);
    CHECK(visit(&fixture, site_a, same_length, false, &v) && record_hit(&v)
          && strcmp(v.summary, "RAN:42:TAH") == 0);
    CHECK(visit(&fixture, site_a, longer, false, &v) && first_visit(&v)
          && !v.digest_ready && strcmp(v.summary, "RAN:42:TAG") == 0);
    CHECK(browser_session_lazy_bundle_pending_count(&session) == 1);
    (void) idle(&session);
    CHECK(records(&session) == 1);
    CHECK(visit(&fixture, site_a, longer, false, &v) && record_hit(&v));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Records are partitioned by top-level site: the same URL and bytes under
   another site miss (without hashing) and get a record of their own. */
static int test_other_site_misses(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_bundle(source, sizeof(source), "TAG", "");
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    CHECK(visit(&fixture, site_b, source, false, &v) && first_visit(&v)
          && !v.digest_ready);
    (void) idle(&session);
    CHECK(records(&session) == 2);
    CHECK(visit(&fixture, site_b, source, false, &v) && record_hit(&v));
    /* A site's data clear drops its records only. */
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    CHECK(tilefinch_url_site_key(site_b, partition, sizeof(partition)));
    browser_session_script_bytecode_clear_partition(&session, partition);
    CHECK(records(&session) == 1);
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Rewrites the one stored record with header word `word` changed. */
static bool corrupt_record_word(BrowserSession *session, size_t word)
{
    BrowserScriptBytecodeTable *table = session->lazy_bundle_records;
    if (table == NULL) return false;
    for (size_t i = 0; i < BROWSER_SCRIPT_BYTECODE_ENTRIES; i++) {
        BrowserScriptBytecodeEntry *entry = &table->entries[i];
        if (entry->bytecode == NULL) continue;
        size_t length = entry->bytecode->length;
        unsigned char copy[1024];
        if (length > sizeof(copy) || 4u * word + 4u > length) return false;
        memcpy(copy, entry->bytecode->data, length);
        copy[4u * word] ^= 0x5au;
        char name[128], url[256], partition[256];
        snprintf(name, sizeof(name), "%s", entry->module_name);
        snprintf(url, sizeof(url), "%s", entry->response_url);
        snprintf(partition, sizeof(partition), "%s", entry->partition_key);
        BrowserScriptBytecodeKey key = {
            .module_name = name,
            .response_url = url,
            .partition_key = partition,
            .source_length = entry->source_length,
            .compile_flags = entry->compile_flags,
            .ordinal = entry->ordinal,
            .digest_ready = true
        };
        memcpy(key.source_digest, entry->source_digest, 32);
        return browser_session_script_bytecode_put(
            session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, &key, 0, copy,
            length);
    }
    return false;
}

/* A record made under another bytecode ABI, engine build or release (the
   identity words of its header) is a miss: the bundle is planned and
   preflighted, and the record is replaced by this build's. */
static int test_abi_or_engine_change_misses(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_bundle(source, sizeof(source), "TAG", "");
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    /* Words 2, 3, 4: bytecode ABI, engine fingerprint, release. */
    for (size_t word = 2; word <= 4; word++) {
        CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v));
        CHECK(corrupt_record_word(&session, word) && records(&session) == 1);
        CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v)
              && v.digest_ready && strcmp(v.summary, "RAN:42:TAG") == 0);
        CHECK(records(&session) == 1
              && browser_session_lazy_bundle_pending_count(&session) == 0);
    }
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* A no-store response never gets a record. */
static int test_no_store_not_recorded(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_bundle(source, sizeof(source), "TAG", "");
    Visit v;
    CHECK(visit(&fixture, site_a, source, true, &v) && first_visit(&v)
          && v.result.lazy_webpack_record_queued == 0);
    CHECK(browser_session_lazy_bundle_pending_count(&session) == 0);
    (void) idle(&session);
    CHECK(records(&session) == 0);
    CHECK(visit(&fixture, site_a, source, true, &v) && first_visit(&v));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The bundle used by the syntax-error tests: factories 1-3 as usual, then
   on lines of their own a factory 4 whose body does not parse (`const`
   with no binding, on the bundle's line 3). */
static void make_broken_bundle(char *out, size_t capacity)
{
    snprintf(out, capacity,
             "(self.webpackChunk_t=self.webpackChunk_t||[]).push([[1],{"
             "1:(m,e,r)=>{e.answer=r(2)+1},"
             "2:m=>{m.exports=41},"
             "3:function(m,e,r){e.tag='TAG'},\n"
             "4:m=>{\nconst = 1}"
             "}]);");
}

/* What a page sees: nothing went wrong, as far as the page can tell. */
static bool nothing_reported(const Visit *v)
{
    return v->result.error[0] == '\0'
        && v->result.external_scripts_failed == 0
        && v->result.lazy_webpack_factory_compile_failures == 0
        && v->result.lazy_webpack_syntax_preflight_failures == 0;
}

/* A syntax error in a factory that never runs is never found: the bundle
   registers lazily, its other factories run, and nothing is reported. (A
   whole-bundle compile, and the retired first-visit preflight, rejected the
   bundle instead: no factory ran.) The same holds on a revisit served from
   the bundle's record. */
static int test_syntax_error_in_unrun_factory(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_broken_bundle(source, sizeof(source));
    static const char run[] =
        "(()=>{const m=globalThis.__m,c={};"
        "function r(id){if(c[id])return c[id].exports;"
        "const x=c[id]={exports:{}};m[id](x,x.exports,r);"
        "return x.exports}"
        "globalThis.pocSummary='RAN:'+r(1).answer+':'+r(3).tag"
        "+':'+(globalThis.__tilefinchUncaughtErrorCount|0)})()";
    for (int round = 0; round < 2; round++) {
        Visit v;
        CHECK(visit_run(&fixture, site_a, source, false, run, &v)
              && v.planned && v.factories == 4
              && v.evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
              && strcmp(v.summary, "RAN:42:TAG:0") == 0
              && nothing_reported(&v));
        CHECK(round == 0 ? first_visit(&v) : record_hit(&v));
        (void) idle(&session);
        CHECK(records(&session) == 1);
    }
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* A syntax error in a factory that runs surfaces when it runs: the call
   throws a SyntaxError located in the bundle (its URL, line 3, the column
   of the bad token), each call throws again, and the other factories keep
   working. Uncaught, it reaches the page's error reporting like any
   exception thrown by a callback (window.onerror). Revisits behave alike. */
static int test_syntax_error_in_run_factory(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_broken_bundle(source, sizeof(source));
    static const char run[] =
        "(()=>{const m=globalThis.__m,c={};"
        "function r(id){if(c[id])return c[id].exports;"
        "const x=c[id]={exports:{}};m[id](x,x.exports,r);"
        "return x.exports}"
        "const seen=[];"
        "for(let i=0;i<2;i++){try{r(4);seen.push('ran')}catch(e){"
        "delete c[4];"
        "seen.push(e instanceof SyntaxError,e.fileName,e.lineNumber,"
        "e.columnNumber,String(e.stack).split('\\n')[0].trim())}}"
        "globalThis.pocSummary=seen.join('|')+'|'+r(1).answer+':'"
        "+r(3).tag})()";
    for (int round = 0; round < 2; round++) {
        Visit v;
        CHECK(visit_run(&fixture, site_a, source, false, run, &v)
              && v.planned && v.factories == 4
              && v.evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED);
        CHECK(round == 0 ? first_visit(&v) : record_hit(&v));
        static const char expected[] =
            "true|https://cdn.test/chunk.js|3|7|"
            "at https://cdn.test/chunk.js:3:7|"
            "true|https://cdn.test/chunk.js|3|7|"
            "at https://cdn.test/chunk.js:3:7|42:TAG";
        if (strcmp(v.summary, expected) != 0)
            fprintf(stderr, "summary=%s\n", v.summary);
        CHECK(strcmp(v.summary, expected) == 0);
        CHECK(v.result.lazy_webpack_factory_compile_failures == 2);
        (void) idle(&session);
    }
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* The uncaught case end to end: a factory's SyntaxError thrown from a
   microtask reaches window.onerror with the error located in the bundle. */
static int test_run_factory_syntax_error_is_reported(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512];
    make_broken_bundle(source, sizeof(source));
    static const char run[] =
        "(()=>{const m=globalThis.__m;"
        "globalThis.onerror=(msg,src,line,col,error)=>{"
        "globalThis.__reported=(error instanceof SyntaxError)+':'"
        "+error.fileName+':'+error.lineNumber};"
        "queueMicrotask(()=>m[4]({exports:{}}));"
        "globalThis.pocSummary='queued'})()";
    Visit v;
    visit_after = "globalThis.pocSummary=String(globalThis.__reported)";
    bool visited = visit_run(&fixture, site_a, source, false, run, &v);
    visit_after = NULL;
    if (strcmp(v.summary, "true:https://cdn.test/chunk.js:3") != 0)
        fprintf(stderr, "reported=%s\n", v.summary);
    CHECK(visited && first_visit(&v)
          && strcmp(v.summary, "true:https://cdn.test/chunk.js:3") == 0);
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* A script the planner does not take (here, code after the registration)
   still goes through the eager path exactly as before: one compile of the
   whole script, so a syntax error anywhere in it rejects all of it and no
   factory runs. */
static int test_eager_fallback_unchanged(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    static const char broken[] =
        "(self.webpackChunk_t=self.webpackChunk_t||[]).push([[1],{"
        "1:(m,e,r)=>{e.answer=r(2)+1},2:m=>{m.exports=41},"
        "3:function(m,e,r){e.tag='TAG'},4:m=>{const = 1}}]);"
        "globalThis.after=1;";
    static const char fine[] =
        "(self.webpackChunk_t=self.webpackChunk_t||[]).push([[1],{"
        "1:(m,e,r)=>{e.answer=r(2)+1},2:m=>{m.exports=41},"
        "3:function(m,e,r){e.tag='TAG'}}]);globalThis.after=1;";
    static const char run[] =
        "globalThis.pocSummary=(globalThis.__m?'REGISTERED':'NONE')"
        "+':'+(globalThis.after|0)";
    Visit v;
    CHECK(visit_run(&fixture, site_a, broken, false, run, &v)
          && !v.planned && v.eager && !v.eager_ok
          && v.result.external_scripts_failed == 1
          && v.result.lazy_webpack_applied == 0);
    CHECK(visit_run(&fixture, site_a, fine, false, run, &v)
          && !v.planned && v.eager && v.eager_ok
          && strcmp(v.summary, "REGISTERED:1") == 0);
    (void) idle(&session);
    CHECK(records(&session) == 0);
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* Clearing the cache, the reclaims and teardown leave nothing behind:
   queued records release their bodies and records their bytes. */
static int test_clears_and_teardown_release_everything(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    size_t baseline = fixture.budget.current;
    char source[512], other[512];
    make_bundle(source, sizeof(source), "TAG", "");
    make_bundle(other, sizeof(other), "TAG", "+0");
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    CHECK(visit(&fixture, site_b, other, false, &v) && first_visit(&v));
    CHECK(records(&session) == 1
          && browser_session_lazy_bundle_pending_count(&session) == 1
          && fixture.budget.current > baseline);
    /* The Budget reclaim hook goes to the queued record first: it holds
       the body's last reference, so its digest is finished there and the
       body released, and the record itself is kept for the next idle
       turn. */
    size_t queued_bytes = fixture.budget.current;
    CHECK(browser_session_script_bytecode_reclaim(&session, 1)
              == strlen(other)
          && fixture.budget.current < queued_bytes
          && browser_session_lazy_bundle_pending_count(&session) == 1
          && records(&session) == 1);
    CHECK(idle(&session) == 1 && records(&session) == 2
          && browser_session_lazy_bundle_pending_count(&session) == 0);
    CHECK(visit(&fixture, site_b, other, false, &v) && record_hit(&v));
    /* Then everything else, records last (the hook frees entries only;
       the emptied table goes with the next clear). */
    CHECK(browser_session_script_bytecode_reclaim(&session, SIZE_MAX) != 0
          && records(&session) == 0);
    /* A cache clear drops queued and stored records alike. */
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    CHECK(visit(&fixture, site_b, other, false, &v) && first_visit(&v));
    browser_session_cache_clear(&session);
    CHECK(records(&session) == 0
          && browser_session_lazy_bundle_pending_count(&session) == 0
          && fixture.budget.current == baseline);
    /* Teardown with a record and a queued one outstanding. */
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    (void) idle(&session);
    CHECK(visit(&fixture, site_b, other, false, &v) && first_visit(&v));
    CHECK(records(&session) == 1
          && browser_session_lazy_bundle_pending_count(&session) == 1);
    browser_session_destroy(&session);
    /* fixture_close: zero bytes owned. */
    CHECK(fixture_close(&fixture));
    return 0;
}

static unsigned reclaim_calls;

static size_t reclaim_everything(void *opaque, size_t needed_bytes)
{
    (void) needed_bytes;
    reclaim_calls++;
    return browser_session_script_bytecode_reclaim(opaque, SIZE_MAX);
}

/* The Budget reclaim hook can run inside the idle store of a digested
   record (the store allocates): the record being stored is out of the
   queue by then, so the hook only finishes the others' digests and evicts
   records, and every byte stays accounted for. */
static int test_reclaim_during_idle_store(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    BrowserSession session;
    CHECK(session_start(&fixture, &session, NULL));
    char source[512], other[512];
    make_bundle(source, sizeof(source), "TAG", "");
    make_bundle(other, sizeof(other), "TAG", "+0");
    Visit v;
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    CHECK(visit(&fixture, site_b, other, false, &v) && first_visit(&v));
    CHECK(browser_session_lazy_bundle_pending_count(&session) == 2);
    budget_set_reclaim_hook(&fixture.budget, reclaim_everything, &session);
    size_t limit = fixture.budget.limit;
    fixture.budget.limit = fixture.budget.current + 64u;
    (void) idle(&session);
    fixture.budget.limit = limit;
    budget_set_reclaim_hook(&fixture.budget, NULL, NULL);
    CHECK(reclaim_calls != 0
          && browser_session_lazy_bundle_pending_count(&session) == 0
          && records(&session) <= 2);
    CHECK(budget_categories_reconcile(&fixture.budget));
    browser_session_destroy(&session);
    CHECK(fixture_close(&fixture));
    return 0;
}

/* ---- the persistent tier ---- */

static char scratch_root[512];

static bool make_scratch_root(void)
{
    return script_test_make_scratch_root(scratch_root, sizeof(scratch_root),
                                         "bundle-records");
}

static size_t packs(const char *directory, char *first, size_t capacity)
{
    size_t count = 0;
    DIR *dir = opendir(directory);
    if (dir == NULL) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 5, ".tfsc") != 0)
            continue;
        if (count++ == 0 && first != NULL)
            snprintf(first, capacity, "%s/%s", directory, entry->d_name);
    }
    closedir(dir);
    return count;
}

/* Flips the record payload's bytecode-ABI word inside a one-record pack
   and keeps the pack's CRC valid, as a build with another ABI would see
   it (only the payload check stands between it and a hit). */
static bool rewrite_pack_payload_abi(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char data[4096];
    size_t length = fread(data, 1, sizeof(data), file);
    fclose(file);
    /* Pack header 56, record header 48, then the payload. */
    const size_t payload = 56u + 48u;
    if (length < payload + 32u + 4u || length == sizeof(data)) return false;
    data[payload + 8u] ^= 0x5au;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data, (uInt) (length - 4u));
    data[length - 4u] = (unsigned char) crc;
    data[length - 3u] = (unsigned char) (crc >> 8);
    data[length - 2u] = (unsigned char) (crc >> 16);
    data[length - 1u] = (unsigned char) (crc >> 24);
    file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

/* With the opt-in tier on, records are written at idle and a restarted
   browser (a fresh session on the same host directory) hits them; another
   ABI's record in a pack misses. */
static int test_disk_tier_round_trip(void)
{
    Fixture fixture;
    CHECK(fixture_open(&fixture));
    char directory[700];
    snprintf(directory, sizeof(directory), "%s/cache", scratch_root);
    char source[512];
    make_bundle(source, sizeof(source), "TAG", "");
    BrowserSession session;
    Visit v;
    CHECK(session_start(&fixture, &session, directory));
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v));
    CHECK(idle(&session) >= 2);
    browser_session_destroy(&session);
    char pack[900];
    CHECK(packs(directory, pack, sizeof(pack)) == 1);

    /* Restart: the record comes from the card. */
    CHECK(session_start(&fixture, &session, directory));
    CHECK(records(&session) == 0);
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v)
          && strcmp(v.summary, "RAN:42:TAG") == 0);
    browser_session_destroy(&session);

    /* Another ABI's record (valid pack): a miss, replaced at idle. */
    CHECK(rewrite_pack_payload_abi(pack));
    CHECK(session_start(&fixture, &session, directory));
    CHECK(visit(&fixture, site_a, source, false, &v) && first_visit(&v)
          && strcmp(v.summary, "RAN:42:TAG") == 0);
    (void) idle(&session);
    browser_session_destroy(&session);
    CHECK(packs(directory, NULL, 0) == 1);
    CHECK(session_start(&fixture, &session, directory));
    CHECK(visit(&fixture, site_a, source, false, &v) && record_hit(&v));
    browser_session_destroy(&session);
    script_test_remove_tree(directory);
    CHECK(fixture_close(&fixture));
    return 0;
}

int main(void)
{
    if (!make_scratch_root()) {
        fprintf(stderr, "cannot create a scratch directory\n");
        return 1;
    }
    int failures = 0;
    failures += test_hit_skips_plan_and_preflight();
    failures += test_changed_bytes_miss_and_replan();
    failures += test_other_site_misses();
    failures += test_abi_or_engine_change_misses();
    failures += test_no_store_not_recorded();
    failures += test_syntax_error_in_unrun_factory();
    failures += test_syntax_error_in_run_factory();
    failures += test_run_factory_syntax_error_is_reported();
    failures += test_eager_fallback_unchanged();
    failures += test_clears_and_teardown_release_everything();
    failures += test_reclaim_during_idle_store();
    failures += test_disk_tier_round_trip();
    script_test_remove_tree(scratch_root);
    if (failures != 0) {
        fprintf(stderr, "%d lazy bundle record test(s) failed\n", failures);
        return 1;
    }
    printf("lazy bundle record tests passed\n");
    return 0;
}
