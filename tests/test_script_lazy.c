#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/script_admission.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/session.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/node.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static bool plans(Budget *budget, const char *source,
                  ScriptLazyWebpackPlan *plan)
{
    return script_lazy_webpack_plan_create(
        budget, source, strlen(source), plan);
}

static void test_nested_template_depth_is_bounded(Budget *budget)
{
    static const char prefix[] =
        "(self.webpackChunk_depth=self.webpackChunk_depth||[])"
        ".push([[1],{1:a=>{const value=";
    static const char suffix[] = ";return value}}]);";
    static const unsigned depths[] = {16u, 64u, 65u, 80u};
    for (size_t depth_index = 0;
         depth_index < sizeof(depths) / sizeof(depths[0]); depth_index++) {
        unsigned depth = depths[depth_index];
        size_t capacity = sizeof(prefix) + sizeof(suffix)
            + (size_t) depth * 5u + 8u;
        char *source = malloc(capacity);
        CHECK(source != NULL);
        if (source == NULL) continue;
        size_t used = 0;
        memcpy(source + used, prefix, sizeof(prefix) - 1u);
        used += sizeof(prefix) - 1u;
        for (unsigned i = 0; i < depth; i++) {
            memcpy(source + used, "`${", 3u);
            used += 3u;
        }
        source[used++] = '0';
        for (unsigned i = 0; i < depth; i++) {
            memcpy(source + used, "}`", 2u);
            used += 2u;
        }
        memcpy(source + used, suffix, sizeof(suffix));
        used += sizeof(suffix) - 1u;
        ScriptLazyWebpackPlan plan = {0};
        size_t before = budget->current;
        bool planned = script_lazy_webpack_plan_create(
            budget, source, used, &plan);
        CHECK(depth <= 64u ? planned : !planned);
        if (planned) script_lazy_webpack_plan_destroy(&plan);
        CHECK(budget->current == before);
        free(source);
    }
}

static lxb_dom_node_t *find_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 6 && memcmp(name, "script", 6) == 0) {
            return node;
        }
        lxb_dom_node_t *nested = find_script(node->first_child);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static BrowserSharedBody *shared_source(Budget *budget, const char *source)
{
    size_t length = strlen(source);
    unsigned char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, source, length + 1);
    BrowserSharedBody *body = browser_shared_body_take(
        budget, copy, length);
    if (body == NULL) budget_free(budget, copy);
    return body;
}

static void test_resource_loader_statement_planning(void)
{
    static const char source[] =
        "/* retained response */\n"
        "mw.loader.impl(function(){return[\"one\",function(){"
        "return /[;)]/.test(`x${1 + 2}`)}]});\n"
        "mw.loader.impl(function(){return[\"two\",function(){"
        "return {nested:[1,2,3]}}]});";
    Budget budget;
    budget_init(&budget, 64u * 1024u);
    ScriptResourceLoaderPlan plan;
    CHECK(script_resource_loader_plan_create(
              &budget, source, sizeof(source) - 1, &plan)
          && plan.statement_count == 2
          && plan.largest_statement_bytes > 32
          && plan.statement_source_bytes < sizeof(source) - 1
          && plan.statements[0].source_offset
               < plan.statements[1].source_offset
          && plan.statements[0].source_length
               <= sizeof(source) - 1 - plan.statements[0].source_offset
          && plan.statements[1].source_length
               <= sizeof(source) - 1 - plan.statements[1].source_offset);
    script_resource_loader_plan_destroy(&plan);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);

    static const char mixed[] =
        "mw.loader.impl(function(){return['one',function(){}]});"
        "globalThis.after=1;";
    CHECK(!script_resource_loader_plan_create(
        &budget, mixed, sizeof(mixed) - 1, &plan));
    static const char malformed[] =
        "mw.loader.impl(function(){return['one',function(){}]});"
        "mw.loader.impl(function(){";
    CHECK(!script_resource_loader_plan_create(
        &budget, malformed, sizeof(malformed) - 1, &plan));
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
}

static void release_shared_source(void *opaque)
{
    browser_shared_body_release((BrowserSharedBody *) opaque);
}

static bool test_foreign_session_cache_is_not_reclaimed(void)
{
    static const char html[] =
        "<!doctype html><html><body><script id=target></script></body></html>";
    static const char install_capture[] =
        "globalThis.webpackChunk_foreign=[];"
        "globalThis.webpackChunk_foreign.push=function(value){"
        "globalThis.__foreignLazyModules=value[1]};";
    static const char large_prefix[] =
        "(self.webpackChunk_foreign=self.webpackChunk_foreign||[]).push([[1],{"
        "1:function(module){/*";
    static const char large_suffix[] =
        "*/module.exports=99}}]);";
    const size_t large_comment_length = 270u * 1024u;
    const size_t large_source_length = sizeof(large_prefix) - 1
        + large_comment_length + sizeof(large_suffix) - 1;
    const size_t cache_payload_length = 128u * 1024u;
    const size_t pressure_remaining = 512u * 1024u;

    Budget page_budget, session_budget;
    budget_init(&page_budget, 16u * 1024u * 1024u);
    budget_init(&session_budget, 2u * 1024u * 1024u);
    bool lexbor_installed = budget_install_lexbor(&page_budget);
    PocDocument document = {0};
    bool document_ready = lexbor_installed
        && document_parse(&document, &page_budget, html,
                          sizeof(html) - 1, 17);
    BrowserSession foreign_session = {0};
    bool session_ready = browser_session_init(
        &foreign_session, &session_budget, 512u * 1024u);
    ViewportContext viewport;
    bool viewport_ready = viewport_context_init(
        &viewport, 480, 272, 480, 272);
    ScriptRuntime *runtime = NULL;
    ScriptResult result = {0};
    ScriptLazyWebpackPlan plan = {0};
    bool plan_ready = false;
    BrowserSharedBody *body = NULL;
    char *large_source = NULL;
    void *pressure_filler = NULL;
    bool ok = document_ready && session_ready && viewport_ready
        && foreign_session.budget != &page_budget;

    if (ok) {
        ScriptRuntimeOptions options = {
            .session = &foreign_session,
            .viewport = viewport,
            .defer_document_scripts = true
        };
        runtime = script_runtime_create_configured(
            &document, &page_budget, 8u * 1024u * 1024u, 3000,
            "https://foreign-budget.test/", &options, &result);
        ok = runtime != NULL && result.success;
    }
    lxb_dom_node_t *script = ok ? find_script(
        lxb_dom_interface_node(document.html)) : NULL;
    ok = ok && script != NULL
        && script_runtime_evaluate_diagnostic(
               runtime, install_capture, "<install-foreign-capture>", &result);

    unsigned char *cache_payload = ok ? malloc(cache_payload_length) : NULL;
    if (cache_payload != NULL) memset(cache_payload, 'c', cache_payload_length);
    ok = ok && cache_payload != NULL
        && browser_session_cache_put_response(
               &foreign_session,
               "https://foreign-budget.test/optional-cache.bin",
               cache_payload, cache_payload_length, NULL, NULL,
               "application/octet-stream");
    free(cache_payload);

    large_source = ok ? malloc(large_source_length + 1) : NULL;
    if (large_source != NULL) {
        size_t at = 0;
        memcpy(large_source + at, large_prefix, sizeof(large_prefix) - 1);
        at += sizeof(large_prefix) - 1;
        memset(large_source + at, 'a', large_comment_length);
        at += large_comment_length;
        memcpy(large_source + at, large_suffix, sizeof(large_suffix));
    }
    plan_ready = large_source != NULL
        && script_lazy_webpack_plan_create(
               &page_budget, large_source, large_source_length, &plan);
    body = plan_ready ? shared_source(&page_budget, large_source) : NULL;
    ok = ok && plan_ready && body != NULL;
    if (ok) {
        ScriptLazyEvaluation evaluation =
            script_runtime_evaluate_external_lazy_webpack(
                runtime, script, (const char *) body->data, body->length,
                "https://foreign-budget.test/large-chunk.js", &plan,
                body, release_shared_source, &result);
        if (evaluation != SCRIPT_LAZY_EVALUATION_FALLBACK) body = NULL;
        ok = evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED;
    }
    if (plan_ready) {
        script_lazy_webpack_plan_destroy(&plan);
        plan_ready = false;
    }
    free(large_source);
    large_source = NULL;

    static const char schedule_pressure_failure[] =
        "globalThis.__foreignLazyUnexpected=false;"
        "globalThis.__foreignLazyModule={exports:{}};"
        "setTimeout(()=>{globalThis.__foreignLazyModules[1]("
        "globalThis.__foreignLazyModule);"
        "globalThis.__foreignLazyUnexpected=true},0)";
    ok = ok && script_runtime_evaluate_diagnostic(
        runtime, schedule_pressure_failure,
        "<schedule-foreign-budget-pressure>", &result);
    if (ok) {
        (void) script_runtime_collect_and_trim(runtime);
        size_t remaining = budget_remaining(&page_budget);
        size_t filler_length = remaining > pressure_remaining + 128u
            ? remaining - pressure_remaining - 128u : 0;
        pressure_filler = filler_length == 0 ? NULL
            : budget_malloc_category(
                  &page_budget, BUDGET_CATEGORY_RESOURCE, filler_length);
        ok = pressure_filler != NULL;
    }
    size_t cache_bytes_before = foreign_session.cache_bytes;
    size_t cache_evictions_before = foreign_session.cache_evictions;
    size_t cache_reclaimed_before =
        result.lazy_webpack_compile_cache_reclaimed_bytes;
    size_t admission_rejections_before =
        result.lazy_webpack_compile_admission_rejections;
    size_t compile_failures_before =
        result.lazy_webpack_factory_compile_failures;
    if (ok) {
        ok = script_runtime_advance(runtime, 0, 1, &result)
            && result.lazy_webpack_compile_admission_rejections
                   == admission_rejections_before + 1
            && result.lazy_webpack_factory_compile_failures
                   == compile_failures_before + 1
            && result.lazy_webpack_compile_cache_reclaimed_bytes
                   == cache_reclaimed_before
            && foreign_session.cache_bytes == cache_bytes_before
            && foreign_session.cache_evictions == cache_evictions_before;
    }
    budget_free(&page_budget, pressure_filler);
    pressure_filler = NULL;
    static const char verify_isolation[] =
        "globalThis.pocSummary=!globalThis.__foreignLazyUnexpected"
        "?'FOREIGN-CACHE-ISOLATED':'FOREIGN-CACHE-EVICTED'";
    ok = ok && script_runtime_evaluate_diagnostic(
        runtime, verify_isolation, "<verify-foreign-cache-isolation>", &result)
        && strcmp(result.summary, "FOREIGN-CACHE-ISOLATED") == 0;

    browser_shared_body_release(body);
    if (plan_ready) script_lazy_webpack_plan_destroy(&plan);
    free(large_source);
    budget_free(&page_budget, pressure_filler);
    script_runtime_destroy(runtime);
    if (document_ready) document_destroy(&document);
    if (session_ready) browser_session_destroy(&foreign_session);
    bool clean = page_budget.current == 0
        && session_budget.current == 0
        && budget_active_allocations(&page_budget, NULL) == 0
        && budget_active_allocations(&session_budget, NULL) == 0
        && budget_categories_reconcile(&page_budget)
        && budget_categories_reconcile(&session_budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&page_budget) && clean;
    }
    return ok && clean;
}

static int inspect_files(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb");
        if (file == NULL || fseek(file, 0, SEEK_END) != 0) return 2;
        long measured = ftell(file);
        if (measured <= 0 || fseek(file, 0, SEEK_SET) != 0) return 2;
        size_t length = (size_t) measured;
        char *source = malloc(length);
        if (source == NULL || fread(source, 1, length, file) != length) {
            free(source);
            fclose(file);
            return 2;
        }
        fclose(file);
        Budget budget;
        budget_init(&budget, 16u * 1024u * 1024u);
        ScriptLazyWebpackPlan plan;
        bool ok = script_lazy_webpack_plan_create(
            &budget, source, length, &plan);
        ScriptResourceLoaderPlan resource_loader;
        bool resource_loader_ok = script_resource_loader_plan_create(
            &budget, source, length, &resource_loader);
        printf("%s recognized=%s factories=%zu factory-bytes=%zu "
               "largest=%zu strict=%s resource-loader=%s statements=%zu "
               "rl-largest=%zu\n",
               argv[i], ok ? "yes" : "no",
               ok ? plan.factory_count : 0,
               ok ? plan.factory_source_bytes : 0,
               ok ? plan.largest_factory_bytes : 0,
               ok && plan.strict_mode ? "yes" : "no",
               resource_loader_ok ? "yes" : "no",
               resource_loader_ok ? resource_loader.statement_count : 0,
               resource_loader_ok
                   ? resource_loader.largest_statement_bytes : 0);
        if (resource_loader_ok) {
            for (size_t statement = 0;
                 statement < resource_loader.statement_count; statement++) {
                printf("  statement=%zu offset=%zu length=%zu\n", statement,
                       resource_loader.statements[statement].source_offset,
                       resource_loader.statements[statement].source_length);
            }
        }
        script_resource_loader_plan_destroy(&resource_loader);
        script_lazy_webpack_plan_destroy(&plan);
        free(source);
    }
    return 0;
}

/* The planner's exact output on bundles built to exercise its lexer: every
   operator it knows (longest match), regular expressions against division,
   template literals with nested substitutions, comments and strings holding
   braces, and gaps between keys and factories. Each factory's offsets,
   key, kind and arity are known by construction. The lexer's operator scan
   was rewritten as a first-character switch (script_lazy.c); these pin the
   factory boundaries it must keep finding. */
typedef struct {
    const char *key;
    const char *gap;
    const char *factory;
    ScriptLazyFactoryKind kind;
    uint8_t arity;
} SyntheticFactory;

static void test_planner_synthetic_identity(void)
{
    static const SyntheticFactory factories[] = {
        {"1", "", "(m,e)=>{let a=1,b=2,c=3;a>>>=1;b**=2;c&&=a;c||=b;"
         "c\x3f\x3f=a;a=a===b;a=a!==b;a=b>>>1;a<<=1;a>>=1;a=a==b;a=a!=b;"
         "a=a<=b;a=a>=b;a++;a--;a=a&&b;a=a||b;a=a\x3f\x3f" "b;a=a**b;"
         "a=a<<b;a=a>>b;a+=1;a-=1;a*=2;a/=2;a%=2;a&=1;a|=1;a^=1;"
         "e.x=m?.y;e.s=[...[a]];e.f=(...z)=>z;e.g=a<b?a>b:!a;e.h=~a}",
         SCRIPT_LAZY_FACTORY_ARROW, 2},
        {"22", " ", "function(m,e){var a=4,b=2,g=1;e.d=a/b/g;"
         "e.r=/}{/g.test('}');e.q=a/ /*}*/b;e.t=typeof /x{/;e.u=(a)/2;"
         "e.v=[1][0]/2;e.w=a++/2;e.k=g?/[}]/:/{/;e.n=a/=2;"
         "return /\\/}/.source}",
         SCRIPT_LAZY_FACTORY_FUNCTION, 2},
        {"abc", "/* } */ ", "m=>{const n=1;m.exports=`a${n}{b}${`in${n+1}}`}"
         "c${'}'}${({a:1}).a}`+`x\\`}`+`${`${`${n}`}`}`}",
         SCRIPT_LAZY_FACTORY_ARROW, 1},
        {"4", "\n// }\n", "function(m){// } brace\n/* } { */"
         "var s='}{\\'',t=\"}\\\"{\",u='a\\\nb';m.exports=s+t+u}",
         SCRIPT_LAZY_FACTORY_FUNCTION, 1},
        {"$five", "  ", "function(){return{a:{b:[{}]}}.a.b.length/1}",
         SCRIPT_LAZY_FACTORY_FUNCTION, 0},
        {"6", "", "()=>{if(1)/x/.test('');else{}return 6..toFixed(1)}",
         SCRIPT_LAZY_FACTORY_ARROW, 0}
    };
    static const char prefix[] =
        "\"use strict\";(self.webpackChunk_s=self.webpackChunk_s||[])"
        ".push([[7,\"x\"],{";
    static const char suffix[] = "}]);";
    size_t count = sizeof(factories) / sizeof(factories[0]);
    char source[2048];
    size_t used = 0;
    size_t key_offsets[8], offsets[8];
    used += (size_t) snprintf(source + used, sizeof(source) - used, "%s",
                              prefix);
    for (size_t i = 0; i < count; i++) {
        if (i != 0) source[used++] = ',';
        key_offsets[i] = used;
        used += (size_t) snprintf(source + used, sizeof(source) - used,
                                  "%s:%s", factories[i].key,
                                  factories[i].gap);
        offsets[i] = used;
        used += (size_t) snprintf(source + used, sizeof(source) - used,
                                  "%s", factories[i].factory);
    }
    used += (size_t) snprintf(source + used, sizeof(source) - used, "%s",
                              suffix);
    CHECK(used < sizeof(source) - 1);
    Budget budget;
    budget_init(&budget, 256u * 1024u);
    ScriptLazyWebpackPlan plan;
    CHECK(script_lazy_webpack_plan_create(&budget, source, used, &plan));
    CHECK(plan.factory_count == count && plan.strict_mode);
    size_t total = 0;
    for (size_t i = 0; i < count && i < plan.factory_count; i++) {
        const ScriptLazyFactory *factory = &plan.factories[i];
        size_t length = strlen(factories[i].factory);
        CHECK(factory->source_offset == offsets[i]
              && factory->source_length == length
              && factory->key_offset == key_offsets[i]
              && factory->key_length == strlen(factories[i].key)
              && factory->kind == factories[i].kind
              && factory->arity == factories[i].arity);
        if (factory->source_offset != offsets[i]
            || factory->source_length != length) {
            fprintf(stderr, "factory %zu: planned %zu+%zu, built %zu+%zu\n",
                    i, factory->source_offset, factory->source_length,
                    offsets[i], length);
        }
        total += length;
    }
    CHECK(plan.factory_source_bytes == total);
    script_lazy_webpack_plan_destroy(&plan);

    /* One byte off in each lexer path changes the outcome: an operator
       that is not one, a regular expression read as division, a template
       substitution left open. */
    static const char *const rejected[] = {
        /* `/` after `)` is division, so the regex's brace counts. */
        "(self.webpackChunk_s=self.webpackChunk_s||[]).push([[1],{"
        "1:()=>{if(a)/}/.test('')}}]);",
        "(self.webpackChunk_s=self.webpackChunk_s||[]).push([[1],{"
        "1:()=>{return `${`}]);",
        /* `=>` is required between the parameters and the body. */
        "(self.webpackChunk_s=self.webpackChunk_s||[]).push([[1],{"
        "1:(a)= >{}}]);"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        CHECK(!script_lazy_webpack_plan_create(
            &budget, rejected[i], strlen(rejected[i]), &plan));
    }
    CHECK(budget.current == 0);
}

/* Factory text outside the heap (js_lazy_webpack.c): a factory of 1 KiB or
   more compiles without its text in the QuickJS heap and its functions read
   it from the lazy tier. toString() stays exact (ASCII and not), a lazily
   compiled inner function compiles from it on its first call even after
   the plain copy was given up for room, and stacks are the ones the
   factory has with its text in the heap. */
#define EXT_PAD "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
#define EXT_EARLY "function early(a){var pad=\"" EXT_PAD "\";return a+pad.length}"
#define EXT_LATE "function late(b){var pad=\"" EXT_PAD "\";\n" \
    "  return \"late-\"+b+pad.length}"
#define EXT_WORD "function word(){var pad=\"" EXT_PAD "\";" \
    "return \"\xc3\xa9t\xc3\xa9\"+pad.length}"
#define EXT_THROWER "function thrower(){var pad=\"" EXT_PAD "\";\n" \
    "    throw new Error(\"boom\"+pad.length)}"
#define EXT_PAD8 EXT_PAD EXT_PAD EXT_PAD EXT_PAD \
    EXT_PAD EXT_PAD EXT_PAD EXT_PAD
#define EXT_FACTORY "function(module,exports){/*" EXT_PAD8 EXT_PAD8 \
    EXT_PAD8 EXT_PAD8 EXT_PAD8 EXT_PAD8 EXT_PAD8 EXT_PAD8 "*/\n" \
    "exports.early=" EXT_EARLY ";\n" \
    "exports.late=" EXT_LATE ";\n" \
    "exports.word=" EXT_WORD ";\n" \
    "exports.thrower=" EXT_THROWER ";\n" \
    "exports.firstEarly=exports.early(1)}"

static bool register_external_chunk_source(ScriptRuntime *runtime,
                                    lxb_dom_node_t *script,
                                    Budget *budget, const char *chunk,
                                    const char *factory_text, ScriptResult *result)
{
    static char source[32768];
    int written = snprintf(
        source, sizeof(source),
        "(self.%s=self.%s||[]).push([[6],{6:%s}]);",
        chunk, chunk, factory_text);
    if (written <= 0 || (size_t) written >= sizeof(source)) return false;
    char capture[256];
    snprintf(capture, sizeof(capture),
             "globalThis.%s=[];globalThis.%s.push=function(v){"
             "globalThis.__extModules=v[1]};",
             chunk, chunk);
    if (!script_runtime_evaluate_diagnostic(runtime, capture,
                                            "<install-ext-capture>", result))
        return false;
    ScriptLazyWebpackPlan plan;
    if (!plans(budget, source, &plan)) return false;
    bool ok = plan.factory_count == 1;
    BrowserSharedBody *body = shared_source(budget, source);
    ok = ok && body != NULL;
    if (ok) {
        ok = script_runtime_evaluate_external_lazy_webpack(
                 runtime, script, (const char *) body->data, body->length,
                 "https://example.test/ext.js", &plan, body,
                 release_shared_source, result)
            == SCRIPT_LAZY_EVALUATION_SUCCEEDED;
    } else if (body != NULL) {
        browser_shared_body_release(body);
    }
    script_lazy_webpack_plan_destroy(&plan);
    return ok;
}

static bool register_external_chunk(ScriptRuntime *runtime,
    lxb_dom_node_t *script, Budget *budget, const char *chunk, ScriptResult *result)
{
    return register_external_chunk_source(runtime, script, budget, chunk,
                                           EXT_FACTORY, result);
}

typedef struct {
    ScriptRuntime *runtime;
    const void *borrowed;
    size_t limit;
    unsigned calls;
    bool remained_owned;
    bool refuse;
} FactoryTextGrowth;

static size_t reclaim_factory_text(void *opaque, size_t live,
                                    size_t growth, size_t limit)
{
    (void) live; (void) growth; (void) limit;
    FactoryTextGrowth *probe = opaque;
    probe->calls++;
    (void) js_rt_source_store_relieve(probe->runtime, SIZE_MAX);
    probe->remained_owned = budget_owns(probe->runtime->budget,
                                        probe->borrowed);
    /* Refuse on old code rather than letting its dangling input be read. */
    return probe->remained_owned && !probe->refuse ? probe->limit : 0;
}

static void test_factory_text_allocation_case(ScriptRuntime *runtime,
    lxb_dom_node_t *script, Budget *budget, ScriptResult *result, bool ascii)
{
    char expected[] = EXT_FACTORY;
    if (ascii)
        for (size_t i = 0; i < sizeof(expected) - 1; i++)
            if ((unsigned char) expected[i] >= 0x80u) expected[i] = '?';
    CHECK(register_external_chunk_source(runtime, script, budget,
                                   "webpackChunk_reclaim", expected, result));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const m={exports:{}};__extModules[6](m,m.exports);"
        "globalThis.__reclaimExports=m.exports})()", "<retain-factory-text>", result));
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context);
    JSValue modules = JS_GetPropertyStr(context, global, "__extModules");
    JSValue wrapper = JS_GetPropertyUint32(context, modules, 6);
    JSValue to_string = JS_GetPropertyStr(context, wrapper, "toString");
    ScriptLazyRuntimeFactory *factory = NULL;
    for (ScriptLazyRuntimeBundle *b = runtime->lazy_webpack_bundles; b; b = b->next)
        for (size_t i = 0; i < b->factory_count; i++)
            if (JS_VALUE_GET_PTR(b->factories[i].wrapper) == JS_VALUE_GET_PTR(wrapper))
                factory = &b->factories[i];
    CHECK(factory != NULL && factory->plain != NULL && factory->external_owners != 0
        && JS_IsUndefined(factory->compiled));
    if (factory != NULL && factory->plain != NULL) {
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(runtime->runtime, &usage);
        FactoryTextGrowth probe = {runtime, factory->plain,
                                  (size_t)usage.malloc_limit, 0, false, false};
        budget_quickjs_pool_set_limit_growth(runtime->quickjs_pool,
                                              reclaim_factory_text, &probe);
        JS_SetMemoryLimit(runtime->runtime, (size_t)usage.malloc_size);
        JSValue text = JS_Call(context, to_string, wrapper, 0, NULL);
        JS_SetMemoryLimit(runtime->runtime, (size_t)usage.malloc_limit);
        budget_quickjs_pool_set_limit_growth(runtime->quickjs_pool,
                                              js_rt_heap_growth_hook, runtime);
        CHECK(probe.calls != 0 && probe.remained_owned && !JS_IsException(text));
        if (JS_IsException(text)) JS_FreeValue(context, JS_GetException(context));
        else {
            const char *source = JS_ToCString(context, text);
            CHECK(source != NULL && strcmp(source, expected) == 0);
            if (source != NULL) JS_FreeCString(context, source);
        }
        JS_FreeValue(context, text);
        /* A refused output allocation restores the borrowed cache too. */
        JS_ComputeMemoryUsage(runtime->runtime, &usage);
        probe.calls = 0; probe.remained_owned = false; probe.refuse = true;
        budget_quickjs_pool_set_limit_growth(runtime->quickjs_pool,
                                              reclaim_factory_text, &probe);
        JS_SetMemoryLimit(runtime->runtime, (size_t)usage.malloc_size);
        text = JS_Call(context, to_string, wrapper, 0, NULL);
        JS_SetMemoryLimit(runtime->runtime, (size_t)usage.malloc_limit);
        budget_quickjs_pool_set_limit_growth(runtime->quickjs_pool,
                                              js_rt_heap_growth_hook, runtime);
        CHECK(probe.calls != 0 && probe.remained_owned && JS_IsException(text)
            && factory->plain == probe.borrowed);
        if (JS_IsException(text)) JS_FreeValue(context, JS_GetException(context));
        JS_FreeValue(context, text);
    }
    JS_FreeValue(context, to_string); JS_FreeValue(context, wrapper);
    JS_FreeValue(context, modules); JS_FreeValue(context, global);
}

static void test_factory_text_allocation_reclaim(ScriptRuntime *runtime,
    lxb_dom_node_t *script, Budget *budget, ScriptResult *result)
{
    test_factory_text_allocation_case(runtime, script, budget, result, false);
    test_factory_text_allocation_case(runtime, script, budget, result, true);
}

static void test_factory_text_outside_heap(ScriptRuntime *runtime,
                                           lxb_dom_node_t *script,
                                           Budget *budget,
                                           ScriptResult *result)
{
    /* The same factory with its text in the heap (lab knob) gives the
       reference stack. */
    setenv("TILEFINCH_LAZY_WEBPACK_FACTORY_TEXT", "heap", 1);
    size_t external_before = result->lazy_webpack_external_text_compiles;
    CHECK(register_external_chunk(runtime, script, budget,
                                  "webpackChunk_extheap", result));
    static const char run_heap[] =
        "(()=>{const m={exports:{}};"
        "globalThis.__extModules[6](m,m.exports);"
        "globalThis.__extHeap=m.exports;"
        "try{m.exports.thrower()}catch(e){globalThis.__extHeapStack=e.stack}"
        "globalThis.pocSummary=m.exports.firstEarly===161"
        "&&m.exports.late(2)==='late-2160'"
        "?'EXT-HEAP-OK':'EXT-HEAP-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(runtime, run_heap,
                                             "<ext-heap>", result)
          && strcmp(result->summary, "EXT-HEAP-OK") == 0
          && result->lazy_webpack_external_text_compiles == external_before);
    unsetenv("TILEFINCH_LAZY_WEBPACK_FACTORY_TEXT");

    CHECK(register_external_chunk(runtime, script, budget,
                                  "webpackChunk_ext", result));
    static const char run_external[] =
        "(()=>{const m={exports:{}};"
        "globalThis.__extModules[6](m,m.exports);"
        "const e=globalThis.__ext=m.exports,t=Function.prototype.toString;"
        "globalThis.pocSummary=e.firstEarly===161"
        "&&t.call(e.early)===String.raw`" EXT_EARLY "`"
        "&&t.call(e.late)===String.raw`" EXT_LATE "`"
        "&&t.call(e.thrower)===String.raw`" EXT_THROWER "`"
        "?'EXT-TEXT-OK':'EXT-TEXT-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(runtime, run_external,
                                             "<ext-external>", result)
          && strcmp(result->summary, "EXT-TEXT-OK") == 0
          && result->lazy_webpack_external_text_compiles
                 == external_before + 1
          && result->lazy_webpack_external_text_read_failures == 0);

    /* Pressure: with almost no room left, the safe point gives up the plain
       copy. The functions not yet called then compile from text decoded
       out of the compressed copy, and toString() reads it the same way. */
    (void) script_runtime_collect_and_trim(runtime);
    size_t drops = result->lazy_webpack_external_text_plain_drops;
    size_t decodes = result->lazy_webpack_external_text_decodes;
    size_t remaining = budget_remaining(budget);
    /* room for a quarter of it to stay plain is less than the factory */
    size_t keep = 24u * 1024u;
    void *filler = remaining > keep
        ? budget_malloc_category(budget, BUDGET_CATEGORY_RESOURCE,
                                 remaining - keep)
        : NULL;
    CHECK(filler != NULL);
    CHECK(script_runtime_advance(runtime, 0, 1, result)
          && result->lazy_webpack_external_text_plain_drops > drops);
    budget_free(budget, filler);
    static const char run_after_pressure[] =
        "(()=>{const e=globalThis.__ext,h=globalThis.__extHeap,"
        "t=Function.prototype.toString;let stack='';"
        "try{e.thrower()}catch(x){stack=x.stack}"
        /* the frames above the caller's: the thrower's own */
        "const top=x=>x.split('\\n').slice(0,2).join('\\n');"
        "const failed=[e.late(3)==='late-3160',"
        "e.word()==='\\u00e9t\\u00e9160',"
        "t.call(e.word)===String.raw`" EXT_WORD "`,"
        "t.call(e.word)===t.call(h.word),"
        "t.call(e.early)===String.raw`" EXT_EARLY "`,"
        "stack!==''&&top(stack)===top(globalThis.__extHeapStack),"
        "/ext\\.js#factory-0:8:20\\)/.test(stack)]"
        ".map((ok,i)=>ok?'':i).join('');"
        "globalThis.pocSummary=failed===''?'EXT-PRESSURE-OK'"
        ":'EXT-PRESSURE-FAILED:'+failed})()";
    CHECK(script_runtime_evaluate_diagnostic(runtime, run_after_pressure,
                                             "<ext-after-pressure>", result)
          && strcmp(result->summary, "EXT-PRESSURE-OK") == 0);
    if (strcmp(result->summary, "EXT-PRESSURE-OK") != 0)
        fprintf(stderr, "summary: %s\n", result->summary);
    CHECK(result->lazy_webpack_external_text_decodes > decodes
          && result->lazy_webpack_external_text_decoded_bytes != 0
          && result->lazy_webpack_external_text_read_failures == 0);
}

int main(int argc, char **argv)
{
    if (argc > 1) return inspect_files(argc, argv);
    test_resource_loader_statement_planning();
    test_planner_synthetic_identity();
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    test_nested_template_depth_is_bounded(&budget);
    const char source[] =
        "\"use strict\";(self.webpackChunk_demo=self.webpackChunk_demo||[])"
        ".push([[1,2],{10:(a,b,c)=>{b.x=/[{}]/.test(\"x\");return `v${a}`},"
        "20:function(a){return a/2}}]);";
    ScriptLazyWebpackPlan plan;
    CHECK(plans(&budget, source, &plan));
    CHECK(plan.factory_count == 2);
    CHECK(plan.factories[0].kind == SCRIPT_LAZY_FACTORY_ARROW);
    CHECK(plan.factories[0].arity == 3);
    CHECK(plan.factories[1].kind == SCRIPT_LAZY_FACTORY_FUNCTION);
    CHECK(plan.factories[1].arity == 1);
    char *rendered = NULL;
    size_t rendered_length = 0;
    CHECK(script_lazy_webpack_render(
        &budget, source, strlen(source), &plan, 7,
        &rendered, &rendered_length));
    CHECK(rendered != NULL && rendered_length == strlen(rendered));
    CHECK(strstr(rendered, "__tilefinchLazyWebpackWrap(7,0)") != NULL);
    CHECK(strstr(rendered,
                 "__tilefinchLazyWebpackWrap(7,1,function($a){return "
                 "__tilefinchLazyWebpackInvoke(7,1,this,arguments)})")
          != NULL);
    CHECK(strstr(rendered, "b.x=/[{}]/") == NULL);
    budget_free(&budget, rendered);
    script_lazy_webpack_plan_destroy(&plan);

    /* A UTF-8 byte order mark before the registration (m.vk.com's chunks)
       still plans; the factory offsets stay offsets into the original
       bytes. */
    const char bom_source[] =
        "\xef\xbb\xbf\"use strict\";"
        "(self.webpackChunk_bom=self.webpackChunk_bom||[])"
        ".push([[3],{30:function(a,b){b.y=1}}]);";
    ScriptLazyWebpackPlan bom_plan;
    CHECK(plans(&budget, bom_source, &bom_plan));
    CHECK(bom_plan.factory_count == 1);
    CHECK(bom_plan.strict_mode);
    if (bom_plan.factory_count == 1) {
        CHECK(memcmp(bom_source + bom_plan.factories[0].source_offset,
                     "function(a,b)", 13) == 0);
    }
    script_lazy_webpack_plan_destroy(&bom_plan);

    static const char *const rejected[] = {
        "(self.notWebpack=self.notWebpack||[]).push([[1],{1:a=>{}}]);",
        "(self.webpackChunk_a=self.webpackChunk_b||[]).push([[1],{1:a=>{}}]);",
        "(self.webpackChunk_a=self.webpackChunk_a||[]).push([[1],{1:{x:1}}]);",
        "(self.webpackChunk_a=self.webpackChunk_a||[]).push([[1],{1:({a})=>{}}]);",
        "(self.webpackChunk_a=self.webpackChunk_a||[]).push([[1],{1:async a=>{}}]);",
        "(self.webpackChunk_a=self.webpackChunk_a||[]).push([[1],{1:a=>{}}]);x()",
        "(foo.webpackChunk_a=foo.webpackChunk_a||[]).push([[1],{1:a=>{}}]);",
        "(self.webpackChunk_a=self.webpackChunk_a||[]).push([[1],{1:a=>{]);"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ScriptLazyWebpackPlan rejected_plan;
        CHECK(!plans(&budget, rejected[i], &rejected_plan));
    }
    /* A large ordinary script must take the bounded prefix rejection path,
       without allocating a factory table or walking into the body. */
    size_t ordinary_length = 4u * 1024u * 1024u;
    char *ordinary = malloc(ordinary_length);
    CHECK(ordinary != NULL);
    if (ordinary != NULL) {
        memset(ordinary, 'x', ordinary_length);
        memcpy(ordinary, "(()=>{return 1})()", 18);
        ScriptLazyWebpackPlan ordinary_plan;
        size_t before = budget.current;
        CHECK(!script_lazy_webpack_plan_create(
            &budget, ordinary, ordinary_length, &ordinary_plan));
        CHECK(budget.current == before);
        free(ordinary);
    }
    /* Even a matching prefix cannot opt an unbounded source into planning. */
    size_t oversized_length = 9u * 1024u * 1024u;
    char *oversized = malloc(oversized_length);
    CHECK(oversized != NULL);
    if (oversized != NULL) {
        memset(oversized, ' ', oversized_length);
        static const char oversized_prefix[] =
            "(self.webpackChunk_x=self.webpackChunk_x||[])";
        memcpy(oversized, oversized_prefix, sizeof(oversized_prefix) - 1);
        ScriptLazyWebpackPlan oversized_plan;
        size_t before = budget.current;
        CHECK(!script_lazy_webpack_plan_create(
            &budget, oversized, oversized_length, &oversized_plan));
        CHECK(budget.current == before);
        free(oversized);
    }
    CHECK(budget.current == 0);
    CHECK(budget_categories_reconcile(&budget));

    Budget runtime_budget;
    budget_init(&runtime_budget, 16u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&runtime_budget));
    static const char html[] =
        "<!doctype html><html><body><script id=target></script></body></html>";
    PocDocument document;
    CHECK(document_parse(&document, &runtime_budget, html,
                         sizeof(html) - 1, 17));
    BrowserSession runtime_session;
    CHECK(browser_session_init(
        &runtime_session, &runtime_budget, 512u * 1024u));
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptRuntimeOptions options = {
        .session = &runtime_session,
        .viewport = viewport,
        .defer_document_scripts = true
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &runtime_budget, 8u * 1024u * 1024u, 3000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    lxb_dom_node_t *script = find_script(
        lxb_dom_interface_node(document.html));
    CHECK(script != NULL);
    static const char install_resource_event_probe[] =
        "(()=>{const target=document.getElementById('target');"
        "globalThis.__resourceErrorAtTarget=false;"
        "globalThis.__resourceErrorBubbled=0;"
        "target.addEventListener('error',event=>{"
        "globalThis.__resourceErrorAtTarget=event.bubbles===false});"
        "document.body.addEventListener('error',()=>{"
        "globalThis.__resourceErrorBubbled++});})()";
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, install_resource_event_probe,
        "<install-resource-event-probe>", &result));
    CHECK(script_runtime_dispatch_node(
        runtime, script, "error", &result));
    static const char verify_resource_event[] =
        "globalThis.pocSummary=globalThis.__resourceErrorAtTarget"
        "&&globalThis.__resourceErrorBubbled===0"
        "?'RESOURCE-ERROR-NONBUBBLING':'RESOURCE-ERROR-FAILED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify_resource_event,
              "<verify-resource-event>", &result)
          && strcmp(result.summary, "RESOURCE-ERROR-NONBUBBLING") == 0);
    static const char install_capture[] =
        "globalThis.webpackChunk_test=[];"
        "globalThis.webpackChunk_test.push=function(value){"
        "globalThis.__lazyModules=value[1];globalThis.__lazyOrder='push';};";
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, install_capture, "<install-chunk-capture>", &result));
    static const char lazy_source[] =
        "\"use strict\";"
        "(self.webpackChunk_test=self.webpackChunk_test||[]).push([[1],{"
        "1:(module,exports,require)=>{exports.answer=require(2)+1},"
        "2:module=>{module.exports=41},"
        "3:function(module,exports,require){"
        "exports.strictThis=this===undefined;exports.extra=arguments.length;"
        "exports.nested=function named(){return 7}}"
        "}]);";
    ScriptLazyWebpackPlan runtime_plan;
    CHECK(plans(&runtime_budget, lazy_source, &runtime_plan)
          && runtime_plan.factory_count == 3
          && runtime_plan.strict_mode);
    BrowserSharedBody *lazy_body = shared_source(
        &runtime_budget, lazy_source);
    CHECK(lazy_body != NULL);
    size_t basic_registration_compiles = result.host_compile_attempts;
    size_t basic_preflight_attempts =
        result.lazy_webpack_syntax_preflight_attempts;
    size_t basic_preflight_failures =
        result.lazy_webpack_syntax_preflight_failures;
    size_t basic_preflight_bytes =
        result.lazy_webpack_syntax_preflight_source_bytes;
    CHECK(script_runtime_evaluate_external_lazy_webpack(
              runtime, script, (const char *) lazy_body->data,
              lazy_body->length, "https://example.test/chunk.js",
              &runtime_plan, lazy_body, release_shared_source, &result)
          == SCRIPT_LAZY_EVALUATION_SUCCEEDED
          /* The registration only: no factory compiles before it runs. */
          && result.host_compile_attempts
                 == basic_registration_compiles + 1
          && result.lazy_webpack_syntax_preflight_attempts
                 == basic_preflight_attempts
          && result.lazy_webpack_syntax_preflight_failures
                 == basic_preflight_failures
          && result.lazy_webpack_syntax_preflight_source_bytes
                 == basic_preflight_bytes);
    script_lazy_webpack_plan_destroy(&runtime_plan);
    static const char invoke_modules[] =
        "(()=>{const modules=globalThis.__lazyModules,cache={};"
        "function require(id){if(cache[id])return cache[id].exports;"
        "const module=cache[id]={exports:{}};"
        "modules[id].call(undefined,module,module.exports,require);"
        "return module.exports}"
        "const one=require(1),three=require(3);"
        "globalThis.pocSummary=globalThis.__lazyOrder==='push'"
        "&&one.answer===42&&three.strictThis&&three.extra===3"
        "&&modules[1]===modules[1]&&!('prototype' in modules[1])"
        "&&('prototype' in modules[3])&&modules[1].length===3"
        "&&modules[3].length===3&&modules[1].name==='1'"
        "&&Function.prototype.toString.call(modules[1])"
        "==='(module,exports,require)=>{exports.answer=require(2)+1}'"
        "&&three.nested()===7"
        "&&Function.prototype.toString.call(three.nested)"
        ".includes('function named(){return 7}')"
        "?'LAZY-WEBPACK-OK':'LAZY-WEBPACK-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, invoke_modules, "<invoke-lazy-modules>", &result)
          && strcmp(result.summary, "LAZY-WEBPACK-OK") == 0
          && result.lazy_webpack_candidates == 1
          && result.lazy_webpack_applied == 1
          && result.lazy_webpack_fallbacks == 0
          && result.lazy_webpack_factories_deferred == 3
          && result.lazy_webpack_compressed_source_bytes != 0
          && result.lazy_webpack_bytecode_bytes == 0
          && result.lazy_webpack_compressed_bytecode_bytes == 0
          && result.lazy_webpack_source_compiles == 3
          && result.lazy_webpack_bytecode_restores == 0
          /* Exact cold source services Function#toString without compiling
             another short-lived factory object. */
          && result.lazy_webpack_factories_compiled == 3
          && result.lazy_webpack_compiled_factory_evictions == 3
          && result.lazy_webpack_factory_compile_failures == 0);
    static const char repeat_factory[] =
        "(()=>{const factory=globalThis.__lazyModules[1];"
        "const module={exports:{}};"
        "factory(module,module.exports,()=>41);"
        "globalThis.pocSummary=module.exports.answer===42"
        "&&Function.prototype.toString.call(factory)"
        ".includes('exports.answer=require(2)+1')"
        "?'LAZY-WEBPACK-REPEAT-OK':'LAZY-WEBPACK-REPEAT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, repeat_factory, "<repeat-lazy-factory>", &result)
          && strcmp(result.summary, "LAZY-WEBPACK-REPEAT-OK") == 0
          && result.lazy_webpack_source_compiles == 4
          && result.lazy_webpack_factories_compiled == 4
          && result.lazy_webpack_compiled_factory_evictions == 4
          && result.lazy_webpack_factory_compile_failures == 0);

    /* A syntactically large but semantically tiny factory proves registration
       retains only exact compressed source and compiles nothing. The long
       comment makes retained bytecode easy to detect. */
    static const char large_prefix[] =
        "(self.webpackChunk_test=self.webpackChunk_test||[]).push([[9],{"
        "9:function(module){/*";
    static const char large_suffix[] =
        "*/module.exports=99}}]);";
    const size_t large_comment_length = 270u * 1024u;
    const size_t large_source_length = sizeof(large_prefix) - 1
        + large_comment_length + sizeof(large_suffix) - 1;
    char *large_source = malloc(large_source_length + 1);
    CHECK(large_source != NULL);
    if (large_source != NULL) {
        size_t at = 0;
        memcpy(large_source + at, large_prefix, sizeof(large_prefix) - 1);
        at += sizeof(large_prefix) - 1;
        memset(large_source + at, 'a', large_comment_length);
        at += large_comment_length;
        memcpy(large_source + at, large_suffix, sizeof(large_suffix));
        ScriptLazyWebpackPlan large_plan;
        bool large_planned = plans(
            &runtime_budget, large_source, &large_plan);
        CHECK(large_planned && large_plan.factory_count == 1
              && large_plan.largest_factory_bytes
                     >= 256u * 1024u);
        BrowserSharedBody *large_body = shared_source(
            &runtime_budget, large_source);
        CHECK(large_body != NULL);
        if (large_planned && large_body != NULL) {
            size_t registration_compiles = result.host_compile_attempts;
            size_t preflight_attempts =
                result.lazy_webpack_syntax_preflight_attempts;
            size_t preflight_bytes =
                result.lazy_webpack_syntax_preflight_source_bytes;
            ScriptLazyEvaluation large_evaluation =
                script_runtime_evaluate_external_lazy_webpack(
                    runtime, script, (const char *) large_body->data,
                    large_body->length,
                    "https://example.test/large-chunk.js", &large_plan,
                    large_body, release_shared_source, &result);
            CHECK(large_evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
                  && result.lazy_webpack_candidates == 2
                  && result.lazy_webpack_applied == 2
                  && result.host_compile_attempts
                         == registration_compiles + 1
                  && result.lazy_webpack_syntax_preflight_attempts
                         == preflight_attempts
                  && result.lazy_webpack_syntax_preflight_source_bytes
                         == preflight_bytes
                  && result.lazy_webpack_source_compiles == 4
                  && result.lazy_webpack_bytecode_bytes == 0
                  && result.lazy_webpack_compressed_bytecode_bytes == 0);
            if (large_evaluation == SCRIPT_LAZY_EVALUATION_FALLBACK) {
                browser_shared_body_release(large_body);
            }
        } else if (large_body != NULL) {
            browser_shared_body_release(large_body);
        }
        if (large_planned) script_lazy_webpack_plan_destroy(&large_plan);
        free(large_source);
    }
    static const char invoke_large_factory[] =
        "(()=>{const factory=globalThis.__lazyModules[9],module={exports:{}};"
        "factory(module);"
        "globalThis.pocSummary=module.exports===99"
        "&&Function.prototype.toString.call(factory)"
        ".includes('module.exports=99')"
        "?'LAZY-LARGE-SOURCE-OK':'LAZY-LARGE-SOURCE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, invoke_large_factory,
              "<invoke-large-lazy-factory>", &result)
          && strcmp(result.summary, "LAZY-LARGE-SOURCE-OK") == 0
          && result.lazy_webpack_source_compiles == 5
          && result.lazy_webpack_bytecode_restores == 0
          && result.lazy_webpack_bytecode_demotions == 0
          && result.lazy_webpack_bytecode_bytes_released == 0
          && result.lazy_webpack_bytecode_bytes == 0
          && result.lazy_webpack_compressed_bytecode_bytes == 0
          && result.lazy_webpack_factories_compiled == 5
          && result.lazy_webpack_compiled_factory_evictions == 5
          && result.lazy_webpack_factory_compile_failures == 0);

    /* First-use admission must fail inside only the optional timer callback,
       reclaim response-cache state, and leave the realm usable. This creates
       real page-budget pressure rather than relying on a site-shaped fixture
       or a timing-sensitive allocator failure. */
    const size_t cache_payload_length = 128u * 1024u;
    unsigned char *cache_payload = malloc(cache_payload_length);
    CHECK(cache_payload != NULL);
    if (cache_payload != NULL) {
        memset(cache_payload, 'c', cache_payload_length);
        CHECK(browser_session_cache_put_response(
            &runtime_session, "https://example.test/optional-cache.bin",
            cache_payload, cache_payload_length, NULL, NULL,
            "application/octet-stream"));
        free(cache_payload);
    }
    static const char schedule_pressure_failure[] =
        "globalThis.__lazyPressureUnexpected=false;"
        "globalThis.__lazyLargeFactory=globalThis.__lazyModules[9];"
        "globalThis.__lazyPressureModule={exports:{}};"
        "setTimeout(()=>{globalThis.__lazyModules[9]("
        "globalThis.__lazyPressureModule);"
        "globalThis.__lazyPressureUnexpected=true},0)";
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, schedule_pressure_failure,
        "<schedule-lazy-pressure-failure>", &result));
    (void) script_runtime_collect_and_trim(runtime);
    size_t remaining_before_filler = budget_remaining(&runtime_budget);
    const size_t pressure_remaining = 512u * 1024u;
    size_t filler_length = remaining_before_filler > pressure_remaining + 128u
        ? remaining_before_filler - pressure_remaining - 128u : 0;
    void *pressure_filler = filler_length == 0 ? NULL
        : budget_malloc_category(&runtime_budget, BUDGET_CATEGORY_RESOURCE,
                                 filler_length);
    CHECK(pressure_filler != NULL);
    size_t admission_rejections_before =
        result.lazy_webpack_compile_admission_rejections;
    size_t cache_reclaimed_before =
        result.lazy_webpack_compile_cache_reclaimed_bytes;
    size_t compile_failures_before =
        result.lazy_webpack_factory_compile_failures;
    CHECK(pressure_filler != NULL
          && script_runtime_advance(runtime, 0, 1, &result)
          && result.lazy_webpack_compile_admission_rejections
                 == admission_rejections_before + 1
          && result.lazy_webpack_compile_cache_reclaimed_bytes
                 > cache_reclaimed_before
          && result.lazy_webpack_factory_compile_failures
                 == compile_failures_before + 1
          && strstr(result.last_uncaught_callback_error,
                    "out of memory") != NULL
          && runtime_session.cache_bytes == 0);
    budget_free(&runtime_budget, pressure_filler);
    static const char verify_pressure_isolation[] =
        "globalThis.pocSummary=!globalThis.__lazyPressureUnexpected"
        "?'LAZY-PRESSURE-ISOLATED':'LAZY-PRESSURE-ESCAPED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify_pressure_isolation,
              "<verify-lazy-pressure-isolation>", &result)
          && strcmp(result.summary, "LAZY-PRESSURE-ISOLATED") == 0
          && result.lazy_webpack_source_compiles == 5
          && result.lazy_webpack_factories_compiled == 5
          && result.lazy_webpack_compiled_factory_evictions == 5);

    /* A thousand neutral factories register with one compile (the
       registration) and no executable bytecode resident. Only the two used
       wrappers are compiled into callable functions later. */
    const size_t thousand_factory_count = 1000;
    const size_t thousand_capacity = 256u * 1024u;
    char *thousand_source = malloc(thousand_capacity);
    CHECK(thousand_source != NULL);
    bool thousand_built = thousand_source != NULL;
    size_t thousand_at = 0;
    if (thousand_built) {
        int written = snprintf(
            thousand_source, thousand_capacity,
            "(self.webpackChunk_test=self.webpackChunk_test||[]).push([[10],{");
        thousand_built = written > 0 && (size_t) written < thousand_capacity;
        if (thousand_built) thousand_at = (size_t) written;
    }
    for (size_t i = 0; thousand_built && i < thousand_factory_count; i++) {
        int written = snprintf(
            thousand_source + thousand_at, thousand_capacity - thousand_at,
            "%s%zu:(module)=>{module.exports=%zu;/*",
            i == 0 ? "" : ",", 10000u + i, i);
        if (written <= 0 || (size_t) written >= thousand_capacity - thousand_at) {
            thousand_built = false;
            break;
        }
        thousand_at += (size_t) written;
        if (thousand_capacity - thousand_at <= 99u) {
            thousand_built = false;
            break;
        }
        memset(thousand_source + thousand_at, (int) ('a' + i % 26u), 96u);
        thousand_at += 96u;
        memcpy(thousand_source + thousand_at, "*/}", 3u);
        thousand_at += 3u;
    }
    if (thousand_built && thousand_capacity - thousand_at > 5u) {
        memcpy(thousand_source + thousand_at, "}]);", 5u);
        thousand_at += 5u;
        thousand_source[thousand_at] = '\0';
    } else {
        thousand_built = false;
    }
    ScriptLazyWebpackPlan thousand_plan;
    bool thousand_planned = thousand_built
        && plans(&runtime_budget, thousand_source, &thousand_plan);
    CHECK(thousand_planned
          && thousand_plan.factory_count == thousand_factory_count);
    BrowserSharedBody *thousand_body = thousand_planned
        ? shared_source(&runtime_budget, thousand_source) : NULL;
    CHECK(thousand_body != NULL);
    if (thousand_planned && thousand_body != NULL) {
        size_t registration_compiles = result.host_compile_attempts;
        size_t preflight_attempts =
            result.lazy_webpack_syntax_preflight_attempts;
        size_t preflight_failures =
            result.lazy_webpack_syntax_preflight_failures;
        size_t preflight_bytes =
            result.lazy_webpack_syntax_preflight_source_bytes;
        ScriptLazyEvaluation thousand_evaluation =
            script_runtime_evaluate_external_lazy_webpack(
                runtime, script, (const char *) thousand_body->data,
                thousand_body->length,
                "https://example.test/thousand-chunk.js", &thousand_plan,
                thousand_body, release_shared_source, &result);
        CHECK(thousand_evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
              && result.lazy_webpack_candidates == 3
              && result.lazy_webpack_applied == 3
              && result.lazy_webpack_factories_deferred == 1004
              && result.host_compile_attempts
                     == registration_compiles + 1
              && result.lazy_webpack_syntax_preflight_attempts
                     == preflight_attempts
              && result.lazy_webpack_syntax_preflight_failures
                     == preflight_failures
              && result.lazy_webpack_syntax_preflight_source_bytes
                     == preflight_bytes
              && result.lazy_webpack_source_compiles == 5
              && result.lazy_webpack_bytecode_bytes == 0
              && result.lazy_webpack_compressed_bytecode_bytes == 0
              && result.lazy_webpack_compressed_source_bytes
                     <= result.lazy_webpack_resident_source_limit_bytes
              && result.lazy_webpack_residency_rejections == 0);
        if (thousand_evaluation == SCRIPT_LAZY_EVALUATION_FALLBACK) {
            browser_shared_body_release(thousand_body);
        }
    } else if (thousand_body != NULL) {
        browser_shared_body_release(thousand_body);
    }
    if (thousand_planned) script_lazy_webpack_plan_destroy(&thousand_plan);
    free(thousand_source);
    static const char invoke_thousand_factories[] =
        "(()=>{const modules=globalThis.__lazyModules;"
        "const first={exports:{}},last={exports:{}};"
        "modules[10000](first);modules[10999](last);"
        "globalThis.pocSummary=first.exports===0&&last.exports===999"
        "?'LAZY-THOUSAND-OK':'LAZY-THOUSAND-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, invoke_thousand_factories,
              "<invoke-thousand-lazy-factories>", &result)
          && strcmp(result.summary, "LAZY-THOUSAND-OK") == 0
          && result.lazy_webpack_source_compiles == 7
          && result.lazy_webpack_factories_compiled == 7
          && result.lazy_webpack_compiled_factory_evictions == 7
          && result.lazy_webpack_factory_compile_failures == 1);

        /* A lexically well-formed factory with invalid grammar registers like
       any other (nothing compiles it before it runs, so nothing finds the
       error); calling it throws the SyntaxError, at the bundle position,
       and leaves the realm usable. */
    static const char invalid_source[] =
        "(self.webpackChunk_bad=self.webpackChunk_bad||[]).push([[2],{"
        "4:(a,b,c)=>{const = 1}}]);";
    ScriptLazyWebpackPlan invalid_plan;
    CHECK(plans(&runtime_budget, invalid_source, &invalid_plan));
    BrowserSharedBody *invalid_body = shared_source(
        &runtime_budget, invalid_source);
    CHECK(invalid_body != NULL);
    size_t invalid_registration_compiles = result.host_compile_attempts;
    size_t invalid_preflight_attempts =
        result.lazy_webpack_syntax_preflight_attempts;
    size_t invalid_preflight_failures =
        result.lazy_webpack_syntax_preflight_failures;
    ScriptLazyEvaluation invalid_evaluation =
        script_runtime_evaluate_external_lazy_webpack(
            runtime, script, (const char *) invalid_body->data,
            invalid_body->length, "https://example.test/invalid-chunk.js",
            &invalid_plan, invalid_body, release_shared_source, &result);
    CHECK(invalid_evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED
          && result.lazy_webpack_candidates == 4
          && result.lazy_webpack_applied == 4
          && result.lazy_webpack_fallbacks == 0
          && result.host_compile_attempts
                 == invalid_registration_compiles + 1
          && result.lazy_webpack_syntax_preflight_attempts
                 == invalid_preflight_attempts
          && result.lazy_webpack_syntax_preflight_failures
                 == invalid_preflight_failures
          && result.lazy_webpack_source_compiles == 7
          && result.lazy_webpack_factory_compile_failures == 1);
    if (invalid_evaluation == SCRIPT_LAZY_EVALUATION_FALLBACK)
        browser_shared_body_release(invalid_body);
    script_lazy_webpack_plan_destroy(&invalid_plan);
    static const char verify_invalid_factory_throws[] =
        "(()=>{const f=globalThis.webpackChunk_bad[0][1][4];let r='NONE';"
        "try{f(1,2,3)}catch(e){r=(e instanceof SyntaxError)+':'+e.fileName"
        "+':'+e.lineNumber}"
        "globalThis.pocSummary=r})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify_invalid_factory_throws,
              "<verify-invalid-lazy-factory>", &result)
          && strcmp(result.summary,
                    "true:https://example.test/invalid-chunk.js:1") == 0
          && result.lazy_webpack_source_compiles == 7
          && result.lazy_webpack_factory_compile_failures == 2);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary='LAZY-AFTER-FAILURE-OK'",
              "<verify-lazy-after-failure>", &result)
          && strcmp(result.summary, "LAZY-AFTER-FAILURE-OK") == 0);

    /* Invalid author calls must not turn deferred factories back into a
       resident bytecode set. Exercise distinct traditional-function wrappers,
       whose generated forwarding path owns the bounded argument copy. */
    const size_t rejected_factory_count = 32;
    const size_t rejected_source_capacity = 16u * 1024u;
    char *rejected_source = malloc(rejected_source_capacity);
    CHECK(rejected_source != NULL);
    bool rejected_built = rejected_source != NULL;
    size_t rejected_at = 0;
    if (rejected_built) {
        int written = snprintf(
            rejected_source, rejected_source_capacity,
            "(self.webpackChunk_test=self.webpackChunk_test||[]).push([[12],{");
        rejected_built = written > 0
            && (size_t) written < rejected_source_capacity;
        if (rejected_built) rejected_at = (size_t) written;
    }
    for (size_t i = 0; rejected_built && i < rejected_factory_count; i++) {
        int written = snprintf(
            rejected_source + rejected_at,
            rejected_source_capacity - rejected_at,
            "%s%zu:function(module){module.exports=%zu}",
            i == 0 ? "" : ",", 20000u + i, i);
        if (written <= 0
            || (size_t) written >= rejected_source_capacity - rejected_at) {
            rejected_built = false;
            break;
        }
        rejected_at += (size_t) written;
    }
    if (rejected_built && rejected_source_capacity - rejected_at > 5u) {
        memcpy(rejected_source + rejected_at, "}]);", 5u);
        rejected_at += 4u;
    } else {
        rejected_built = false;
    }
    ScriptLazyWebpackPlan rejected_plan = {0};
    bool rejected_planned = rejected_built
        && script_lazy_webpack_plan_create(
               &runtime_budget, rejected_source, rejected_at, &rejected_plan);
    CHECK(rejected_planned
          && rejected_plan.factory_count == rejected_factory_count);
    BrowserSharedBody *rejected_body = rejected_planned
        ? shared_source(&runtime_budget, rejected_source) : NULL;
    CHECK(rejected_body != NULL);
    bool rejected_applied = false;
    if (rejected_planned && rejected_body != NULL) {
        ScriptLazyEvaluation evaluation =
            script_runtime_evaluate_external_lazy_webpack(
                runtime, script, (const char *) rejected_body->data,
                rejected_body->length,
                "https://example.test/rejected-calls-chunk.js",
                &rejected_plan, rejected_body, release_shared_source, &result);
        rejected_applied = evaluation == SCRIPT_LAZY_EVALUATION_SUCCEEDED;
        if (evaluation != SCRIPT_LAZY_EVALUATION_FALLBACK) {
            rejected_body = NULL;
        }
    }
    CHECK(rejected_applied);
    size_t rejected_compiles_before =
        result.lazy_webpack_factories_compiled;
    size_t rejected_evictions_before =
        result.lazy_webpack_compiled_factory_evictions;
    static const char invoke_rejected_factories[] =
        "(()=>{const modules=globalThis.__lazyModules,args=Array(17);"
        "let rejected=0;for(let i=0;i<32;i++){try{"
        "modules[20000+i](...args)}catch(error){"
        "if(error instanceof RangeError)rejected++}}"
        "globalThis.pocSummary=rejected===32"
        "?'LAZY-INVALID-ARGS-ISOLATED':'LAZY-INVALID-ARGS-FAILED:'+rejected})()";
    if (rejected_applied) {
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime, invoke_rejected_factories,
                  "<invoke-invalid-lazy-arguments>", &result)
              && strcmp(result.summary, "LAZY-INVALID-ARGS-ISOLATED") == 0
              && result.lazy_webpack_factories_compiled
                     == rejected_compiles_before + rejected_factory_count
              && result.lazy_webpack_compiled_factory_evictions
                     == rejected_evictions_before + rejected_factory_count);
    }
    browser_shared_body_release(rejected_body);
    if (rejected_planned) script_lazy_webpack_plan_destroy(&rejected_plan);
    free(rejected_source);

    /* First-use admission plans on the measured model whole scripts are
       admitted with (twice the source plus parser state, and the execution
       reserve beside it), not four times the source. With the page Budget
       between the two requirements the factory compiles and runs. */
    {
        const size_t factory_bytes = large_comment_length;
        size_t measured = factory_bytes + 32u
            + script_admission_compile_peak(factory_bytes)
            + SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES;
        size_t four_times = factory_bytes + 32u + factory_bytes * 4u
            + 256u * 1024u + 512u * 1024u;
        CHECK(four_times > measured + 256u * 1024u);
        static const char schedule_measured[] =
            "globalThis.__lazyMeasuredModule={exports:{}};"
            "setTimeout(()=>{globalThis.__lazyLargeFactory("
            "globalThis.__lazyMeasuredModule)},0)";
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, schedule_measured, "<schedule-lazy-measured>",
            &result));
        (void) script_runtime_collect_and_trim(runtime);
        size_t target = measured + (four_times - measured) / 2u;
        size_t remaining = budget_remaining(&runtime_budget);
        size_t filler_bytes = remaining > target ? remaining - target : 0;
        void *filler = filler_bytes == 0 ? NULL
            : budget_malloc_category(&runtime_budget,
                                     BUDGET_CATEGORY_RESOURCE, filler_bytes);
        CHECK(filler != NULL);
        size_t rejections = result.lazy_webpack_compile_admission_rejections;
        size_t compile_failures =
            result.lazy_webpack_factory_compile_failures;
        CHECK(filler != NULL
              && script_runtime_advance(runtime, 0, 1, &result)
              && result.lazy_webpack_compile_admission_rejections
                     == rejections
              && result.lazy_webpack_factory_compile_failures
                     == compile_failures);
        budget_free(&runtime_budget, filler);
        static const char verify_measured[] =
            "globalThis.pocSummary="
            "globalThis.__lazyMeasuredModule.exports===99"
            "?'LAZY-MEASURED-ADMITTED':'LAZY-MEASURED-REFUSED'";
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime, verify_measured, "<verify-lazy-measured>",
                  &result)
              && strcmp(result.summary, "LAZY-MEASURED-ADMITTED") == 0);
    }

    test_factory_text_outside_heap(runtime, script, &runtime_budget,
                                   &result);
    test_factory_text_allocation_reclaim(runtime, script, &runtime_budget,
                                         &result);

    /* A registration that throws (here in the page's chunk push) still
       ran: the window error event reports its exception, then the element
       gets `load`, not `error` (HTML "execute the script element"). */
    static const char install_throwing_push[] =
        "(()=>{const target=document.getElementById('target');"
        "const events=globalThis.__lazyThrowEvents=[];"
        "globalThis.webpackChunk_boom=[];"
        "globalThis.webpackChunk_boom.push=()=>{throw new Error('push-boom')};"
        "onerror=m=>{events.push('on:'+m);return true};"
        "target.onload=()=>events.push('load');"
        "target.onerror=()=>events.push('error');})()";
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, install_throwing_push, "<install-throwing-push>", &result));
    static const char throwing_source[] =
        "(self.webpackChunk_boom=self.webpackChunk_boom||[]).push([[4],{"
        "4:(module,exports,require)=>{exports.four=require(5)},"
        "5:module=>{module.exports=5}"
        "}]);";
    ScriptLazyWebpackPlan throwing_plan;
    bool throwing_planned = plans(
        &runtime_budget, throwing_source, &throwing_plan);
    CHECK(throwing_planned && throwing_plan.factory_count == 2);
    BrowserSharedBody *throwing_body = shared_source(
        &runtime_budget, throwing_source);
    CHECK(throwing_body != NULL);
    if (throwing_planned && throwing_body != NULL) {
        CHECK(script_runtime_evaluate_external_lazy_webpack(
                  runtime, script, (const char *) throwing_body->data,
                  throwing_body->length, "https://example.test/boom.js",
                  &throwing_plan, throwing_body, release_shared_source,
                  &result)
              == SCRIPT_LAZY_EVALUATION_FAILED);
        /* Its own exception, so the parser's failure breaker leaves it
           uncounted. */
        CHECK(script_runtime_last_script_author_error(runtime));
        static const char verify_throwing[] =
            "globalThis.pocSummary=globalThis.__lazyThrowEvents.join(',');"
            "onerror=null";
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime, verify_throwing, "<verify-throwing-push>",
                  &result)
              && strcmp(result.summary, "on:push-boom,load") == 0);
    } else if (throwing_body != NULL) {
        release_shared_source(throwing_body);
    }
    if (throwing_planned) script_lazy_webpack_plan_destroy(&throwing_plan);

    script_runtime_destroy(runtime);
    document_destroy(&document);
    browser_session_destroy(&runtime_session);
    CHECK(runtime_budget.current == 0
          && budget_active_allocations(&runtime_budget, NULL) == 0
          && budget_categories_reconcile(&runtime_budget));
    CHECK(budget_uninstall_lexbor(&runtime_budget));
    CHECK(test_foreign_session_cache_is_not_reclaimed());
    if (failures != 0) return 1;
    puts("script lazy tests passed");
    return 0;
}
