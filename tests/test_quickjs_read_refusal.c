#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include <cutils.h>
#include <libunicode.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A refused Unicode range allocation must return, including the temporary
   ranges used by Script_Extensions, rather than re-enter its cleanup label. */
static void *refuse_range(void *opaque, void *pointer, size_t size)
{
    (void) opaque;
    if (size == 0) free(pointer);
    return NULL;
}

static int unicode_refusal(void)
{
    for (int extensions = 0; extensions <= 1; extensions++) {
        CharRange range;
        cr_init(&range, NULL, refuse_range);
        int result = unicode_script(&range, "Latin", extensions);
        cr_free(&range);
        if (result != -1) return 1;
    }
    puts("Unicode script allocation refusal: PASS");
    return 0;
}

/* Generate valid bytecode with the real writer, then truncate every prefix.
   BigInts allocated before a truncated limb must be released immediately,
   not merely hidden in the allocator's still-live arena. */
static int bigint_truncation(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    JSContext *context = runtime ? JS_NewContext(runtime) : NULL;
    bool okay = context != NULL;
    if (context != NULL) {
        const char source[] = "123456789012345678901234567890123456789n";
        JSValue integer = JS_Eval(context, source, sizeof(source) - 1u,
                                 "<bigint-truncation>", JS_EVAL_TYPE_GLOBAL);
        size_t length = 0;
        uint8_t *bytes = JS_IsException(integer) ? NULL
            : JS_WriteObject(context, &length, integer, JS_WRITE_OBJ_BYTECODE);
        okay = bytes != NULL && length > 4u && length < 256u;
        if (okay) {
            /* Materialize exception metadata before taking the baseline. */
            JSValue warm = JS_ReadObject(context, bytes, 0, JS_READ_OBJ_BYTECODE);
            JS_FreeValue(context, warm);
            JS_FreeValue(context, JS_GetException(context));
            JS_RunGC(runtime);
            JSMemoryUsage before, after;
            JS_ComputeMemoryUsage(runtime, &before);
            for (size_t cut = 0; cut < length; cut++) {
                JSValue partial = JS_ReadObject(context, bytes, cut,
                                                JS_READ_OBJ_BYTECODE);
                okay = JS_IsException(partial) && okay;
                JS_FreeValue(context, partial);
                JS_FreeValue(context, JS_GetException(context));
            }
            JS_RunGC(runtime);
            JS_ComputeMemoryUsage(runtime, &after);
            if (after.malloc_arena_used != before.malloc_arena_used) {
                fprintf(stderr, "truncated BigInt retained %lld bytes\n",
                    (long long) (after.malloc_arena_used - before.malloc_arena_used));
                okay = false;
            }
            JSValue complete = JS_ReadObject(context, bytes, length,
                                             JS_READ_OBJ_BYTECODE);
            const char *actual = JS_ToCString(context, complete);
            okay = actual != NULL
                && strcmp(actual, "123456789012345678901234567890123456789") == 0
                && okay;
            JS_FreeCString(context, actual);
            JS_FreeValue(context, complete);
        }
        js_free(context, bytes);
        JS_FreeValue(context, integer);
        JS_FreeContext(context);
    }
    if (runtime != NULL) JS_FreeRuntime(runtime);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    if (okay) puts("Truncated BigInt bytecode ownership: PASS");
    return okay ? 0 : 1;
}

/* Coercion temporarily owns even a built-in symbol. Constant atoms ignore
   JS_FreeAtom, so transferring that value directly leaks its reference. */
static int symbol_key_ownership(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *context = runtime ? JS_NewContext(runtime) : NULL;
    bool okay = context != NULL;
    const char *cases[] = {
        "({key:Symbol.iterator,[Symbol.toPrimitive](){return this.key;}})",
        "({key:Symbol('key'),[Symbol.toPrimitive](){return this.key;}})"
    };
    for (size_t i = 0; context && i < sizeof(cases) / sizeof(cases[0]); i++) {
        JSValue object = JS_Eval(context, cases[i], strlen(cases[i]),
                                 "<symbol-key-ownership>", JS_EVAL_TYPE_GLOBAL);
        JSValue key = JS_GetPropertyStr(context, object, "key");
        if (JS_VALUE_GET_TAG(key) != JS_TAG_SYMBOL) okay = false;
        else {
            JSRefCountHeader *header = __js_rc(JS_VALUE_GET_PTR(key));
            int before = header->ref_count;
            for (unsigned n = 0; n < 100u; n++) {
                JSAtom atom = JS_ValueToAtom(context, object);
                okay = atom != JS_ATOM_NULL && okay;
                JS_FreeAtom(context, atom);
                atom = JS_ValueToAtom(context, key);
                okay = atom != JS_ATOM_NULL && okay;
                JS_FreeAtom(context, atom);
            }
            if (header->ref_count != before) {
                fprintf(stderr, "symbol coercion retained %d references\n",
                        header->ref_count - before);
                okay = false;
                /* Keep a failing run's teardown valid as well. */
                while (header->ref_count > before)
                    JS_FreeValue(context, key);
            }
        }
        JS_FreeValue(context, key);
        JS_FreeValue(context, object);
    }
    if (context) JS_FreeContext(context);
    if (runtime) JS_FreeRuntime(runtime);
    if (okay) puts("Symbol key coercion ownership: PASS");
    return okay ? 0 : 1;
}

static int captured_reference_limit(void)
{
    Budget budget;
    budget_init(&budget, 64u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    JSRuntime *runtime = pool
        ? JS_NewRuntime2(budget_quickjs_pool_allocator(), pool) : NULL;
    JSContext *context = runtime ? JS_NewContext(runtime) : NULL;
    bool okay = context != NULL;
    char *source = malloc(1024u * 1024u);
    okay = source != NULL && okay;
    for (unsigned mode = 0; okay && mode < 3u; mode++) {
        unsigned arguments = mode == 2u ? 65533u : 32768u;
        unsigned locals = mode == 2u ? 2u : 32768u;
        size_t used = (size_t) snprintf(source, 1024u * 1024u, "(function(");
        for (unsigned i = 0; i < arguments; i++)
            used += (size_t) snprintf(source + used, 1024u * 1024u - used,
                                     "%sa%u", i ? "," : "", i);
        used += (size_t) snprintf(source + used, 1024u * 1024u - used,
                                  "){%s ", mode == 1u ? "let" : "var");
        for (unsigned i = 0; i < locals; i++)
            used += (size_t) snprintf(source + used, 1024u * 1024u - used,
                                     "%sv%u", i ? "," : "", i);
        used += (size_t) snprintf(source + used, 1024u * 1024u - used, "%s",
            mode == 2u ? ";function inner(){return v0+v1;}return arguments;})"
                       : ";return eval('v0');})");
        JSValue function = JS_Eval(context, source, used, "<capture-limit>",
            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        bool refused = JS_IsException(function);
        JS_FreeValue(context, function);
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        okay = refused && message && strstr(message, "variable references") != NULL;
        if (!okay) fprintf(stderr, "captured-reference overflow was not refused correctly (mode %u)\n", mode);
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
    }
    if (context) {
        const char control[] = "(function(a){let b=2;return eval('a+b')})(40)";
        JSValue value = JS_Eval(context, control, sizeof(control) - 1u,
                                "<capture-control>", JS_EVAL_TYPE_GLOBAL);
        int32_t result = 0;
        okay = JS_ToInt32(context, &result, value) == 0 && result == 42 && okay;
        JS_FreeValue(context, value);
        JS_FreeContext(context);
    }
    free(source);
    if (runtime) JS_FreeRuntime(runtime);
    if (pool) okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    if (okay) puts("Captured-variable index limit and valid eval: PASS");
    return okay ? 0 : 1;
}

static int promise_publication_refusal(void)
{
    unsigned refusals = 0, successes = 0;
    for (unsigned nth = 1; nth <= 64; nth++) {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
        JSRuntime *rt = pool ? JS_NewRuntime2(budget_quickjs_pool_allocator(), pool) : NULL;
        JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
        if (!ctx) return 1;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, "Promise");
        JSValue factory = JS_GetPropertyStr(ctx, ctor, "withResolvers");
        JSValue warm = JS_Call(ctx, factory, ctor, 0, NULL);
        bool okay = !JS_IsException(warm);
        JS_FreeValue(ctx, warm);
        JS_RunGC(rt);
        JS_TestRefuseAllocation(nth);
        JSValue record = JS_Call(ctx, factory, ctor, 0, NULL);
        JS_TestRefuseAllocation(0);
        if (JS_IsException(record)) {
            refusals++;
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            successes++;
            JSValue promise = JS_GetPropertyStr(ctx, record, "promise");
            JSValue resolve = JS_GetPropertyStr(ctx, record, "resolve");
            JSValue reject = JS_GetPropertyStr(ctx, record, "reject");
            okay = okay && JS_IsObject(promise) && JS_IsFunction(ctx, resolve)
                && JS_IsFunction(ctx, reject) && !JS_HasException(ctx);
            JS_FreeValue(ctx, record);
            record = JS_UNDEFINED;
            JS_RunGC(rt);
            JSValue answer = JS_NewInt32(ctx, 42);
            JSValue called = JS_Call(ctx, nth & 1u ? resolve : reject,
                                      JS_UNDEFINED, 1, &answer);
            JSValue settled = JS_PromiseResult(ctx, promise);
            int32_t number = 0;
            okay = okay && !JS_IsException(called)
                && JS_PromiseState(ctx, promise)
                    == (nth & 1u ? JS_PROMISE_FULFILLED : JS_PROMISE_REJECTED)
                && JS_ToInt32(ctx, &number, settled) == 0 && number == 42;
            JS_FreeValue(ctx, settled);
            JS_FreeValue(ctx, called);
            JS_FreeValue(ctx, reject);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, promise);
        }
        JS_FreeValue(ctx, record);
        JS_FreeValue(ctx, factory);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
        if (!okay) {
            fprintf(stderr, "promise publication refusal failed at allocation %u\n", nth);
            return 1;
        }
    }
    if (!refusals || !successes) return 1;
    puts("Promise publication ownership under refusal: PASS");
    return 0;
}

static int append_publication_refusal(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    JSRuntime *rt = pool ? JS_NewRuntime2(budget_quickjs_pool_allocator(), pool) : NULL;
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) return 1;
    const char *initial[] = {"'a'", "128", "-1", "1000", "1.5", "128", "128"};
    const char *appended[] = {"'b'", "129", "-2", "1001", "2.5", "1000", "2.5"};
    bool okay = true;
    unsigned failures[7] = {0};
    for (unsigned kind = 0; okay && kind < 7; kind++) {
        JSValue seed = JS_Eval(ctx, initial[kind], strlen(initial[kind]), "<append-seed>", 0);
        JSValue value = JS_Eval(ctx, appended[kind], strlen(appended[kind]), "<append-value>", 0);
        uint32_t samples = kind == 3 ? 160u : 48u;
        for (uint32_t sample = 0; okay && sample < samples; sample++) {
            /* Short backing is selected by the bounded repacker once the
               initial ordinary numeric array reaches its checkpoint. The
               range also crosses growth when compact storage is disabled. */
            uint32_t length = kind == 3 ? 385u + sample : sample;
            for (unsigned nth = 1; okay && nth <= 5; nth++) {
                JSValue array = JS_NewArray(ctx);
                for (uint32_t i = 0; okay && i < length; i++)
                    okay = JS_SetPropertyUint32(ctx, array, i, JS_DupValue(ctx, seed)) >= 0;
                JS_TestRefuseAllocation(nth);
                int result = JS_SetPropertyUint32(ctx, array, length, JS_DupValue(ctx, value));
                JS_TestRefuseAllocation(0);
                if (result < 0) {
                    failures[kind]++;
                    JS_FreeValue(ctx, JS_GetException(ctx));
                }
                JSValue size = JS_GetPropertyStr(ctx, array, "length");
                uint32_t actual = 0;
                okay = okay && JS_ToUint32(ctx, &actual, size) == 0
                    && actual == length + (result >= 0);
                JS_FreeValue(ctx, size);
                if (result >= 0) {
                    JSValue last = JS_GetPropertyUint32(ctx, array, length);
                    const char *got = JS_ToCString(ctx, last), *want = JS_ToCString(ctx, value);
                    okay = okay && got && want && strcmp(got, want) == 0;
                    JS_FreeCString(ctx, got);
                    JS_FreeCString(ctx, want);
                    JS_FreeValue(ctx, last);
                }
                if (length) {
                    JSValue last = JS_GetPropertyUint32(ctx, array, length - 1u);
                    const char *got = JS_ToCString(ctx, last), *want = JS_ToCString(ctx, seed);
                    okay = okay && got && want && strcmp(got, want) == 0;
                    JS_FreeCString(ctx, got);
                    JS_FreeCString(ctx, want);
                    JS_FreeValue(ctx, last);
                }
                JS_FreeValue(ctx, array);
                if (!okay) fprintf(stderr, "append publication failed kind=%u length=%u allocation=%u\n", kind, length, nth);
            }
        }
        okay = okay && failures[kind] != 0;
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, seed);
        JS_RunGC(rt);
    }
    const char control[] = "(()=>{const a=[1];Object.defineProperty(a,'length',{writable:false});"
        "try{a.push(2);return false}catch(e){return a.length===1&&a[0]===1}})()";
    JSValue result = JS_Eval(ctx, control, sizeof(control) - 1u, "<append-control>", 0);
    okay = okay && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    if (okay) puts("Array append publication under refusal: PASS");
    return okay ? 0 : 1;
}

typedef struct { BudgetQuickJSPool *pool; unsigned oversized; } GuardAllocator;
static void *guard_malloc(JSMallocState *s, size_t size)
{
    GuardAllocator *guard = s->opaque;
    if (size > UINT32_MAX) { guard->oversized++; return NULL; }
    JSMallocState inner = *s;
    inner.opaque = guard->pool;
    void *result = budget_quickjs_pool_allocator()->js_malloc(&inner, size);
    *s = inner; s->opaque = guard;
    return result;
}
static void guard_free(JSMallocState *s, void *ptr)
{
    GuardAllocator *guard = s->opaque;
    JSMallocState inner = *s;
    inner.opaque = guard->pool;
    budget_quickjs_pool_allocator()->js_free(&inner, ptr);
    *s = inner; s->opaque = guard;
}
static void *guard_realloc(JSMallocState *s, void *ptr, size_t size)
{
    GuardAllocator *guard = s->opaque;
    if (size > UINT32_MAX) { guard->oversized++; return NULL; }
    JSMallocState inner = *s;
    inner.opaque = guard->pool;
    void *result = budget_quickjs_pool_allocator()->js_realloc(&inner, ptr, size);
    *s = inner; s->opaque = guard;
    return result;
}
static int ownkeys_allocation_bounds(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    GuardAllocator guard = {budget_quickjs_pool_create(&budget), 0};
    JSMallocFunctions allocator = {guard_malloc, guard_free, guard_realloc,
        budget_quickjs_pool_allocator()->js_malloc_usable_size};
    JSRuntime *rt = guard.pool ? JS_NewRuntime2(&allocator, &guard) : NULL;
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) return 1;
    const uint64_t lengths[] = {UINT32_MAX / sizeof(JSPropertyEnum) + UINT64_C(1),
                               INT32_MAX, UINT64_C(4294967297)};
    bool okay = true;
    for (unsigned i = 0; okay && i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        char source[384];
        snprintf(source, sizeof(source), "(()=>{let reads=0;const x=new Proxy({},"
            "{ownKeys(){return {length:%.0f,get 0(){reads++;return 'a'}}}});"
            "try{Reflect.ownKeys(x);return false}catch(e){return reads===0&&"
            "e instanceof InternalError&&e.message==='out of memory'}})()", (double) lengths[i]);
        JSValue result = JS_Eval(ctx, source, strlen(source), "<ownkeys-bounds>", 0);
        okay = JS_ToBool(ctx, result) == 1 && guard.oversized == 0;
        JS_FreeValue(ctx, result);
        if (!okay) fprintf(stderr, "ownKeys allocation guard failed at case %u\n", i);
    }
    const char control[] = "(()=>{let reads=0;const x=new Proxy({}, {ownKeys(){return {"
        "get length(){reads++;return 2},0:'a',1:'b'}}});"
        "if(Reflect.ownKeys(x).join(',')!=='a,b'||reads!==1)return false;"
        "const sentinel={};try{Reflect.ownKeys(new Proxy({}, {ownKeys(){return {"
        "get length(){throw sentinel}}}}));return false}catch(e){return e===sentinel}})()";
    JSValue result = JS_Eval(ctx, control, sizeof(control) - 1u, "<ownkeys-control>", 0);
    okay = okay && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    okay = budget_quickjs_pool_destroy(guard.pool) && budget.current == 0 && okay;
    if (okay) puts("Proxy ownKeys portable allocation bounds: PASS");
    return okay ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--bigint-only") == 0)
        return bigint_truncation();
    if (argc == 2 && strcmp(argv[1], "--unicode-only") == 0)
        return unicode_refusal();
    if (argc == 2 && strcmp(argv[1], "--symbol-only") == 0)
        return symbol_key_ownership();
    if (argc == 2 && strcmp(argv[1], "--capture-only") == 0)
        return captured_reference_limit();
    if (argc == 2 && strcmp(argv[1], "--promise-only") == 0)
        return promise_publication_refusal();
    if (argc == 2 && strcmp(argv[1], "--append-only") == 0)
        return append_publication_refusal();
    if (argc == 2 && strcmp(argv[1], "--ownkeys-only") == 0)
        return ownkeys_allocation_bounds();
    return unicode_refusal() || bigint_truncation()
        || symbol_key_ownership() || captured_reference_limit()
        || promise_publication_refusal() || append_publication_refusal()
        || ownkeys_allocation_bounds();
}
