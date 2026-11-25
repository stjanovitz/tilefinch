/* Unused variable tables (bootstrap-unused-locals layer).
 *
 * JS_WRITE_OBJ_STRIP_UNUSED_LOCALS, which only the bootstrap generator
 * passes, omits the variable table of a stripped function whose locals
 * nothing captures. Checked here:
 *  - an eligible function loses its table, in ordinary and ROM reads, and
 *    still runs, throws and reports its lexical-variable errors as before;
 *  - functions whose table has a reader (captured locals, mapped
 *    arguments, direct eval, kept names) are written byte for byte as
 *    without the flag, and their captures, eval and errors work;
 *  - without the flag (every page-script writer) nothing is omitted;
 *  - generator, async, sloppy-arguments, `with`, indirect-eval, class and
 *    default-argument shapes run the same stripped, each eligible or not
 *    as stated;
 *  - the reader refuses a record whose table length does not match its
 *    slots, or that has no table but a captured local;
 *  - in a browser realm, bootstrap closures with captures work and errors
 *    thrown in bootstrap code reach page script with their names. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"

/* Absent before this layer: the writer then ignores the bit, the table
   stays, and the size checks below fail. */
#ifndef JS_WRITE_OBJ_STRIP_UNUSED_LOCALS
#define JS_WRITE_OBJ_STRIP_UNUSED_LOCALS (1 << 5)
#endif

#define MIB (1024u * 1024u)
#define CHECK(c) do { if (!(c)) {                                          \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);   \
    return false; } } while (0)

static bool result_is(JSContext *ctx, JSValue value, const char *expected)
{
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        fprintf(stderr, "unused locals: exception %s\n", text ? text : "?");
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, error);
        return false;
    }
    const char *text = JS_ToCString(ctx, value);
    bool okay = text && strcmp(text, expected) == 0;
    if (!okay)
        fprintf(stderr, "unused locals: got %s, wanted %s\n",
                text ? text : "?", expected);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return okay;
}

/* Every function here captures nothing and makes no direct eval: the
   leaf's 2 arguments, 40 locals and its (strict, unmapped) `arguments`,
   the thrower's argument and local, the TDZ function's two lexical locals,
   the catcher's argument and catch binding. */
static const char eligible_source[] =
    "function leaf(a,b){'use strict';let v0=a+1,v1=v0+b,v2=v1*2,v3=v2-a,v4=v3+b,"
    "v5=v4,v6=v5,v7=v6,v8=v7,v9=v8,v10=v9,v11=v10,v12=v11,v13=v12,"
    "v14=v13,v15=v14,v16=v15,v17=v16,v18=v17,v19=v18,v20=v19,v21=v20,"
    "v22=v21,v23=v22,v24=v23,v25=v24,v26=v25,v27=v26,v28=v27,v29=v28,"
    "v30=v29,v31=v30,v32=v31,v33=v32,v34=v33,v35=v34,v36=v35,v37=v36,"
    "v38=v37,v39=v38;return v39+arguments.length}"
    "function thrower(kind){'use strict';let detail='bad '+kind;"
    "throw new TypeError(detail)}"
    "function temporal(){let x=y+1;let y=2;return x}"
    "function caught(f){try{f();return 'none'}"
    "catch(e){return e.name+':'+e.message}}"
    "[leaf(3,7),caught(()=>thrower('input')),caught(temporal)].join('|')";
static const char eligible_expected[] =
    "28|TypeError:bad input|ReferenceError:lexical variable is not initialized";

/* Every function with locals here has a reader of its table. */
static const char retained_source[] =
    "function counter(start){let value=start;return ()=>++value}"
    "function mapped(a){arguments[0]=7;return a}"
    "function direct(a){let local=3;return eval('a+local')}"
    "function loops(){let out=[];for(let i=0;i<3;i++)out.push(()=>i);"
    "let s='';for(const f of out)s+=f();return s}"
    "function* gen(a){let x=a;yield ()=>++x;return x}"
    "function outer(a){function inner(){return eval('a')}return inner()}"
    "var c=counter(9),g=gen(1),bump=g.next().value;bump();"
    "[c(),c(),mapped(1),direct(2),loops(),g.next().value,outer(5)]"
    ".join('|')";
static const char retained_expected[] = "10|11|7|5|012|2|5";

typedef struct {
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *rt;
    JSContext *ctx;
} Engine;

static bool engine_open(Engine *engine, int strip)
{
    memset(engine, 0, sizeof(*engine));
    budget_init(&engine->budget, 16u * MIB);
    engine->pool = budget_quickjs_pool_create(&engine->budget);
    CHECK(engine->pool);
    engine->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), engine->pool);
    CHECK(engine->rt);
    JS_SetMemoryLimit(engine->rt, 8u * MIB);
    JS_SetMaxStackSize(engine->rt, MIB);
    JS_SetStripInfo(engine->rt, strip);
    engine->ctx = JS_NewContext(engine->rt);
    CHECK(engine->ctx);
    return true;
}

static bool engine_close(Engine *engine)
{
    JS_FreeContext(engine->ctx);
    JS_FreeRuntime(engine->rt);
    (void) budget_quickjs_pool_trim(engine->pool, 0);
    CHECK(budget_quickjs_pool_destroy(engine->pool));
    CHECK(engine->budget.current == 0);
    return true;
}

static uint8_t *compile_and_write(Engine *engine, const char *source,
                                  size_t length, int flags, size_t *size)
{
    JSValue compiled = JS_Eval(engine->ctx, source, length, "<unused-locals>",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return NULL;
    uint8_t *bytes = JS_WriteObject(engine->ctx, size, compiled,
                                    JS_WRITE_OBJ_BYTECODE | flags);
    JS_FreeValue(engine->ctx, compiled);
    return bytes;
}

/* Reads `bytes`, reports the function records' bytes while the program is
   held, then runs it and compares its result. */
static bool read_and_run(Engine *engine, const uint8_t *bytes, size_t size,
                         bool rom, const char *expected, int64_t *func_size)
{
    JSMemoryUsage before, after;
    JS_RunGC(engine->rt);
    JS_ComputeMemoryUsage(engine->rt, &before);
    JSValue program = JS_ReadObject(engine->ctx, bytes, size,
        JS_READ_OBJ_BYTECODE | (rom ? JS_READ_OBJ_ROM_DATA : 0));
    CHECK(!JS_IsException(program));
    JS_ComputeMemoryUsage(engine->rt, &after);
    *func_size = after.js_func_size - before.js_func_size;
    return result_is(engine->ctx, JS_EvalFunction(engine->ctx, program),
                     expected);
}

static bool run_engine_checks(void)
{
    Engine engine;
    CHECK(engine_open(&engine, JS_STRIP_SOURCE | JS_STRIP_DEBUG));
    size_t plain_size = 0, stripped_size = 0;
    uint8_t *plain = compile_and_write(&engine, eligible_source,
        sizeof(eligible_source) - 1, 0, &plain_size);
    uint8_t *stripped = compile_and_write(&engine, eligible_source,
        sizeof(eligible_source) - 1, JS_WRITE_OBJ_STRIP_UNUSED_LOCALS,
        &stripped_size);
    CHECK(plain && stripped);
    /* The serialized tables are gone: at least the 49 entries named
       above, 3 bytes or more each. */
    CHECK(stripped_size + 3u * 49u <= plain_size);
    for (int rom = 0; rom <= 1; rom++) {
        int64_t plain_func = 0, stripped_func = 0;
        CHECK(read_and_run(&engine, plain, plain_size, rom,
                           eligible_expected, &plain_func));
        CHECK(read_and_run(&engine, stripped, stripped_size, rom,
                           eligible_expected, &stripped_func));
        /* 12 bytes per entry on 32- and 64-bit targets alike. */
        printf("unused locals: eligible function records %lld -> %lld "
               "bytes (%s read)\n", (long long) plain_func,
               (long long) stripped_func, rom ? "ROM" : "ordinary");
        CHECK(plain_func - stripped_func >= 12 * 49
              && (plain_func - stripped_func) % 12 == 0);
    }
    js_free(engine.ctx, plain);
    js_free(engine.ctx, stripped);

    /* Tables that have a reader are written unchanged. */
    plain = compile_and_write(&engine, retained_source,
        sizeof(retained_source) - 1, 0, &plain_size);
    stripped = compile_and_write(&engine, retained_source,
        sizeof(retained_source) - 1, JS_WRITE_OBJ_STRIP_UNUSED_LOCALS,
        &stripped_size);
    CHECK(plain && stripped);
    /* Each function with arguments or locals has a captured one or a
       direct eval, and keeps its table. Only the script's own top-level
       function, whose one local (its completion value) nothing captures,
       loses one entry. */
    CHECK(stripped_size < plain_size && plain_size - stripped_size <= 8u);
    for (int rom = 0; rom <= 1; rom++) {
        int64_t plain_func = 0, stripped_func = 0;
        CHECK(read_and_run(&engine, plain, plain_size, rom,
                           retained_expected, &plain_func));
        CHECK(read_and_run(&engine, stripped, stripped_size, rom,
                           retained_expected, &stripped_func));
        CHECK(plain_func - stripped_func == 12);
    }
    js_free(engine.ctx, plain);
    js_free(engine.ctx, stripped);
    CHECK(engine_close(&engine));

    /* Kept names (no JS_STRIP_DEBUG) are a reader: nothing is omitted, so
       a debug build's bytecode is the same with or without the flag. */
    CHECK(engine_open(&engine, JS_STRIP_SOURCE));
    plain = compile_and_write(&engine, eligible_source,
        sizeof(eligible_source) - 1, 0, &plain_size);
    stripped = compile_and_write(&engine, eligible_source,
        sizeof(eligible_source) - 1, JS_WRITE_OBJ_STRIP_UNUSED_LOCALS,
        &stripped_size);
    CHECK(plain && stripped);
    CHECK(plain_size == stripped_size
          && memcmp(plain, stripped, plain_size) == 0);
    js_free(engine.ctx, plain);
    js_free(engine.ctx, stripped);
    CHECK(engine_close(&engine));
    return true;
}

/* More function shapes. Each script leaves its answer in `r`. `entries` is
   how many table entries the flag drops, which says whether the shape is
   eligible: the script's top-level function drops its one local (the
   completion value) unless something captures a top-level binding. */
typedef struct {
    const char *source;
    const char *expected;
    int entries;
} Shape;

static const Shape shapes[] = {
    /* Generator: its frame is kept between resumptions, but nothing
       captures `a` or `x`. Eligible: 2 + top level. */
    { "function* g(a){let x=a+1;yield x;return x+1}"
      "var it=g(1);r=[it.next().value,it.next().value].join()",
      "2,3", 3 },
    /* Async function: as a generator. Eligible: 2, `then` callback's `v`,
       top level. */
    { "async function h(a){let x=await a;return x+1}"
      "h(5).then(v=>{r=v})", "6", 4 },
    /* Sloppy `arguments` with no declared arguments: mapped, but maps
       nothing. Eligible: the `arguments` local + top level. */
    { "function z(){return arguments.length+arguments[1]}r=z(1,2,3)",
      "5", 2 },
    /* `with`: names resolve through the object, not the table. Eligible:
       `o`, the hidden object slot, top level. */
    { "function w(o){with(o){return p}}r=w({p:4})", "4", 3 },
    /* Indirect eval runs in global scope and reads no table. Eligible:
       `a` + top level. */
    { "function ie(a){return (0,eval)('1')+a}r=ie(2)", "3", 2 },
    /* Class with fields and a private field: the constructor, the field
       initializer and the method are eligible (their hidden `this` slots
       and the like: 4 entries). The top level keeps its table, since the
       class's functions capture its class bindings. */
    { "var C=class{#p=1;f=2;get(){return this.#p+this.f}};r=new C().get()",
      "3", 4 },
    /* Default argument (non-simple parameter list, so a separate
       parameter scope): `a`, `b`, `c` and the compiler's own slots, 5 in
       all, + top level. Eligible. */
    { "function d(a,b=a+1){let c=b;return c}r=d(1)", "2", 6 },
};

static bool run_shape(Engine *engine, const uint8_t *bytes, size_t size,
                      bool rom, const char *expected, int64_t *func_size)
{
    JSMemoryUsage before, after;
    JS_RunGC(engine->rt);
    JS_ComputeMemoryUsage(engine->rt, &before);
    JSValue program = JS_ReadObject(engine->ctx, bytes, size,
        JS_READ_OBJ_BYTECODE | (rom ? JS_READ_OBJ_ROM_DATA : 0));
    CHECK(!JS_IsException(program));
    JS_ComputeMemoryUsage(engine->rt, &after);
    *func_size = after.js_func_size - before.js_func_size;
    JSValue value = JS_EvalFunction(engine->ctx, program);
    if (JS_IsException(value))
        return result_is(engine->ctx, value, expected);
    JS_FreeValue(engine->ctx, value);
    JSContext *job_ctx;
    while (JS_ExecutePendingJob(engine->rt, &job_ctx) > 0) {}
    return result_is(engine->ctx,
        JS_Eval(engine->ctx, "String(r)", 9, "<result>", JS_EVAL_TYPE_GLOBAL),
        expected);
}

static bool run_shape_checks(void)
{
    Engine engine;
    CHECK(engine_open(&engine, JS_STRIP_SOURCE | JS_STRIP_DEBUG));
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const Shape *shape = &shapes[i];
        size_t plain_size = 0, stripped_size = 0;
        uint8_t *plain = compile_and_write(&engine, shape->source,
            strlen(shape->source), 0, &plain_size);
        uint8_t *stripped = compile_and_write(&engine, shape->source,
            strlen(shape->source), JS_WRITE_OBJ_STRIP_UNUSED_LOCALS,
            &stripped_size);
        CHECK(plain && stripped);
        for (int rom = 0; rom <= 1; rom++) {
            int64_t plain_func = 0, stripped_func = 0;
            CHECK(run_shape(&engine, plain, plain_size, rom,
                            shape->expected, &plain_func));
            CHECK(run_shape(&engine, stripped, stripped_size, rom,
                            shape->expected, &stripped_func));
            if (plain_func - stripped_func != 12 * shape->entries) {
                fprintf(stderr, "unused locals: shape %zu dropped %lld "
                        "bytes, wanted %d entries\n", i,
                        (long long) (plain_func - stripped_func),
                        shape->entries);
                return false;
            }
        }
        js_free(engine.ctx, plain);
        js_free(engine.ctx, stripped);
    }
    CHECK(engine_close(&engine));
    return true;
}

/* Position of the first byte at which two blobs differ. */
static size_t first_difference(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    while (i < n && a[i] == b[i]) i++;
    return i;
}

/* Start of the LEB128 field whose last byte is at `end`. */
static size_t leb128_start(const uint8_t *bytes, size_t end)
{
    while (end > 0 && (bytes[end - 1] & 0x80)) end--;
    return end;
}

static bool refused_as_invalid_variables(Engine *engine, const uint8_t *bytes,
                                         size_t size)
{
    JSValue program = JS_ReadObject(engine->ctx, bytes, size,
                                    JS_READ_OBJ_BYTECODE);
    if (!JS_IsException(program)) {
        JS_FreeValue(engine->ctx, program);
        fprintf(stderr, "unused locals: inconsistent record was read\n");
        return false;
    }
    JSValue error = JS_GetException(engine->ctx);
    const char *text = JS_ToCString(engine->ctx, error);
    bool okay = text && strstr(text, "invalid function variables") != NULL;
    if (!okay)
        fprintf(stderr, "unused locals: refused with %s\n", text ? text : "?");
    JS_FreeCString(engine->ctx, text);
    JS_FreeValue(engine->ctx, error);
    return okay;
}

/* The reader refuses a function record whose table does not cover its
   arguments and locals, or is absent while some local is captured. */
static bool run_reader_checks(void)
{
    static const char source[] = "function f(a){let x=a+1;return x}r=f(1)";
    Engine engine;
    CHECK(engine_open(&engine, JS_STRIP_SOURCE | JS_STRIP_DEBUG));
    size_t plain_size = 0, stripped_size = 0;
    uint8_t *plain = compile_and_write(&engine, source, sizeof(source) - 1,
                                       0, &plain_size);
    uint8_t *stripped = compile_and_write(&engine, source, sizeof(source) - 1,
        JS_WRITE_OBJ_STRIP_UNUSED_LOCALS, &stripped_size);
    CHECK(plain && stripped && stripped_size < plain_size);
    /* The blobs first differ at the top-level function's table length:
       1 (its completion value) against 0. Before it come byte_code_len,
       cpool_count, closure_var_count and var_ref_count. */
    size_t at = first_difference(plain, stripped, stripped_size);
    CHECK(at < stripped_size && plain[at] == 1 && stripped[at] == 0);
    size_t field = at;
    for (int skip = 0; skip < 3; skip++)
        field = leb128_start(stripped, field - 1);
    size_t var_ref_count_at = field - 1;
    CHECK(stripped[var_ref_count_at] == 0);

    plain[at] = 2;                  /* table longer than its slots */
    CHECK(refused_as_invalid_variables(&engine, plain, plain_size));
    stripped[var_ref_count_at] = 1; /* captured local, no table */
    CHECK(refused_as_invalid_variables(&engine, stripped, stripped_size));
    /* Unmodified, both read. */
    plain[at] = 1;
    stripped[var_ref_count_at] = 0;
    for (int which = 0; which < 2; which++) {
        int64_t func_size = 0;
        CHECK(run_shape(&engine, which ? stripped : plain,
                        which ? stripped_size : plain_size, false, "2",
                        &func_size));
    }
    js_free(engine.ctx, plain);
    js_free(engine.ctx, stripped);
    CHECK(engine_close(&engine));
    return true;
}

/* The shipping bootstrap is written with the flag. Its closures that
   capture locals (event listener registries, observers, timers, promise
   jobs) and its errors must behave as before. */
static bool run_realm_checks(void)
{
    static const char page_html[] =
        "<!doctype html><title>Locals</title><body><p id=p>hi</p></body>";
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    memset(&document, 0, sizeof(document));
    CHECK(document_parse(&document, &budget, page_html,
                         sizeof(page_html) - 1u, 512u));
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .omit_test_probes = true,
    };
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 5u * MIB, 8000, "https://locals.test/",
        &options, &result);
    CHECK(runtime != NULL && result.success);

    memset(&result, 0, sizeof(result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "const log=[];"
        "function thrown(f){try{f();return 'none'}"
        "catch(e){return e.name+':'+e.message+':'+(e instanceof Error)}}"
        /* Bootstrap argument and receiver checks throw from bootstrap
           functions. */
        "log.push(thrown(()=>document.createElement('')));"
        "log.push(thrown(()=>new Event()));"
        "log.push(thrown(()=>Element.prototype.getAttribute.call({},'x')));"
        "log.push(thrown(()=>document.querySelector('[')));"
        /* The stack still names the bootstrap functions it passed. */
        "try{document.createElement('')}catch(e){log.push("
        "/at DOMException\\n[^]*at createElement\\n/.test(e.stack))}"
        /* Listener registries and the dispatch closures capture state. */
        "const p=document.getElementById('p');let hits=0;"
        "const once=()=>hits++;"
        "p.addEventListener('ping',once,{once:true});"
        "p.addEventListener('ping',()=>hits+=10);"
        "p.dispatchEvent(new Event('ping'));"
        "p.dispatchEvent(new Event('ping'));"
        "log.push(String(hits));"
        "const ac=new AbortController();let aborted='';"
        "ac.signal.addEventListener('abort',()=>aborted=ac.signal.reason.name);"
        "ac.abort();log.push(aborted);"
        "const u=new URLSearchParams('a=1&b=2');u.append('c','3');"
        "log.push(u.toString());"
        "const m=new Map();for(const [k,v] of u)m.set(k,v);"
        "log.push([...m.keys()].join(''));"
        "globalThis.pocSummary=log.join('|')",
        "<unused-locals-realm>", &result));
    printf("unused locals: realm %s\n", result.summary);
    CHECK(strcmp(result.summary,
                 "InvalidCharacterError:Invalid name: :true|"
                 "TypeError:Failed to construct 'Event': 1 argument required,"
                 " but only 0 present.:true|"
                 "TypeError:Illegal invocation:true|"
                 "SyntaxError:Invalid selector:true|true|"
                 "21|AbortError|a=1&b=2&c=3|abc") == 0);

    /* Promise jobs and timers resume bootstrap closures later. */
    memset(&result, 0, sizeof(result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.later=[];"
        "queueMicrotask(()=>later.push('micro'));"
        "setTimeout(()=>later.push('timer'),0);"
        "Promise.resolve(4).then(v=>later.push('then'+v));"
        "new MutationObserver(r=>later.push('mutation'+r.length))"
        ".observe(document.body,{childList:true});"
        "document.body.appendChild(document.createElement('i'));"
        "globalThis.pocSummary='queued'",
        "<unused-locals-queue>", &result));
    CHECK(strcmp(result.summary, "queued") == 0);
    for (unsigned turn = 0; turn < 4; turn++)
        CHECK(script_runtime_advance(runtime, 16, 16, &result));
    memset(&result, 0, sizeof(result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, "globalThis.pocSummary=later.slice().sort().join(',')",
        "<unused-locals-later>", &result));
    printf("unused locals: deferred %s\n", result.summary);
    CHECK(strcmp(result.summary, "micro,mutation1,then4,timer") == 0);

    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    return true;
}

int main(void)
{
    if (!run_engine_checks() || !run_shape_checks() || !run_reader_checks()
        || !run_realm_checks())
        return 1;
    puts("QuickJS unused variable tables: PASS");
    return 0;
}
