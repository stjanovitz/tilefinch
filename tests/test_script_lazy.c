#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/session.h"
#include "tilefinch/viewport.h"

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

int main(int argc, char **argv)
{
    if (argc > 1) return inspect_files(argc, argv);
    test_resource_loader_statement_planning();
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
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
          && result.host_compile_attempts
                 == basic_registration_compiles + 4
          && result.lazy_webpack_syntax_preflight_attempts
                 == basic_preflight_attempts + 3
          && result.lazy_webpack_syntax_preflight_failures
                 == basic_preflight_failures
          && result.lazy_webpack_syntax_preflight_source_bytes
                 == basic_preflight_bytes
                      + runtime_plan.factory_source_bytes);
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
       retains only exact compressed source after the mandatory grammar
       preflight. The long comment makes retained bytecode easy to detect. */
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
                         == registration_compiles + 2
                  && result.lazy_webpack_syntax_preflight_attempts
                         == preflight_attempts + 1
                  && result.lazy_webpack_syntax_preflight_source_bytes
                         == preflight_bytes
                              + large_plan.factory_source_bytes
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

    /* A thousand neutral factories expose the unavoidable syntax-preflight
       CPU separately while proving no executable bytecode remains resident.
       Only the two used wrappers are compiled into callable functions later. */
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
                     == registration_compiles + thousand_factory_count + 1
              && result.lazy_webpack_syntax_preflight_attempts
                     == preflight_attempts + thousand_factory_count
              && result.lazy_webpack_syntax_preflight_failures
                     == preflight_failures
              && result.lazy_webpack_syntax_preflight_source_bytes
                     == preflight_bytes
                          + thousand_plan.factory_source_bytes
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

    /* A lexically well-formed factory with invalid grammar must fail the
       one-at-a-time preflight before registration can call push. */
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
    CHECK(script_runtime_evaluate_external_lazy_webpack(
          runtime, script, (const char *) invalid_body->data,
              invalid_body->length, "https://example.test/invalid-chunk.js",
              &invalid_plan, invalid_body, release_shared_source, &result)
          == SCRIPT_LAZY_EVALUATION_FALLBACK
          && result.lazy_webpack_candidates == 4
          && result.lazy_webpack_applied == 3
          && result.lazy_webpack_fallbacks == 1
          && result.host_compile_attempts
                 == invalid_registration_compiles + 1
          && result.lazy_webpack_syntax_preflight_attempts
                 == invalid_preflight_attempts + 1
          && result.lazy_webpack_syntax_preflight_failures
                 == invalid_preflight_failures + 1
          && result.lazy_webpack_source_compiles == 7
          && result.lazy_webpack_factory_compile_failures == 1);
    browser_shared_body_release(invalid_body);
    script_lazy_webpack_plan_destroy(&invalid_plan);
    static const char verify_no_invalid_effect[] =
        "globalThis.pocSummary=typeof globalThis.webpackChunk_bad==='undefined'"
        "?'LAZY-PREFLIGHT-CLEAN':'LAZY-PREFLIGHT-DIRTY'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify_no_invalid_effect,
              "<verify-invalid-lazy-fallback>", &result)
          && strcmp(result.summary, "LAZY-PREFLIGHT-CLEAN") == 0);
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
